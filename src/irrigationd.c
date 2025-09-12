#define _POSIX_C_SOURCE 199309L  // Ensure POSIX functions are available

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <semaphore.h>
#include <sys/types.h>
#include <time.h>               // For nanosleep and struct timespec
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>           // For struct timeval and setsockopt

// Fallback for strdup if it's not available
#ifndef strdup
char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *dup = malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}
#endif

// Fallback for strlcpy if it's not available
#ifndef strlcpy
// Provides a standard C implementation of strlcpy to avoid compiler warnings
// about non-standard GCC extensions.
static size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t src_len = strlen(src);
    if (size > 0) {
        size_t copy_len = (src_len >= size) ? (size - 1) : src_len;
        memcpy(dst, src, copy_len);
        dst[copy_len] = '\0';
    }
    return src_len;
}
#endif

#include "mcp23017.h"
#include "rate_limit.h"
#include "auth.h"

// -----------------------------------------------------------------------------
// irrigationd.c - Irrigation controller daemon (modularized & hardened)
// -----------------------------------------------------------------------------

#define SERVER_PORT     4242
#define CMD_BUF_SZ      256
#define MAX_WORKERS     32
#define ADDRBUF_SZ      64
#define TOKEN_MAX_SZ    128
#define WHO_SZ          64

static int zone_state[MAX_ZONE+1];
static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t worker_slots;
static pthread_mutex_t workers_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t *worker_list = NULL;
static size_t worker_count = 0;
static size_t worker_capacity = 0;
static char *g_auth_token = NULL;
static volatile sig_atomic_t shutting_down = 0;
static volatile int listen_fd = -1;

// -----------------------------------------------------------------------------
// Worker tracking helpers
// -----------------------------------------------------------------------------
static void *xmalloc(size_t sz) {
    void *p = malloc(sz);
    if (!p) { fprintf(stderr, "malloc failed\n"); exit(1); }
    return p;
}

static void add_worker(pthread_t tid) {
    pthread_mutex_lock(&workers_lock);
    if (worker_count == worker_capacity) {
        size_t newcap = worker_capacity ? worker_capacity * 2 : 8;
        pthread_t *newlist = realloc(worker_list, newcap * sizeof(pthread_t));
        if (!newlist) {
            // On allocation failure, log and keep going without expanding.
            fprintf(stderr, "add_worker: realloc failed; continuing without tracking\n");
            pthread_mutex_unlock(&workers_lock);
            return;
        }
        worker_list = newlist;
        worker_capacity = newcap;
    }
    worker_list[worker_count++] = tid;
    pthread_mutex_unlock(&workers_lock);
}

static void remove_worker(pthread_t tid) {
    pthread_mutex_lock(&workers_lock);
    for (size_t i = 0; i < worker_count; i++) {
        if (pthread_equal(worker_list[i], tid)) {
            worker_list[i] = worker_list[--worker_count];
            break;
        }
    }
    pthread_mutex_unlock(&workers_lock);
}

static void join_workers_and_cleanup(void) {
    pthread_mutex_lock(&workers_lock);
    for (size_t i = 0; i < worker_count; i++) {
        int jrc = pthread_join(worker_list[i], NULL);
        if (jrc != 0) {
            fprintf(stderr, "pthread_join failed on worker %zu: %s\n", i, strerror(jrc));
        }
    }
    free(worker_list);
    worker_list = NULL;
    worker_count = worker_capacity = 0;
    pthread_mutex_unlock(&workers_lock);
}

// -----------------------------------------------------------------------------
// Zone control
// -----------------------------------------------------------------------------
static void set_zone_state(const char *who, int zone, int state) {
    if (zone < 1 || zone > MAX_ZONE) return;
    pthread_mutex_lock(&zone_lock);
    zone_state[zone] = state;
    pthread_mutex_unlock(&zone_lock);

    // Perform I2C update under driver lock inside the driver
    mcp_lock();
    if (mcp_set_zone_state(zone, state) < 0) {
        fprintf(stderr, "[%s] mcp_set_zone_state failed for zone %d\n", who ? who : "unknown", zone);
    }
    mcp_unlock();

    fprintf(stderr, "[%s] Zone %d -> %s\n", who ? who : "?", zone, state ? "ON" : "OFF");
}

// -----------------------------------------------------------------------------
// Interruptible sleep for worker threads
// -----------------------------------------------------------------------------
static void interruptible_sleep_seconds(int seconds) {
    // Use nanosleep in a loop and allow pthread_cancel to interrupt
    struct timespec req, rem;
    req.tv_sec = seconds;
    req.tv_nsec = 0;

    // Allow thread cancellation
    int oldstate;
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);

    while (nanosleep(&req, &rem) == -1) {
        if (errno == EINTR) {
            // If interrupted by signal, recalc remaining sleep
            req = rem;
            continue;
        } else {
            break;
        }
    }

    // Disable cancellation if caller expects it
    pthread_setcancelstate(oldstate, NULL);
}

// -----------------------------------------------------------------------------
// Worker thread
// -----------------------------------------------------------------------------
struct worker_arg { int zone, duration; char who[WHO_SZ]; };

static void *worker_thread(void *arg) {
    struct worker_arg *wa = arg;
    if (!wa) return NULL;

    set_zone_state(wa->who, wa->zone, 1);

    if (wa->duration > 0) {
        // Use interruptible sleep so worker can be cancelled during shutdown.
        interruptible_sleep_seconds(wa->duration);
        set_zone_state(wa->who, wa->zone, 0);
    }

    free(wa);
    sem_post(&worker_slots);
    remove_worker(pthread_self());
    return NULL;
}

// -----------------------------------------------------------------------------
// Safe write function that checks write() return value
// -----------------------------------------------------------------------------
static void safe_write(int cfd, const char *msg, size_t len) {
    ssize_t ret = write(cfd, msg, len);
    if (ret == -1) {
        perror("write failed");
    }
}

// -----------------------------------------------------------------------------
// Safe read-line (bounded, newline-terminated)
// -----------------------------------------------------------------------------
static ssize_t read_line(int fd, char *buf, size_t sz, int timeout_seconds) {
    if (sz == 0) return -1;
    size_t i = 0;
    struct timeval tv;  // This requires <sys/time.h>
    tv.tv_sec = timeout_seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (i < sz - 1) {
        char c;
        ssize_t rc = read(fd, &c, 1);
        if (rc == 1) {
            buf[i++] = c;
            if (c == '\n') break;
        } else if (rc == 0) {
            // EOF
            break;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -2; // timeout
            return -1;
        }
    }
    buf[i] = '\0';
    // Trim trailing CR/LF
    while (i > 0 && (buf[i-1] == '\n' || buf[i-1] == '\r')) buf[--i] = '\0';
    return (ssize_t)i;
}

// -----------------------------------------------------------------------------
// Command parsing
// -----------------------------------------------------------------------------
static int parse_command(int cfd, const char *cmd, const char *addrbuf) {
    char zone_s[32] = {0}, time_s[32] = {0}, token_s[TOKEN_MAX_SZ] = {0};
    int have_zone = 0, have_time = 0, have_token = 0;

    // copy safely and ensure NUL termination
    char buf[CMD_BUF_SZ];
    memset(buf, 0, sizeof(buf));
    strlcpy(buf, cmd, sizeof(buf));

    char *saveptr = NULL, *tok = strtok_r(buf, " \t\r\n", &saveptr);
    while (tok) {
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = '\0';
            char *key = tok;
            char *val = eq + 1;
            if (!strcmp(key, "ZONE")) { strncpy(zone_s, val, sizeof(zone_s) - 1); have_zone = 1; }
            else if (!strcmp(key, "TIME")) { strncpy(time_s, val, sizeof(time_s) - 1); have_time = 1; }
            else if (!strcmp(key, "TOKEN")) { strncpy(token_s, val, sizeof(token_s) - 1); have_token = 1; }
        }
        tok = strtok_r(NULL, " \t\r\n", &saveptr);
    }

    if (!have_zone || !have_time || !have_token) {
        safe_write(cfd, "ERR missing fields\n", 19);
        return -1;
    }

    // Trim token_s just in case
    size_t tlen = strlen(token_s);
    while (tlen > 0 && (token_s[tlen - 1] == '\r' || token_s[tlen - 1] == '\n')) token_s[--tlen] = '\0';

    if (!safe_str_equals(token_s, g_auth_token)) {
        char redacted[128];
        sanitize_cmd_for_log(cmd, redacted, sizeof(redacted));
        fprintf(stderr, "[%s] rejected (bad token): %s\n", addrbuf, redacted);
        safe_write(cfd, "ERR auth required\n", 18);
        return -1;
    }

    // parse numbers with checks
    char *endptr = NULL;
    errno = 0;
    long z = strtol(zone_s, &endptr, 10);
    if (endptr == zone_s || errno != 0) {
        safe_write(cfd, "ERR invalid ZONE\n", 17);
        return -1;
    }
    errno = 0;
    long t = strtol(time_s, &endptr, 10);
    if (endptr == time_s || errno != 0) {
        safe_write(cfd, "ERR invalid TIME\n", 17);
        return -1;
    }

    if (z < 1 || z > MAX_ZONE || t < 0 || t > 86400) {
        safe_write(cfd, "ERR invalid\n", 12);
        return -1;
    }

    if (t == 0) {
        set_zone_state(addrbuf, (int)z, 0);
        safe_write(cfd, "OK\n", 3);
        return 0;
    }

    // Acquire worker slot (non-blocking)
    if (sem_trywait(&worker_slots) != 0) {
        safe_write(cfd, "ERR busy\n", 9);
        return -1;
    }

    struct worker_arg *wa = xmalloc(sizeof(*wa));
    wa->zone = (int)z;
    wa->duration = (int)t;
    snprintf(wa->who, sizeof(wa->who), "%s", addrbuf);

    pthread_t tid;
    if (pthread_create(&tid, NULL, worker_thread, wa) != 0) {
        free(wa);
        sem_post(&worker_slots);
        safe_write(cfd, "ERR thread\n", 11);
        return -1;
    }
    add_worker(tid);
    safe_write(cfd, "OK\n", 3);

    char redacted[128];
    sanitize_cmd_for_log(cmd, redacted, sizeof(redacted));
    fprintf(stderr, "[%s] Accepted command: '%s'\n", addrbuf, redacted);
    return 0;
}

// -----------------------------------------------------------------------------
// Client thread
// -----------------------------------------------------------------------------
struct client_arg { int cfd; struct sockaddr_in cli; };

static void *client_thread(void *arg) {
    struct client_arg *carg = arg;
    int cfd = carg->cfd;
    struct sockaddr_in cli = carg->cli;
    free(carg);

    char addrbuf[ADDRBUF_SZ];
    inet_ntop(AF_INET, &cli.sin_addr, addrbuf, sizeof(addrbuf));

    char buf[CMD_BUF_SZ];
    ssize_t n = read_line(cfd, buf, sizeof(buf), 10);
    if (n > 0) {
        parse_command(cfd, buf, addrbuf);
    } else if (n == -2) {
        safe_write(cfd, "ERR timeout\n", 12);
    } else if (n < 0) {
        fprintf(stderr, "[%s] read from client failed: %s\n", addrbuf, strerror(errno));
    }

    close(cfd);
    remove_worker(pthread_self());
    return NULL;
}

// -----------------------------------------------------------------------------
// Signal handler for graceful shutdown
// -----------------------------------------------------------------------------
static void sigint_handler(int signo) {
    (void)signo;
    shutting_down = 1;
    // Closing the socket will unblock accept()
    if (listen_fd >= 0) {
        close(listen_fd);
    }
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------
int main(void) {
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    const char *env_token = getenv("IRRIGATIOND_TOKEN");
    if (!env_token || !*env_token) {
        fprintf(stderr, "IRRIGATIOND_TOKEN not set\n");
        exit(1);
    }
    g_auth_token = strdup(env_token);
    if (!g_auth_token) {
        fprintf(stderr, "failed to allocate token copy\n");
        exit(1);
    }

    const char *bind_addr_str = getenv("IRRIGATIOND_BIND_ADDR");
    if (!bind_addr_str || !*bind_addr_str) {
        bind_addr_str = "127.0.0.1"; // Safe default if not set
    }    

    if (mcp_i2c_open("/dev/i2c-1") < 0) exit(1);

    sem_init(&worker_slots, 0, MAX_WORKERS);
    rl_init();

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }

    // set CLOEXEC
    int flags = fcntl(listen_fd, F_GETFD);
    if (flags >= 0) fcntl(listen_fd, F_SETFD, flags | FD_CLOEXEC);

    int yes = 1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(listen_fd, 128) < 0) { perror("listen"); exit(1); }

    fprintf(stderr, "irrigationd listening on port %d\n", SERVER_PORT);

    while (!shutting_down) {
        struct sockaddr_in cli; socklen_t clen = sizeof(cli);
        int cfd = accept(listen_fd, (struct sockaddr*)&cli, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        uint32_t ip = ntohl(cli.sin_addr.s_addr);
        if (!rl_check_and_consume(ip)) {
            safe_write(cfd, "ERR rate limit\n", 15);
            close(cfd);
            continue;
        }

        struct client_arg *carg = xmalloc(sizeof(*carg));
        carg->cfd = cfd;
        carg->cli = cli;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, carg) != 0) {
            close(cfd);
            free(carg);
            continue;
        }
        add_worker(tid);
    }

    // Shutdown: cancel worker threads and join
    pthread_mutex_lock(&workers_lock);
    for (size_t i = 0; i < worker_count; i++) {
        pthread_cancel(worker_list[i]); // interrupt sleeping workers
    }
    pthread_mutex_unlock(&workers_lock);

    join_workers_and_cleanup();
    rl_cleanup();
    mcp_i2c_close();
    free(g_auth_token);
    close(listen_fd);
    return 0;
}
