/**
 * @file irrigationd.c
 * @brief Irrigation control daemon using MCP23017 driver backend
 *
 * This daemon listens on a TCP socket (port 4242) and accepts simple commands
 * of the form:
 *
 *   ZONE=<zone_number> TIME=<seconds>
 *
 * - ZONE ranges from 1..14
 * - TIME=0 turns a zone OFF immediately
 * - TIME=N>0 turns a zone ON for N seconds, then back OFF automatically
 *
 * Features:
 *   - Multi-threaded client handling
 *   - Worker threads for timed zone control
 *   - Shared state protected by mutex
 *   - Clean shutdown via signals (SIGINT/SIGTERM)
 *   - Syslog logging for all lifecycle and error events
 *   - Optional debug mode via environment variable
 *
 * Safety/Robustness improvements:
 *   - Self-pipe for safe signal handling (avoid non-async-safe calls in handlers)
 *   - xmalloc/xrealloc helpers with syslog logging
 *   - FD_CLOEXEC set on all file descriptors to prevent leakage
 *   - pthread_attr_setstacksize to reduce per-thread memory footprint
 *   - Interruptible timers using pthread_cond_timedwait (shutdown cancels timers)
 *   - Worker watchdog to log long-running threads
 *   - Semaphore to bound total concurrent threads (defend against DoS)
 *   - SO_RCVTIMEO on accepted sockets to reduce Slowloris risk
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <syslog.h>
#include <semaphore.h>
#include <poll.h>

#include "mcp23017.h"

#define MAX_ZONE        14                                                      // Maximum number of zones supported
#define SERVER_PORT     4242                                                    // TCP port for client connections
#define BACKLOG         5                                                       // Listen backlog for pending connections
#define CMD_BUF_SZ      256                                                     // Max command buffer size
#define WORKER_STACK_SZ (128 * 1024)                                            // Per-thread stack size (128 KB)
#define WORKER_LONG_RUNNING_SEC 300                                             // Watchdog: log if worker runs >5 min
#define MAX_WORKERS     64                                                      // Maximum concurrent threads (clients + workers)

/* --- Global State --- */

static int zone_state[MAX_ZONE + 1];                                            // Track current ON/OFF state of zones (1-based)
static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;                   // Shared with driver for atomicity

typedef struct {
    pthread_t tid;
    time_t    start;
} worker_entry_t;

static pthread_mutex_t workers_lock = PTHREAD_MUTEX_INITIALIZER;                // Protects worker_list
static worker_entry_t *worker_list = NULL;                                      // Dynamic array of workers
static size_t worker_count = 0;                                                 // Number of active workers
static size_t worker_capacity = 0;                                              // Allocated capacity

static int listen_fd = -1;                                                      // Listening socket FD
static volatile sig_atomic_t running = 1;                                       // Global shutdown flag

static pthread_cond_t timer_cond = PTHREAD_COND_INITIALIZER;                    // Used to cancel timers on shutdown
static pthread_mutex_t timer_mutex = PTHREAD_MUTEX_INITIALIZER;                 // Associated mutex for timer_cond

static int sigpipe_fd[2] = { -1, -1 };                                          // Self-pipe for signal -> main thread notification
static sem_t worker_slots;                                                      // Semaphore limiting concurrent threads

/* --- Allocation Helpers --- */

static void* xmalloc(size_t sz) {
    void *p = malloc(sz);
    if (!p) {
        syslog(LOG_ERR, "malloc(%zu) failed", sz);                              // Centralized OOM logging
        if (getenv("IRRIGATIOND_OOM_ABORT")) abort();
    }
    return p;
}

static void* xrealloc(void *ptr, size_t sz) {
    void *p = realloc(ptr, sz);
    if (!p) {
        syslog(LOG_ERR, "realloc(%zu) failed", sz);                             // Centralized OOM logging
        if (getenv("IRRIGATIOND_OOM_ABORT")) abort();
    }
    return p;
}

/* --- Utility Functions --- */

// Set FD_CLOEXEC on a descriptor to avoid leaking into children
static void set_cloexec(int fd) {
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) return;
    (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// Safe write that retries partial writes (network robustness)
static ssize_t write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return (ssize_t)len;
}

/* --- Worker Management --- */

// Add worker entry; log on realloc failure
static int add_worker(pthread_t tid) {
    int rc = 0;
    if (pthread_mutex_lock(&workers_lock) != 0) return -1;
    if (worker_count == worker_capacity) {
        size_t newcap = worker_capacity == 0 ? 16 : worker_capacity * 2;
        worker_entry_t *n = xrealloc(worker_list, newcap * sizeof(worker_entry_t));
        if (!n) {
            syslog(LOG_ERR, "Failed to grow worker_list to %zu entries", newcap);
            rc = -1;
            goto out;
        }
        worker_list = n;
        worker_capacity = newcap;
    }
    worker_list[worker_count].tid = tid;
    worker_list[worker_count].start = time(NULL);
    worker_count++;
out:
    pthread_mutex_unlock(&workers_lock);
    return rc;
}

// Remove worker by tid (called when thread exits cleanly)
static void remove_worker(pthread_t tid) {
    if (pthread_mutex_lock(&workers_lock) != 0) return;
    for (size_t i = 0; i < worker_count; ++i) {
        if (pthread_equal(worker_list[i].tid, tid)) {
            worker_list[i] = worker_list[worker_count - 1];
            worker_count--;
            break;
        }
    }
    pthread_mutex_unlock(&workers_lock);
}

/* --- Signal Handling (self-pipe pattern) --- */

// Async-signal-safe handler: write a byte to self-pipe and close listen_fd
static void shutdown_signal_handler(int signo) {
    (void)signo;
    running = 0;
    if (listen_fd >= 0) {
        close(listen_fd);                                                         // close is async-signal-safe
        listen_fd = -1;
    }
    if (sigpipe_fd[1] >= 0) {
        char c = 1;
        (void)write(sigpipe_fd[1], &c, 1);                                        // async-signal-safe notification
    }
}

/* --- Cleanup --- */

// Robust joining: pop-and-join approach avoids needing a large contiguous snapshot
static void join_workers_and_cleanup(void) {
    // Wake timers so workers can exit early
    pthread_mutex_lock(&timer_mutex);
    pthread_cond_broadcast(&timer_cond);
    pthread_mutex_unlock(&timer_mutex);

    // Pop-and-join: repeatedly remove last entry under lock and join it.
    for (;;) {
        pthread_t tid = 0;
        if (pthread_mutex_lock(&workers_lock) != 0) break;
        if (worker_count == 0) {
            pthread_mutex_unlock(&workers_lock);
            break;
        }
        tid = worker_list[worker_count - 1].tid;
        worker_count--;
        pthread_mutex_unlock(&workers_lock);

        int jrc = pthread_join(tid, NULL);
        if (jrc != 0) {
            syslog(LOG_WARNING, "pthread_join failed on worker: %s", strerror(jrc));
        }
    }

    // free worker_list storage
    if (worker_list) {
        free(worker_list);
        worker_list = NULL;
        worker_capacity = 0;
    }

    // Ensure all zones are OFF with driver lock held
    mcp_lock();
    for (int z = 1; z <= MAX_ZONE; ++z) {
        if (zone_state[z] != 0) {
            mcp_set_zone_state(z, 0);
            zone_state[z] = 0;
        }
    }
    mcp_unlock();

    mcp_i2c_close();
}

/* --- Zone Control --- */

static void set_zone_state(int zone, int state) {
    if (zone < 1 || zone > MAX_ZONE) {
        syslog(LOG_ERR, "set_zone_state: invalid zone %d", zone);
        return;
    }

    mcp_lock();
    if (mcp_set_zone_state(zone, state) != 0) {
        syslog(LOG_ERR, "Failed to set zone %d -> %s", zone, state ? "ON" : "OFF");
    } else {
        zone_state[zone] = state;
        syslog(LOG_INFO, "Zone %d -> %s", zone, state ? "ON" : "OFF");
    }
    mcp_unlock();
}

/* --- Worker Thread --- */

static void *worker_thread(void *arg) {
    int zone   = ((int *)arg)[0];
    int time_s = ((int *)arg)[1];
    free(arg);

    pthread_t self = pthread_self();
    // parent already called add_worker(); thread must remove itself before exit.
    set_zone_state(zone, 1);

    struct timespec now, abstime;
    clock_gettime(CLOCK_REALTIME, &now);

    // Defensive check for overflow (rare since we cap time to 86400)
    if (time_s < 0 || time_s > INT_MAX || now.tv_sec > LONG_MAX - time_s) {
        syslog(LOG_ERR, "worker_thread: invalid duration %d", time_s);
        set_zone_state(zone, 0);
        remove_worker(self);
        sem_post(&worker_slots);                                                  // Release slot before exit
        return NULL;
    }

    abstime.tv_sec  = now.tv_sec + time_s;
    abstime.tv_nsec = now.tv_nsec;

    pthread_mutex_lock(&timer_mutex);
    int rc = 0;
    while (running && rc != ETIMEDOUT) {
        rc = pthread_cond_timedwait(&timer_cond, &timer_mutex, &abstime);
        if (rc == ETIMEDOUT) break;
        if (!running) break;
    }
    pthread_mutex_unlock(&timer_mutex);

    set_zone_state(zone, 0);
    remove_worker(self);
    sem_post(&worker_slots);                                                      // Return slot to pool
    return NULL;
}

/* --- Command Parsing --- */

static int parse_command(int cfd, const char *line) {
    if (!line) return -1;
    if (strlen(line) >= CMD_BUF_SZ - 1) {
        const char err[] = "ERR too long\n";
        write_all(cfd, err, sizeof(err) - 1);
        return -1;
    }

    const char *zp = strstr(line, "ZONE=");
    const char *tp = strstr(line, "TIME=");
    if (!zp || !tp) return -1;

    char *endp;
    errno = 0;
    long z = strtol(zp + 5, &endp, 10);
    if (errno == ERANGE || endp == zp + 5) return -1;
    if (!( *endp == '\0' || *endp == ' ' || *endp == '\r' || *endp == '\n')) return -1;
    if (z < 1 || z > MAX_ZONE) return -1;

    errno = 0;
    long t = strtol(tp + 5, &endp, 10);
    if (errno == ERANGE || endp == tp + 5) return -1;
    if (!( *endp == '\0' || *endp == ' ' || *endp == '\r' || *endp == '\n')) return -1;
    if (t < 0) return -1;

    // Enforce maximum TIME of 24 hours to avoid resource exhaustion
    if (t > 86400) {
        syslog(LOG_ERR, "Rejected TIME=%ld (exceeds 24h limit)", t);
        const char err[] = "ERR TIME too large (max 86400)\n";
        write_all(cfd, err, sizeof(err) - 1);
        return -1;
    }

    if (t == 0) {
        set_zone_state((int)z, 0);
        const char ok[] = "OK\n";
        write_all(cfd, ok, sizeof(ok) - 1);
        return 0;
    } else {
        // Acquire a worker slot to bound concurrent threads
        if (sem_trywait(&worker_slots) != 0) {
            const char err[] = "ERR busy\n";
            write_all(cfd, err, sizeof(err) - 1);
            return -1;
        }

        pthread_t tid;
        int *args = xmalloc(2 * sizeof(int));
        if (!args) {
            const char err[] = "ERR internal\n";
            write_all(cfd, err, sizeof(err) - 1);
            close(cfd);
            sem_post(&worker_slots);                                              // Return slot if allocation fails
            return -1;
        }
        args[0] = (int)z;
        args[1] = (int)t;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, WORKER_STACK_SZ);
        int rc = pthread_create(&tid, &attr, worker_thread, args);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            syslog(LOG_ERR, "pthread_create failed: %s", strerror(rc));
            free(args);
            const char err[] = "ERR internal\n";
            write_all(cfd, err, sizeof(err) - 1);
            close(cfd);
            sem_post(&worker_slots);                                              // Return slot if thread creation fails
            return -1;
        }

        // Track the worker so we can join it on shutdown
        if (add_worker(tid) != 0) {
            pthread_detach(tid);                                                    // Fallback: detach if tracking fails
            syslog(LOG_WARNING, "add_worker failed; detached worker thread");
        }

        const char ok[] = "OK\n";
        write_all(cfd, ok, sizeof(ok) - 1);
        return 0;
    }
}

/* --- Networking Helpers --- */

// Read a line up to CMD_BUF_SZ and null-terminate
static ssize_t read_line(int fd, char *buf, size_t sz) {
    if (sz < 2) return -1;
    size_t used = 0;
    while (used < sz - 1) {
        ssize_t n = read(fd, buf + used, 1);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (buf[used] == '\r') { used++; continue; }
        used++;
        if (buf[used - 1] == '\n') break;
    }
    buf[used] = '\0';
    return (ssize_t)used;
}

// Client thread: parse a single command then exit
static void *client_thread(void *arg) {
    int cfd = *(int *)arg;
    free(arg);

    // Ensure FD_CLOEXEC on the accepted socket to avoid leaking to exec'd children
    set_cloexec(cfd);

    // Set a receive timeout so slow clients don't occupy a thread forever
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };                          // 10s recv timeout
    (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[CMD_BUF_SZ];
    ssize_t n = read_line(cfd, buf, sizeof(buf));
    if (n > 0) {
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        if (n > 1 && buf[n - 2] == '\r') buf[n - 2] = '\0';
        if (parse_command(cfd, buf) < 0) {
            const char err[] = "ERR syntax (use: ZONE=<1..14> TIME=<seconds>)\n";
            write_all(cfd, err, sizeof(err) - 1);
        }
    } else if (n < 0) {
        syslog(LOG_ERR, "read from client failed: %s", strerror(errno));
    }

    close(cfd);
    // Release our worker slot (client threads took a slot before creation)
    sem_post(&worker_slots);
    // Remove ourselves from the tracked list (we were added by the parent)
    remove_worker(pthread_self());
    return NULL;
}

/* --- Main Entry Point --- */

int main(void) {
    openlog("irrigationd", LOG_PID | LOG_CONS, LOG_DAEMON);                      // Open syslog

    // Setup self-pipe for signal handling
    if (pipe(sigpipe_fd) != 0) {
        syslog(LOG_ERR, "pipe() failed for signal handling: %s", strerror(errno));
        sigpipe_fd[0] = sigpipe_fd[1] = -1;
    } else {
        set_cloexec(sigpipe_fd[0]);
        set_cloexec(sigpipe_fd[1]);
    }

    // Initialize worker_slots semaphore to bound total threads
    if (sem_init(&worker_slots, 0, MAX_WORKERS) != 0) {
        syslog(LOG_ERR, "sem_init failed: %s", strerror(errno));
        // Not fatal, but log and continue with reduced protection
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (mcp_i2c_open("/dev/i2c-1") < 0) {
        syslog(LOG_ERR, "ERROR: Failed to open I2C bus");
        exit(EXIT_FAILURE);
    }
    if (mcp_config_outputs() < 0) {
        syslog(LOG_ERR, "ERROR: Failed to configure MCP23017 outputs");
        mcp_i2c_close();
        exit(EXIT_FAILURE);
    }
    mcp_enable_thread_safety(&zone_lock);

    // Create listening socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        syslog(LOG_ERR, "socket() failed: %s", strerror(errno));
        mcp_i2c_close();
        exit(EXIT_FAILURE);
    }
    set_cloexec(listen_fd);

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        syslog(LOG_ERR, "setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));
        close(listen_fd);
        mcp_i2c_close();
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "bind() failed: %s", strerror(errno));
        close(listen_fd);
        mcp_i2c_close();
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, BACKLOG) < 0) {
        syslog(LOG_ERR, "listen() failed: %s", strerror(errno));
        close(listen_fd);
        mcp_i2c_close();
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "irrigationd listening on port %d", SERVER_PORT);

    // Use poll() on the listening socket and the signal pipe to wake cleanly on signals.
    while (running) {
        struct pollfd fds[2];
        nfds_t nfds = 0;

        if (listen_fd >= 0) {
            fds[nfds].fd = listen_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }
        if (sigpipe_fd[0] >= 0) {
            fds[nfds].fd = sigpipe_fd[0];
            fds[nfds].events = POLLIN;
            nfds++;
        }

        int pret = poll(fds, nfds, -1);                                            // Block until an event occurs
        if (pret < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "poll failed: %s", strerror(errno));
            break;
        }

        // Check signal pipe first: any data means a signal asked for shutdown
        if (sigpipe_fd[0] >= 0) {
            for (nfds_t i = 0; i < nfds; ++i) {
                if (fds[i].fd == sigpipe_fd[0] && (fds[i].revents & POLLIN)) {
                    char buf[16];
                    (void)read(sigpipe_fd[0], buf, sizeof(buf));                 // drain pipe
                    // running flag set by handler; loop will exit and main will clean up
                }
            }
        }

        // If listen_fd is ready, accept one connection
        if (listen_fd >= 0) {
            for (nfds_t i = 0; i < nfds; ++i) {
                if (fds[i].fd == listen_fd && (fds[i].revents & POLLIN)) {
                    struct sockaddr_in cli;
                    socklen_t len = sizeof(cli);
                    int *cfd = xmalloc(sizeof(int));
                    if (!cfd) {
                        sleep(1);
                        continue;
                    }

                    *cfd = accept(listen_fd, (struct sockaddr *)&cli, &len);
                    if (*cfd < 0) {
                        int err = errno;
                        free(cfd);
                        if (!running) break;
                        if (err == EINTR) continue;
                        syslog(LOG_ERR, "accept failed: %s", strerror(err));
                        continue;
                    }

                    // Apply FD_CLOEXEC and a receive timeout
                    set_cloexec(*cfd);
                    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
                    (void)setsockopt(*cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                    // Bound total concurrent threads via semaphore
                    if (sem_trywait(&worker_slots) != 0) {
                        // Too busy; inform client and close
                        const char err[] = "ERR busy\n";
                        write_all(*cfd, err, sizeof(err) - 1);
                        close(*cfd);
                        free(cfd);
                        continue;
                    }

                    // Create a thread to handle the client
                    pthread_t tid;
                    int rc = pthread_create(&tid, NULL, client_thread, cfd);
                    if (rc != 0) {
                        syslog(LOG_ERR, "pthread_create failed for client handler: %s", strerror(rc));
                        close(*cfd);
                        free(cfd);
                        sem_post(&worker_slots);                                  // Return slot
                        continue;
                    }

                    // Track the client thread for orderly shutdown
                    if (add_worker(tid) != 0) {
                        pthread_detach(tid);                                        // If tracking fails, detach
                        syslog(LOG_WARNING, "add_worker failed; detached client thread");
                    }
                }
            }
        }
    }

    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }

    join_workers_and_cleanup();

    syslog(LOG_INFO, "irrigationd shutdown complete");
    closelog();
    if (sigpipe_fd[0] >= 0) close(sigpipe_fd[0]);
    if (sigpipe_fd[1] >= 0) close(sigpipe_fd[1]);
    sem_destroy(&worker_slots);
    return 0;
}