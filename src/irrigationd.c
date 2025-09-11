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

#define _POSIX_C_SOURCE 200809L                                                 // Required for sigaction, strsignal, etc.

#include <stdio.h>                                                              // Standard I/O
#include <stdlib.h>                                                             // Standard library: malloc, free, exit
#include <string.h>                                                             // String handling: memset, strstr
#include <unistd.h>                                                             // POSIX API: close, sleep
#include <errno.h>                                                              // errno and strerror
#include <sys/types.h>                                                          // Socket programming
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>                                                            // POSIX threads
#include <signal.h>                                                             // Signal handling
#include <stdatomic.h>                                                          // Atomic variables for clean shutdown flag

#include "mcp23017.h"                                                           // Hardware abstraction for MCP23017 driver

#define MAX_ZONE        14                                                      // Hardware supports 14 logical irrigation zones
#define SERVER_PORT     4242                                                    // Default TCP/IP port for client connections
#define BACKLOG         5                                                       // Maximum number of queued incoming connections
#define CMD_BUF_SZ      256                                                     // Size of command buffer for reading client input

// --- Global State ---

static int zone_state[MAX_ZONE + 1];                                            // Tracks current ON/OFF state of each zone (1-based indexing)
static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;                   // Mutex for protecting zone_state array

static pthread_mutex_t workers_lock = PTHREAD_MUTEX_INITIALIZER;                // Protects worker thread list
static pthread_t *worker_list = NULL;                                           // Dynamic array of worker thread IDs
static size_t worker_count = 0;                                                 // Number of active worker threads
static size_t worker_capacity = 0;                                              // Current capacity of worker_list array

static int listen_fd = -1;                                                      // File descriptor for listening socket
static volatile sig_atomic_t running = 1;                                       // Global flag used to shut down gracefully on SIGINT/SIGTERM

// write_all() ensures that short writes caused by signals or partial sends
// are retried until the entire buffer is written or an error occurs.
static ssize_t write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;                                       // Retry if interrupted by a signal
            return -1;                                                          // On permanent error, abort
        }
        p += n;                                                                 // Advance pointer by number of bytes successfully written
        remaining -= (size_t)n;                                                 // Decrease remaining count
    }
    return (ssize_t)len;                                                        // Return total number of bytes written
}

// Dynamically track worker threads. Ensures we can join them later for clean shutdown.
static int add_worker(pthread_t tid) {
    int rc = 0;
    if (pthread_mutex_lock(&workers_lock) != 0) return -1;

    if (worker_count == worker_capacity) {                                      // Need to grow the dynamic array
        size_t newcap = worker_capacity == 0 ? 16 : worker_capacity * 2;        // Start with 16, then double
        pthread_t *n = realloc(worker_list, newcap * sizeof(pthread_t));
        if (!n) {
            fprintf(stderr, "ERROR: realloc failed while tracking threads\n");  // Added explicit error log
            rc = -1;
            goto out;
        }
        worker_list = n;
        worker_capacity = newcap;
    }
    worker_list[worker_count++] = tid;                                          // Append new thread ID
out:
    pthread_mutex_unlock(&workers_lock);
    return rc;
}

// Signal handler for SIGINT/SIGTERM
// Sets the global shutdown flag and closes the listening socket.
static void shutdown_signal_handler(int signo) {
    (void)signo;
    running = 0;
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
}

// Wait for all worker threads to finish, then free resources and turn all zones off.
static void join_workers_and_cleanup(void) {
    if (pthread_mutex_lock(&workers_lock) == 0) {
        for (size_t i = 0; i < worker_count; ++i) {
            int jrc = pthread_join(worker_list[i], NULL);
            if (jrc != 0) {
                fprintf(stderr, "WARNING: pthread_join failed on worker %zu: %s\n",
                        i, strerror(jrc));                                      // Added safety: log join failures
            }
        }
        free(worker_list);
        worker_list = NULL;
        worker_count = 0;
        worker_capacity = 0;
        pthread_mutex_unlock(&workers_lock);
    }

    // Ensure all zones are OFF at shutdown
    mcp_lock();
    for (int z = 1; z <= MAX_ZONE; ++z) {
        if (zone_state[z] != 0) {
            mcp_set_zone_state(z, 0);
            zone_state[z] = 0;
        }
    }
    mcp_unlock();

    mcp_i2c_close();                                                            // Close the I²C device cleanly
}

// Set a single zone ON or OFF (thread-safe)
static void set_zone_state(int zone, int state) {
    if (zone < 1 || zone > MAX_ZONE) {
        fprintf(stderr, "ERROR: set_zone_state(): invalid zone %d\n", zone);
        return;
    }

    mcp_lock();                                                                 // Lock MCP23017 driver mutex
    if (mcp_set_zone_state(zone, state) != 0) {
        fprintf(stderr, "ERROR: Failed to set zone %d -> %s\n", zone, state ? "ON" : "OFF");
    } else {
        zone_state[zone] = state;
        printf("Zone %d -> %s\n", zone, state ? "ON" : "OFF");
    }
    mcp_unlock();                                                               // Unlock driver mutex
}

// Worker thread: turns a zone ON, sleeps for given time, then turns it OFF.
static void *worker_thread(void *arg) {
    int zone   = ((int *)arg)[0];
    int time_s = ((int *)arg)[1];
    free(arg);                                                                  // Free the heap-allocated argument array

    set_zone_state(zone, 1);
    sleep(time_s);
    set_zone_state(zone, 0);
    return NULL;
}

// Parse a client command line. Supports "ZONE=X TIME=Y".
// Uses strtol() for robust numeric parsing and error checking.
static int parse_command(int cfd, const char *line) {
    const char *zp = strstr(line, "ZONE=");
    const char *tp = strstr(line, "TIME=");
    if (!zp || !tp) return -1;

    char *endp;
    long z = strtol(zp + 5, &endp, 10);
    if (endp == zp + 5 || z < 1 || z > MAX_ZONE) return -1;                     // Invalid zone

    long t = strtol(tp + 5, &endp, 10);
    if (endp == tp + 5 || t < 0) return -1;                                     // Invalid time

    if (t == 0) {                                                               // TIME=0 → turn OFF immediately
        set_zone_state((int)z, 0);
        const char ok[] = "OK\n";
        (void)write_all(cfd, ok, sizeof(ok) - 1);
        return 0;
    } else {                                                                    // TIME>0 → create worker thread
        pthread_t tid;
        int *args = malloc(2 * sizeof(int));
        if (!args) {
            perror("malloc");
            close(cfd);                                                         // FIX: avoid FD leak if malloc fails
            return -1;
        }
        args[0] = (int)z;
        args[1] = (int)t;

        if (pthread_create(&tid, NULL, worker_thread, args) != 0) {
            perror("pthread_create");
            free(args);
            close(cfd);                                                         // FIX: avoid FD leak if pthread_create fails
            return -1;
        }
        if (add_worker(tid) != 0) {
            pthread_detach(tid);                                                // If tracking fails, detach to avoid zombie thread
        }

        const char ok[] = "OK\n";
        (void)write_all(cfd, ok, sizeof(ok) - 1);
        return 0;
    }
}

// Read a line from a client socket, handling CR/LF termination.
static ssize_t read_line(int fd, char *buf, size_t sz) {
    if (sz < 2) return -1;
    size_t used = 0;

    while (used < sz - 1) {
        ssize_t n = read(fd, buf + used, 1);
        if (n == 0) break;                                                      // EOF (client closed connection)
        else if (n < 0) {
            if (errno == EINTR) continue;                                       // Retry if interrupted by signal
            return -1;
        } else {
            if (buf[used] == '\r') continue;                                    // Ignore carriage return
            used++;
            if (buf[used - 1] == '\n') break;                                   // Stop at newline
        }
    }

    buf[used] = '\0';                                                           // Null-terminate string
    return (ssize_t)used;
}

// Thread for each client connection
static void *client_thread(void *arg) {
    int cfd = *(int *)arg;
    free(arg);

    char buf[CMD_BUF_SZ];
    ssize_t n = read_line(cfd, buf, sizeof(buf));
    if (n > 0) {
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        if (n > 1 && buf[n - 2] == '\r') buf[n - 2] = '\0';
        if (parse_command(cfd, buf) < 0) {
            const char err[] = "ERR syntax (use: ZONE=<1..14> TIME=<seconds>)\n";
            (void)write_all(cfd, err, sizeof(err) - 1);
        }
    } else if (n < 0) {
        perror("read");
    }

    close(cfd);                                                                 // Always close client socket
    return NULL;
}

int main(void) {
    // Setup signal handlers for graceful shutdown
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Initialize MCP23017 driver
    if (mcp_i2c_open("/dev/i2c-1") < 0) {
        fprintf(stderr, "ERROR: Failed to open I2C bus\n");
        exit(EXIT_FAILURE);
    }
    if (mcp_config_outputs() < 0) {
        fprintf(stderr, "ERROR: Failed to configure MCP23017 outputs\n");
        mcp_i2c_close();
        exit(EXIT_FAILURE);
    }
    mcp_enable_thread_safety(&zone_lock);                                       // Enable driver-level thread safety

    // Create listening socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        mcp_i2c_close();
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        mcp_i2c_close();
        exit(EXIT_FAILURE);
    }
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        close(listen_fd);
        mcp_i2c_close();
        exit(EXIT_FAILURE);
    }

    printf("irrigationd listening on port %d\n", SERVER_PORT);

    // Main server loop
    while (running) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);

        int *cfd = malloc(sizeof(int));
        if (!cfd) {
            perror("malloc");
            sleep(1);
            continue;
        }

        *cfd = accept(listen_fd, (struct sockaddr *)&cli, &len);
        if (*cfd < 0) {
            int err = errno;
            free(cfd);
            if (!running) break;                                                // If shutting down, break loop
            if (err == EINTR) continue;                                         // Retry if interrupted
            perror("accept");
            continue;
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, cfd) != 0) {
            perror("pthread_create");
            close(*cfd);
            free(cfd);
            continue;
        }

        if (add_worker(tid) != 0) {
            pthread_detach(tid);                                                // If tracking fails, detach thread
        }
    }

    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }

    join_workers_and_cleanup();                                                 // Ensure all workers exit and zones OFF
    return 0;
}