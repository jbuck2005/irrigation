/**
 * @file irrigationd.c
 * @author James Buck
 * @date September 10, 2025
 * @brief Irrigation control daemon which controls a custom MCP23017 backend.
 *
 * @description
 * This file implements a daemon that listens for client connections over TCP/IP
 * and processes simple text commands to control irrigation zones connected
 * through an MCP23017 I/O expander.
 *
 * Each client sends commands of the form:
 *   ZONE=<zone_number> TIME=<seconds>
 *
 * Zones are numbered 1 through 14.  TIME=0 means "turn off immediately."
 * TIME=N>0 means "turn on the zone for N seconds, then turn it off."
 *
 * Features include:
 *   - Multi-threaded handling of client connections
 *   - Thread-safe access to shared zone state
 *   - Safe shutdown via SIGINT/SIGTERM, ensuring all zones turn off
 *   - Integration with the MCP23017 driver library
 *
 * The daemon maintains a table of active worker threads, each of which controls
 * a zone timer. A global shutdown procedure joins all worker threads and resets
 * all zones to OFF.
 */

#define _POSIX_C_SOURCE 200809L                                                 // Required for sigaction, clock_gettime, etc.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

#include "mcp23017.h"

#define MAX_ZONE        14
#define SERVER_PORT     4242
#define BACKLOG         5
#define CMD_BUF_SZ      256
#define WORKER_STACK_SZ (128 * 1024)                                              //                                                  ADDED AT COL 81 -> Per-worker stack size (128KB) to reduce memory usage per thread
#define WORKER_LONG_RUNNING_SEC 300                                               //                                                  ADDED AT COL 81 -> Warn if worker runs longer than 5 minutes

// --- Original global state preserved ---
static int zone_state[MAX_ZONE + 1];
static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;

// Worker tracking: store tid + start time so we can watch for long-running workers
typedef struct {
    pthread_t tid;
    time_t    start;
} worker_entry_t;

static pthread_mutex_t workers_lock = PTHREAD_MUTEX_INITIALIZER;
static worker_entry_t *worker_list = NULL;
static size_t worker_count = 0;
static size_t worker_capacity = 0;

static int listen_fd = -1;
static volatile sig_atomic_t running = 1;

// Cond + mutex used to implement interruptible timers for workers
static pthread_cond_t timer_cond = PTHREAD_COND_INITIALIZER;                     //                                                  ADDED AT COL 81 -> Condition variable used so workers can wake early on shutdown
static pthread_mutex_t timer_mutex = PTHREAD_MUTEX_INITIALIZER;                  //                                                  ADDED AT COL 81 -> Mutex associated with timer_cond

// Small allocation helpers to centralize OOM behavior (xmalloc/xrealloc).
// If IRRIGATIOND_OOM_ABORT is set, wrappers abort the process on allocation failure.
// Otherwise they return NULL and the caller must handle it.
static void* xmalloc(size_t sz) {
    void *p = malloc(sz);
    if (!p) {
        syslog(LOG_ERR, "malloc(%zu) failed", sz);                                //                                                  ADDED AT COL 81 -> Centralized OOM logging
        if (getenv("IRRIGATIOND_OOM_ABORT")) abort();
    }
    return p;
}

static void* xrealloc(void *ptr, size_t sz) {
    void *p = realloc(ptr, sz);
    if (!p) {
        syslog(LOG_ERR, "realloc(%zu) failed", sz);                              //                                                  ADDED AT COL 81 -> Centralized OOM logging
        if (getenv("IRRIGATIOND_OOM_ABORT")) abort();
    }
    return p;
}

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

// Add worker entry; log on realloc failure
static int add_worker(pthread_t tid) {
    int rc = 0;
    if (pthread_mutex_lock(&workers_lock) != 0) return -1;
    if (worker_count == worker_capacity) {
        size_t newcap = worker_capacity == 0 ? 16 : worker_capacity * 2;
        worker_entry_t *n = xrealloc(worker_list, newcap * sizeof(worker_entry_t));
        if (!n) {
            syslog(LOG_ERR, "Failed to grow worker_list to %zu entries", newcap); //                                                  ADDED AT COL 81 -> Log realloc failures centrally
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
// This keeps list compact so join phase can iterate exactly the remaining threads.
static void remove_worker(pthread_t tid) {
    if (pthread_mutex_lock(&workers_lock) != 0) return;
    for (size_t i = 0; i < worker_count; ++i) {
        if (pthread_equal(worker_list[i].tid, tid)) {
            // move last entry here
            worker_list[i] = worker_list[worker_count - 1];
            worker_count--;
            break;
        }
    }
    pthread_mutex_unlock(&workers_lock);
}

// Signal handler: mark shutdown and close listening socket (async-signal-safe)
static void shutdown_signal_handler(int signo) {
    (void)signo;
    running = 0;
    if (listen_fd >= 0) {
        close(listen_fd);                                                         // close is async-signal-safe
        listen_fd = -1;
    }
    // Wake all timers so workers can exit early
    (void)pthread_cond_broadcast(&timer_cond);                                   // POSIX: pthread_cond_broadcast is not async-signal-safe everywhere
                                                                                //                                                  ADDED AT COL 81 -> It's acceptable in practice on many Unix systems,
                                                                                //                                                  ADDED AT COL 81 -> but if you need strict async-signal-safety, set a flag only.
}

// Join threads and perform final hardware cleanup
static void join_workers_and_cleanup(void) {
    // Signal timers to wake immediately so workers exit quickly
    pthread_mutex_lock(&timer_mutex);
    pthread_cond_broadcast(&timer_cond);
    pthread_mutex_unlock(&timer_mutex);

    // Join workers (we copy the list under lock to avoid races)
    pthread_mutex_lock(&workers_lock);
    size_t count = worker_count;
    worker_entry_t *snapshot = NULL;
    if (count > 0) {
        snapshot = malloc(count * sizeof(worker_entry_t));
        if (snapshot) memcpy(snapshot, worker_list, count * sizeof(worker_entry_t));
    }
    pthread_mutex_unlock(&workers_lock);

    if (snapshot) {
        for (size_t i = 0; i < count; ++i) {
            int jrc = pthread_join(snapshot[i].tid, NULL);
            if (jrc != 0) {
                syslog(LOG_WARNING, "pthread_join failed on worker %zu: %s", i, strerror(jrc)); //                                                  ADDED AT COL 81 -> Log join errors
            } else {
                time_t now = time(NULL);
                time_t start = snapshot[i].start;
                if (now - start > WORKER_LONG_RUNNING_SEC) {
                    syslog(LOG_WARNING, "worker %zu ran %ld seconds", i, (long)(now - start));    //                                                  ADDED AT COL 81 -> Watchdog-style warning
                }
            }
        }
        free(snapshot);
    }

    // Turn off all zones, holding the driver lock so hw + software mirror are consistent
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

// This function updates both hardware and software mirrors atomically
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
        syslog(LOG_INFO, "Zone %d -> %s", zone, state ? "ON" : "OFF");            //                                                  ADDED AT COL 81 -> Prefer syslog for lifecycle events
    }
    mcp_unlock();
}

// Worker thread: use timed wait so shutdown can wake it early
static void *worker_thread(void *arg) {
    int zone   = ((int *)arg)[0];
    int time_s = ((int *)arg)[1];
    free(arg);

    // Register ourselves into worker list so we can be joined/monitored
    pthread_t self = pthread_self();
    if (add_worker(self) != 0) {
        // If we can't track, proceed but warn via syslog
        syslog(LOG_WARNING, "worker_thread: failed to add to tracking list");
    }

    set_zone_state(zone, 1);

    // Wait on condition for time_s seconds or until signaled (shutdown)
    struct timespec now, abstime;
    clock_gettime(CLOCK_REALTIME, &now);
    abstime.tv_sec = now.tv_sec + time_s;
    abstime.tv_nsec = now.tv_nsec;

    pthread_mutex_lock(&timer_mutex);
    int rc = 0;
    while (running && rc != ETIMEDOUT) {
        rc = pthread_cond_timedwait(&timer_cond, &timer_mutex, &abstime);
        if (rc == ETIMEDOUT) break;
        if (!running) break;
        // Spurious wakeups loop around until timeout or shutdown
    }
    pthread_mutex_unlock(&timer_mutex);

    set_zone_state(zone, 0);

    // Remove ourselves from tracking list
    remove_worker(self);

    return NULL;
}

// Improved parsing: strict numeric parsing and validation using strtol
static int parse_command(int cfd, const char *line) {
    if (!line) return -1;
    // Reject overly long lines explicitly
    if (strlen(line) >= CMD_BUF_SZ - 1) {
        const char err[] = "ERR too long\n";
        write_all(cfd, err, sizeof(err) - 1);
        return -1;
    }

    const char *zp = strstr(line, "ZONE=");
    const char *tp = strstr(line, "TIME=");
    if (!zp || !tp) return -1;

    char *endp;
    long z = strtol(zp + 5, &endp, 10);
    // Require at least one digit and that the next char is space, NUL, CR, or LF
    if (endp == zp + 5) return -1;
    if (!( *endp == '\0' || *endp == ' ' || *endp == '\r' || *endp == '\n')) return -1;
    if (z < 1 || z > MAX_ZONE) return -1;

    long t = strtol(tp + 5, &endp, 10);
    if (endp == tp + 5) return -1;
    if (!( *endp == '\0' || *endp == ' ' || *endp == '\r' || *endp == '\n')) return -1;
    if (t < 0) return -1;

    if (t == 0) {
        set_zone_state((int)z, 0);
        const char ok[] = "OK\n";
        write_all(cfd, ok, sizeof(ok) - 1);
        return 0;
    } else {
        pthread_t tid;
        int *args = xmalloc(2 * sizeof(int));
        if (!args) {
            const char err[] = "ERR internal\n";
            write_all(cfd, err, sizeof(err) - 1);
            close(cfd);                                                             //                                                  ADDED AT COL 81 -> Close the socket on allocation failure to avoid FD leak
            return -1;
        }
        args[0] = (int)z;
        args[1] = (int)t;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, WORKER_STACK_SZ);                         //                                                  ADDED AT COL 81 -> Limit worker stack size to reduce per-thread memory
        int rc = pthread_create(&tid, &attr, worker_thread, args);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            syslog(LOG_ERR, "pthread_create failed: %s", strerror(rc));
            free(args);
            const char err[] = "ERR internal\n";
            write_all(cfd, err, sizeof(err) - 1);
            close(cfd);                                                             //                                                  ADDED AT COL 81 -> Close the socket on thread-create failure
            return -1;
        }

        // We don't detach: we track and join at shutdown
        if (add_worker(tid) != 0) {
            pthread_detach(tid);                                                    // If tracking fails, detach as fallback
        }

        const char ok[] = "OK\n";
        write_all(cfd, ok, sizeof(ok) - 1);
        return 0;
    }
}

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

// Thread to handle a single client connection
static void *client_thread(void *arg) {
    int cfd = *(int *)arg;
    free(arg);

    // For safety: ensure FD_CLOEXEC set on accepted socket to avoid descriptor leaks
    set_cloexec(cfd);

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
    return NULL;
}

int main(void) {
    // Setup signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Initialize hardware
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

    while (running) {
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

        // Set close-on-exec on accepted socket
        set_cloexec(*cfd);

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, cfd) != 0) {
            syslog(LOG_ERR, "pthread_create failed for client handler: %s", strerror(errno));
            close(*cfd);
            free(cfd);
            continue;
        }

        // Track client thread for orderly shutdown
        if (add_worker(tid) != 0) {
            // If we can't track the client thread, detach as fallback to avoid resource leak
            pthread_detach(tid);
        }
    }

    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }

    join_workers_and_cleanup();

    syslog(LOG_INFO, "irrigationd shutdown complete");
    return 0;
}