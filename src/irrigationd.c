/**
 * @file irrigationd.c
 * @brief Irrigation control daemon using MCP23017 driver backend
 *
 * This daemon listens on a TCP socket (port 4242 by default) and accepts simple commands
 * of the form:
 *
 *   ZONE=<zone_number> TIME=<seconds> [TOKEN=<secret>]
 *
 * - ZONE ranges from 1..14
 * - TIME=0 turns a zone OFF immediately
 * - TIME=N>0 turns a zone ON for N seconds, then back OFF automatically
 * - TOKEN is optional unless IRRIGATIOND_TOKEN environment variable is set
 *
 * Features:
 *   - Multi-threaded client handling
 *   - Worker threads for timed zone control
 *   - Shared state protected by mutex
 *   - Clean shutdown via signals (SIGINT/SIGTERM) using a self-pipe pattern
 *   - Syslog logging for all lifecycle and error events
 *   - Optional debug mode via environment variable
 *
 * Hardening improvements:
 *   - Token authentication if IRRIGATIOND_TOKEN is set
 *   - Default bind to loopback; IRRIGATIOND_BIND_ADDR=0.0.0.0 exposes externally
 *   - xmalloc/xrealloc helpers with logging
 *   - FD_CLOEXEC set on all file descriptors
 *   - pthread_attr_setstacksize to reduce per-thread memory footprint
 *   - Interruptible timers using pthread_cond_timedwait
 *   - Semaphore limits concurrent workers
 *   - SO_RCVTIMEO reduces Slowloris risk
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
#include <sys/time.h>   // for struct timeval
#include <syslog.h>
#include <semaphore.h>
#include <poll.h>

#include "mcp23017.h"

#define MAX_ZONE        14
#define SERVER_PORT     4242
#define BACKLOG         5
#define CMD_BUF_SZ      256
#define WORKER_STACK_SZ (128 * 1024)
#define MAX_WORKERS     64

// --- Global state ---

static int zone_state[MAX_ZONE + 1];
static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;

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

static pthread_cond_t timer_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t timer_mutex = PTHREAD_MUTEX_INITIALIZER;

static int sigpipe_fd[2] = { -1, -1 };
static sem_t worker_slots;

// Authentication / bind address config
static const char *g_auth_token = NULL;  // Require TOKEN= if set
static int g_bind_inaddr_any = 0;        // 0=loopback(default), 1=INADDR_ANY

// --- Allocation helpers ---

static void* xmalloc(size_t sz) {
    void *p = malloc(sz);
    if (!p) {
        syslog(LOG_ERR, "malloc(%zu) failed", sz);
        if (getenv("IRRIGATIOND_OOM_ABORT")) abort();
    }
    return p;
}

static void* xrealloc(void *ptr, size_t sz) {
    void *p = realloc(ptr, sz);
    if (!p) {
        syslog(LOG_ERR, "realloc(%zu) failed", sz);
        if (getenv("IRRIGATIOND_OOM_ABORT")) abort();
    }
    return p;
}

// --- Utility functions ---

static void set_cloexec(int fd) {
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) return;
    (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

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

// --- Worker management ---

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

// --- Signal handling ---

static void shutdown_signal_handler(int signo) {
    (void)signo;
    running = 0;
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    if (sigpipe_fd[1] >= 0) {
        char c = 1;
        (void)write(sigpipe_fd[1], &c, 1);
    }
}

// --- Cleanup ---

static void join_workers_and_cleanup(void) {
    pthread_mutex_lock(&timer_mutex);
    pthread_cond_broadcast(&timer_cond);
    pthread_mutex_unlock(&timer_mutex);

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
            syslog(LOG_WARNING, "pthread_join failed: %s", strerror(jrc));
        }
    }

    free(worker_list);
    worker_list = NULL;
    worker_capacity = 0;

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

// --- Zone control ---

static void set_zone_state(int zone, int state) {
    if (zone < 1 || zone > MAX_ZONE) {
        syslog(LOG_ERR, "Invalid zone %d", zone);
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

// --- Worker thread ---

static void *worker_thread(void *arg) {
    int zone   = ((int *)arg)[0];
    int time_s = ((int *)arg)[1];
    free(arg);

    pthread_t self = pthread_self();
    set_zone_state(zone, 1);

    struct timespec now, abstime;
    clock_gettime(CLOCK_REALTIME, &now);

    if (time_s < 0 || time_s > INT_MAX) {
        syslog(LOG_ERR, "Invalid duration %d", time_s);
        set_zone_state(zone, 0);
        remove_worker(self);
        sem_post(&worker_slots);
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
    sem_post(&worker_slots);
    return NULL;
}

// --- Command parsing ---

static int parse_command(int cfd, const char *line) {
    if (!line) return -1;
    if (strlen(line) >= CMD_BUF_SZ - 1) {
        const char err[] = "ERR too long\n";
        write_all(cfd, err, sizeof(err) - 1);
        return -1;
    }

    if (g_auth_token) {
        const char *tkp = strstr(line, "TOKEN=");
        if (!tkp) {
            const char err[] = "ERR auth required\n";
            write_all(cfd, err, sizeof(err) - 1);
            syslog(LOG_WARNING, "Rejected command: token missing");
            return -1;
        }
        const char *val_start = tkp + 6;
        const char *val_end = val_start;
        while (*val_end && *val_end != ' ' && *val_end != '\r' && *val_end != '\n') val_end++;
        size_t len = (size_t)(val_end - val_start);
        if (len != strlen(g_auth_token) || memcmp(val_start, g_auth_token, len) != 0) {
            const char err[] = "ERR auth failed\n";
            write_all(cfd, err, sizeof(err) - 1);
            syslog(LOG_WARNING, "Rejected command: token mismatch");
            return -1;
        }
    }

    const char *zp = strstr(line, "ZONE=");
    const char *tp = strstr(line, "TIME=");
    if (!zp || !tp) return -1;

    char *endp;
    errno = 0;
    long z = strtol(zp + 5, &endp, 10);
    if (errno == ERANGE || endp == zp + 5) return -1;
    if (z < 1 || z > MAX_ZONE) return -1;

    errno = 0;
    long t = strtol(tp + 5, &endp, 10);
    if (errno == ERANGE || endp == tp + 5) return -1;
    if (t < 0 || t > 86400) {
        syslog(LOG_ERR, "Rejected TIME=%ld", t);
        const char err[] = "ERR TIME invalid (0-86400)\n";
        write_all(cfd, err, sizeof(err) - 1);
        return -1;
    }

    if (t == 0) {
        set_zone_state((int)z, 0);
        const char ok[] = "OK\n";
        write_all(cfd, ok, sizeof(ok) - 1);
        return 0;
    }

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
        sem_post(&worker_slots);
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
        sem_post(&worker_slots);
        return -1;
    }

    if (add_worker(tid) != 0) {
        pthread_detach(tid);
    }

    const char ok[] = "OK\n";
    write_all(cfd, ok, sizeof(ok) - 1);
    return 0;
}

// --- Networking helpers ---

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

static void *client_thread(void *arg) {
    int cfd = *(int *)arg;
    free(arg);
    set_cloexec(cfd);

    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[CMD_BUF_SZ];
    ssize_t n = read_line(cfd, buf, sizeof(buf));
    if (n > 0) {
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        if (n > 1 && buf[n - 2] == '\r') buf[n - 2] = '\0';
        if (parse_command(cfd, buf) < 0) {
            const char err[] = "ERR syntax (use: ZONE=<1..14> TIME=<seconds> [TOKEN])\n";
            write_all(cfd, err, sizeof(err) - 1);
        }
    } else if (n < 0) {
        syslog(LOG_ERR, "read from client failed: %s", strerror(errno));
    }

    close(cfd);
    sem_post(&worker_slots);
    remove_worker(pthread_self());
    return NULL;
}

// --- Main entry point ---

int main(void) {
    openlog("irrigationd", LOG_PID | LOG_CONS, LOG_DAEMON);

    // Read env configuration
    g_auth_token = getenv("IRRIGATIOND_TOKEN");
    const char *bind_env = getenv("IRRIGATIOND_BIND_ADDR");
    if (bind_env && strcmp(bind_env, "0.0.0.0") == 0) {
        g_bind_inaddr_any = 1;
    }

    if (pipe(sigpipe_fd) == 0) {
        set_cloexec(sigpipe_fd[0]);
        set_cloexec(sigpipe_fd[1]);
    }

    sem_init(&worker_slots, 0, MAX_WORKERS);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (mcp_i2c_open("/dev/i2c-1") < 0) {
        syslog(LOG_ERR, "Failed to open I2C bus");
        exit(EXIT_FAILURE);
    }
    if (mcp_config_outputs() < 0) {
        syslog(LOG_ERR, "Failed to configure MCP23017 outputs");
        mcp_i2c_close();
        exit(EXIT_FAILURE);
    }
    mcp_enable_thread_safety(&zone_lock);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        syslog(LOG_ERR, "socket() failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    set_cloexec(listen_fd);

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = g_bind_inaddr_any ? INADDR_ANY : htonl(INADDR_LOOPBACK);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "bind() failed: %s", strerror(errno));
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    if (listen(listen_fd, BACKLOG) < 0) {
        syslog(LOG_ERR, "listen() failed: %s", strerror(errno));
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "irrigationd listening on port %d", SERVER_PORT);

    while (running) {
        struct pollfd pfd[2];
        pfd[0].fd = listen_fd;
        pfd[0].events = POLLIN;
        pfd[1].fd = sigpipe_fd[0];
        pfd[1].events = POLLIN;
        int prc = poll(pfd, 2, -1);
        if (prc < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "poll() failed: %s", strerror(errno));
            break;
        }
        if (pfd[1].revents & POLLIN) break;

        if (pfd[0].revents & POLLIN) {
            struct sockaddr_in cli;
            socklen_t clen = sizeof(cli);
            int cfd = accept(listen_fd, (struct sockaddr *)&cli, &clen);
            if (cfd < 0) {
                if (errno == EINTR) continue;
                syslog(LOG_ERR, "accept() failed: %s", strerror(errno));
                continue;
            }

            pthread_t tid;
            int *fdarg = xmalloc(sizeof(int));
            if (!fdarg) {
                close(cfd);
                continue;
            }
            *fdarg = cfd;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, WORKER_STACK_SZ);
            int rc = pthread_create(&tid, &attr, client_thread, fdarg);
            pthread_attr_destroy(&attr);
            if (rc != 0) {
                syslog(LOG_ERR, "pthread_create failed: %s", strerror(rc));
                close(cfd);
                free(fdarg);
                continue;
            }
            add_worker(tid);
        }
    }

    join_workers_and_cleanup();

    if (sigpipe_fd[0] >= 0) close(sigpipe_fd[0]);
    if (sigpipe_fd[1] >= 0) close(sigpipe_fd[1]);
    if (listen_fd >= 0) close(listen_fd);

    closelog();
    return 0;
}