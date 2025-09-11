#include <stdio.h>                                                                 // Standard I/O (fprintf, etc.)
#include <stdlib.h>                                                                // Memory management, exit codes
#include <string.h>                                                                // String handling (strstr, strcmp)
#include <unistd.h>                                                                // POSIX API (read, write, close)
#include <errno.h>                                                                 // errno variable and strerror()
#include <pthread.h>                                                               // POSIX threads for concurrency
#include <sys/socket.h>                                                            // Sockets API
#include <netinet/in.h>                                                            // sockaddr_in, htons, INADDR_ANY
#include <arpa/inet.h>                                                             // inet_ntop, inet_addr
#include <semaphore.h>                                                             // POSIX semaphores
#include <sys/types.h>                                                             // Basic system data types
#include <time.h>                                                                  // Sleep handling
#include <sys/time.h>                                                              // struct timeval for timeouts
#include <stdint.h>                                                                // For uint8_t

#include "mcp23017.h"                                                              // MCP23017 driver interface

// -----------------------------------------------------------------------------
// irrigationd.c - Irrigation controller daemon
//
// This daemon listens for simple text-based commands over TCP and manages
// irrigation zones connected through an MCP23017 GPIO expander.
//
// Example command format (single line, terminated by newline):
//   ZONE=1 TIME=30 TOKEN=changeme
//
// Commands:
//   ZONE=n TIME=s [TOKEN=secret] turn zone n ON for s seconds
//   ZONE=n TIME=0 [TOKEN=secret] turn zone n OFF immediately
//
// Token authentication is mandatory: if IRRIGATIOND_TOKEN is not set in the
// environment (via /etc/default/irrigationd), the daemon refuses to start.        // Prevents insecure deployments
//
// -----------------------------------------------------------------------------

#define SERVER_PORT     4242                                                       // TCP port to listen on
#define CMD_BUF_SZ      256                                                        // Max command length in bytes
#define MAX_WORKERS     32                                                         // Max concurrent worker threads

// -----------------------------------------------------------------------------
// Global shared state
// -----------------------------------------------------------------------------
static int zone_state[MAX_ZONE+1];                                                 // zone_state[z] = 1 if ON, 0 if OFF
static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;                      // Protects zone_state updates
static sem_t worker_slots;                                                         // Semaphore limits concurrent workers
static pthread_mutex_t workers_lock = PTHREAD_MUTEX_INITIALIZER;                   // Protects worker_list
static pthread_t *worker_list = NULL;                                              // Dynamic list of active workers
static size_t worker_count = 0;                                                    // Current number of workers
static size_t worker_capacity = 0;                                                 // Capacity of worker_list array
static const char *g_auth_token = NULL;                                            // Authentication token from env
static int g_bind_inaddr_any = 0;                                                  // Bind address flag (0=localhost only)

// -----------------------------------------------------------------------------
// Utility: memory-safe allocation wrappers
// -----------------------------------------------------------------------------
static void *xmalloc(size_t sz) {
    void *p = malloc(sz);
    if (!p) {
        fprintf(stderr, "malloc(%zu) failed: %s\n", sz, strerror(errno));          // stderr logging
        exit(EXIT_FAILURE);
    }
    return p;
}

static void *xrealloc(void *old, size_t sz) {
    void *p = realloc(old, sz);
    if (!p) {
        fprintf(stderr, "realloc(%zu) failed: %s\n", sz, strerror(errno));         // stderr logging
        exit(EXIT_FAILURE);
    }
    return p;
}

// -----------------------------------------------------------------------------
// Worker tracking (to ensure clean shutdown and prevent leaks)
// -----------------------------------------------------------------------------
static void add_worker(pthread_t tid) {
    pthread_mutex_lock(&workers_lock);
    if (worker_count == worker_capacity) {
        size_t newcap = worker_capacity ? worker_capacity * 2 : 8;                 // Grow exponentially
        pthread_t *newlist = xrealloc(worker_list, newcap * sizeof(pthread_t));
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
            worker_list[i] = worker_list[--worker_count];                          // Replace with last element
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
// Zone control helpers
// -----------------------------------------------------------------------------
static void set_zone_state(int zone, int state) {
    if (zone < 1 || zone > MAX_ZONE) {
        fprintf(stderr, "set_zone_state: invalid zone %d\n", zone);
        return;
    }

    pthread_mutex_lock(&zone_lock);
    zone_state[zone] = state;                                                      // Update logical state
    pthread_mutex_unlock(&zone_lock);

    mcp_lock();
    mcp_set_zone_state(zone, state);                                               // Push state to MCP23017 hardware
    mcp_unlock();

    fprintf(stderr, "Zone %d -> %s\n", zone, state ? "ON" : "OFF");
}

// -----------------------------------------------------------------------------
// Worker thread: executes timed zone control
// -----------------------------------------------------------------------------
struct worker_arg {
    int zone;
    int duration;
};

static void *worker_thread(void *arg) {
    struct worker_arg *wa = arg;

    if (!wa) {
        fprintf(stderr, "worker_thread: null arg\n");
        return NULL;
    }

    if (sem_wait(&worker_slots) != 0) {
        fprintf(stderr, "worker_thread: failed to acquire slot semaphore\n");
        free(wa);
        return NULL;
    }

    set_zone_state(wa->zone, 1);
    if (wa->duration > 0) {
        sleep(wa->duration);                                                       // Blocks this thread only
        set_zone_state(wa->zone, 0);
    }

    free(wa);
    sem_post(&worker_slots);                                                       // Release slot
    remove_worker(pthread_self());                                                 // Remove from tracking
    return NULL;
}

// -----------------------------------------------------------------------------
// Command parser
// -----------------------------------------------------------------------------
static char *get_kv(const char *cmd, const char *key) {
    size_t klen = strlen(key);
    const char *p = strstr(cmd, key);
    if (p && p[klen] == '=') {
        return (char *)(p + klen + 1);
    }
    return NULL;
}

static int parse_command(int cfd, const char *cmd, const char *addrbuf) {
    char *zone_s = get_kv(cmd, "ZONE");
    char *time_s = get_kv(cmd, "TIME");
    char *token  = get_kv(cmd, "TOKEN");

    if (!zone_s || !time_s) {
        const char err[] = "ERR missing ZONE or TIME\n";
        write(cfd, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Rejected command (missing ZONE/TIME): '%s'\n", addrbuf, cmd);
        return -1;
    }

    if (!token || strcmp(token, g_auth_token) != 0) {
        const char err[] = "ERR auth required\n";
        write(cfd, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Rejected command (bad/missing token): '%s'\n", addrbuf, cmd);
        return -1;
    }

    int zone = atoi(zone_s);
    int duration = atoi(time_s);

    if (zone < 1 || zone > MAX_ZONE || duration < 0 || duration > 86400) {        // Enforce 24h max
        const char err[] = "ERR invalid ZONE or TIME\n";
        write(cfd, err, sizeof(err) - 1);
        fprintf(stderr, "[%s] Rejected command (invalid zone/time): '%s'\n", addrbuf, cmd);
        return -1;
    }

    struct worker_arg *wa = xmalloc(sizeof(struct worker_arg));
    wa->zone = zone;
    wa->duration = duration;

    pthread_t tid;
    int rc = pthread_create(&tid, NULL, worker_thread, wa);
    if (rc != 0) {
        fprintf(stderr, "pthread_create failed: %s\n", strerror(rc));
        free(wa);
        const char err[] = "ERR cannot create worker\n";
        write(cfd, err, sizeof(err) - 1);
        return -1;
    }
    add_worker(tid);

    const char ok[] = "OK\n";
    write(cfd, ok, sizeof(ok) - 1);
    fprintf(stderr, "[%s] Accepted command: '%s'\n", addrbuf, cmd);
    return 0;
}

// -----------------------------------------------------------------------------
// Client thread handler (per connection)
// -----------------------------------------------------------------------------
static void format_client_addr(const struct sockaddr_in *cli, char *buf, size_t sz) {
    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &cli->sin_addr, ip, sizeof(ip))) {
        snprintf(buf, sz, "unknown");
    } else {
        snprintf(buf, sz, "%s:%d", ip, ntohs(cli->sin_port));
    }
}

static ssize_t read_line(int fd, char *buf, size_t sz) {
    size_t i = 0;
    while (i < sz - 1) {
        char c;
        ssize_t rc = read(fd, &c, 1);
        if (rc == 1) {
            if (c == '\n') break;
            buf[i++] = c;
        } else if (rc == 0) {
            break;                                                                 // EOF
        } else {
            if (errno == EINTR) continue;                                          // Retry if interrupted
            return -1;
        }
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

struct client_arg {
    int cfd;
    struct sockaddr_in cli;
};

static void *client_thread(void *arg) {
    struct client_arg *carg = arg;
    int cfd = carg->cfd;
    struct sockaddr_in cli = carg->cli;
    free(carg);

    char addrbuf[64];
    format_client_addr(&cli, addrbuf, sizeof(addrbuf));

    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[CMD_BUF_SZ];
    ssize_t n = read_line(cfd, buf, sizeof(buf));
    if (n > 0) {
        parse_command(cfd, buf, addrbuf);
    } else if (n < 0) {
        fprintf(stderr, "[%s] read from client failed: %s\n", addrbuf, strerror(errno));
    }

    close(cfd);
    sem_post(&worker_slots);
    remove_worker(pthread_self());
    return NULL;
}

// -----------------------------------------------------------------------------
// Main server loop
// -----------------------------------------------------------------------------
int main(void) {
    // Load environment configuration
    g_auth_token = getenv("IRRIGATIOND_TOKEN");
    const char *bind_env = getenv("IRRIGATIOND_BIND_ADDR");
    if (bind_env && strcmp(bind_env, "0.0.0.0") == 0) {
        g_bind_inaddr_any = 1;
    }

    if (g_auth_token) {
        fprintf(stderr, "Token enforcement enabled (IRRIGATIOND_TOKEN is set)\n");
    } else {
        fprintf(stderr, "IRRIGATIOND_TOKEN is not set, refusing to start insecure\n");
        exit(EXIT_FAILURE);
    }

    if (mcp_i2c_open("/dev/i2c-1") < 0) {
        fprintf(stderr, "ERROR: Failed to open I2C bus\n");
        exit(EXIT_FAILURE);
    }

    sem_init(&worker_slots, 0, MAX_WORKERS);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "socket failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = g_bind_inaddr_any ? INADDR_ANY : inet_addr("127.0.0.1");

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, 8) < 0) {
        fprintf(stderr, "listen failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "irrigationd listening on port %d\n", SERVER_PORT);

    while (1) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int cfd = accept(listen_fd, (struct sockaddr *)&cli, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "accept failed: %s\n", strerror(errno));
            continue;
        }

        struct client_arg *carg = xmalloc(sizeof(struct client_arg));
        carg->cfd = cfd;
        carg->cli = cli;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, carg) != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(errno));
            close(cfd);
            free(carg);
            continue;
        }
        add_worker(tid);
    }

    close(listen_fd);
    join_workers_and_cleanup();
    return 0;
}
