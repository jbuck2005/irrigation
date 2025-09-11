/**
 * @file irrigationd.c
 * @author James Buck
 * @date September 10, 2025
 * @brief Irrigation control daemon which controls a custom MCP23017 backend.
 *
 * @description
 * This daemon provides network-based control for a multi-zone irrigation system.
 * It listens for commands on a TCP port and controls sprinkler zones connected
 * via an MCP23017 I2C I/O expander. The server is multi-threaded, handling each
 * client and timed zone activation in a separate thread for concurrency.
 *
 * @section protocol Network Protocol
 * The server listens on TCP port 4242 and accepts simple text commands.
 * Each command must be terminated by a newline character.
 *
 * Command Format:
 * ZONE=<n> TIME=<seconds>
 *
 * Parameters:
 * - <n>: The irrigation zone number, an integer from 1 to 14.
 * - <seconds>: The duration in seconds for the zone to be active.
 * - If <seconds> > 0, the zone is activated for the specified duration
 * and then automatically deactivated.
 * - If <seconds> == 0, the zone is deactivated immediately. This can be
 * used to cancel an ongoing watering cycle for a specific zone.
 *
 * Server Responses:
 * - "OK\n": Sent upon successful parsing of a valid command.
 * - "ERR syntax (use: ZONE=<1..14> TIME=<seconds>)\n": Sent if the command is malformed or contains invalid parameters.
 *
 * @section dependencies Dependencies
 * - POSIX threads library (`-lpthread`)
 * - A custom driver for the MCP23017 I/O expander (`mcp23017.c`, `mcp23017.h`)
 *
 * @section build Building
 * gcc -o irrigationd irrigationd.c mcp23017.c -Wall -Wextra -pthread
 *
 * @section usage Usage Examples
 * # Turn ON zone 1 for 60 seconds
 * echo "ZONE=1 TIME=60" | netcat localhost 4242
 *
 * # Turn OFF zone 5 immediately
 * echo "ZONE=5 TIME=0" | netcat localhost 4242
 */

#define _POSIX_C_SOURCE 200809L                                                         // Ensure POSIX signal and threading APIs are available

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>

#include "mcp23017.h"                                                           // MCP23017 driver

#define MAX_ZONE        14                                                      // Maximum number of irrigation zones
#define SERVER_PORT     4242                                                    // TCP port for listening to commands
#define BACKLOG         5                                                       // Connection queue size for listen()
#define CMD_BUF_SZ      256                                                     // Command buffer size (larger than before)

/* --- Global State --- */
static int zone_state[MAX_ZONE + 1];                                            // Software mirror of zone states (0=OFF, 1=ON)
                                                                                //                                                                               
/* ADDITIONAL DOC: The zone_state[] array is protected by the MCP driver's mutex  */
// The driver exposes mcp_lock/mcp_unlock (no-op if no mutex) callers must use mcp_lock/mcp_unlock
// when performing a composite operation that touches hardware AND zone_state[]

static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;                   // Mutex to protect access to zone_state and hardware
                                                                                //                                                                               
/* ADDITIONAL DOC: The zone_lock is intended to be shared with the driver using  */
// mcp_enable_thread_safety(&zone_lock). The preferred usage pattern is to call mcp_lock()/mcp_unlock() around driver
// operations that must be atomic with updates to zone_state.

/* Prototypes for mcp_lock()/mcp_unlock() are in the header. */

/* --- Thread tracking for graceful shutdown --- */
static pthread_mutex_t workers_lock = PTHREAD_MUTEX_INITIALIZER;                // Protects worker list
static pthread_t *worker_list = NULL;                                           // Dynamically-grown list of worker + client threads
static size_t worker_count = 0;
static size_t worker_capacity = 0;

// We track created threads (both client handlers and timed worker threads) instead of detaching them so we can join them on shutdown.
// This lets the daemon clean up resources and guarantees orderly hardware shutdown (turning off all zones) before exit.

/* Listening socket (global so the signal handler can close it) */
static int listen_fd = -1;

/* Running flag: when set to 0 the main accept loop exits */
static volatile sig_atomic_t running = 1;

// The signal handler sets `running = 0` and closes the listening socket (close is async-signal-safe).
// The main thread observes running and performs orderly shutdown.

/* --- Helper: safe write_all (handles partial writes) --- */
static ssize_t write_all(int fd, const void *buf, size_t len)
{
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

// write_all() ensures that all bytes are written to the socket even if write() is interrupted or performs a short write.
// This replaces single-shot write() uses to make network I/O robust under stress.

/* --- Thread list helpers --- */
static int add_worker(pthread_t tid)
{
    int rc = 0;
    if (pthread_mutex_lock(&workers_lock) != 0) return -1;
    if (worker_count == worker_capacity) {
        size_t newcap = worker_capacity == 0 ? 16 : worker_capacity * 2;
        pthread_t *n = realloc(worker_list, newcap * sizeof(pthread_t));
        if (!n) {
            rc = -1;
            goto out;
        }
        worker_list = n;
        worker_capacity = newcap;
    }
    worker_list[worker_count++] = tid;
out:
    pthread_mutex_unlock(&workers_lock);
    return rc;
}

// We do not remove entries when threads finish; we collect and join all entries during shutdown. This keeps the implementation simple and safe.

/* --- Forward declarations --- */
static void shutdown_signal_handler(int signo);
static void join_workers_and_cleanup(void);

/**
 * @brief Sets the state of a specific irrigation zone.
 * @description This function updates both the software state array and the
 * physical hardware state via the MCP23017 driver. It is
 * thread-safe when the driver mutex is enabled via mcp_enable_thread_safety.
 * The function takes the following approach:
 *   1) Acquire driver lock via mcp_lock()                              <-- atomic region start
 *   2) Update hardware via mcp_set_zone_state() (caller holds lock)    <-- hardware update
 *   3) Update software mirror zone_state[] while still holding lock    <-- keep hw + sw consistent
 *   4) Release driver lock via mcp_unlock()                            <-- atomic region end
 * @param zone The zone number (1-based index).
 * @param state The desired state: 1 for ON, 0 for OFF.
 * @note The function also validates the zone index before indexing zone_state[].
 */
static void set_zone_state(int zone, int state)
{
    if (zone < 1 || zone > MAX_ZONE) {
        fprintf(stderr, "ERROR: set_zone_state(): invalid zone %d\n", zone);
        return;
    }

    mcp_lock(); // Acquire driver-provided lock to make the following sequence atomic: hardware update + software mirror update

    if (mcp_set_zone_state(zone, state) != 0) {
        fprintf(stderr, "ERROR: Failed to set zone %d -> %s\n", zone, state ? "ON" : "OFF");
        /* Note: we still update the software mirror only if the hardware call succeeded.
         * If you prefer to always reflect desired state in software regardless of HW error,
         * move the assignment outside this if block.
         */
    } else {
        zone_state[zone] = state;
        printf("Zone %d -> %s\n", zone, state ? "ON" : "OFF");
    }

    mcp_unlock();
}

/**
 * @brief Pthread entry point to manage a timed zone activation.
 * @description This function is executed in a new thread for commands with a
 * non-zero duration. It turns a zone ON, waits for the specified
 * time, and then turns the zone OFF.
 * @param arg A pointer to a dynamically allocated integer array containing
 * `{zone, time_s}`. The function takes ownership of this memory
 * and is responsible for freeing it.
 * @return Always returns NULL.
 */
static void *worker_thread(void *arg)
{
    int zone   = ((int *)arg)[0];
    int time_s = ((int *)arg)[1];
    free(arg);                                                                  // Free the argument memory as soon as it's copied
    set_zone_state(zone, 1);                                                    // Turn zone ON
    sleep         (time_s);                                                     // Wait for the specified duration
    set_zone_state(zone, 0);                                                    // Turn zone OFF
    return NULL;
}

/**
 * @brief Parses a command string from a client and executes the requested action.
 * @description Handles both immediate deactivation (TIME=0) and timed activation
 * by spawning a worker thread (TIME>0). Responds to the client
 * with "OK\n" on success or an error message on failure.
 * @param cfd The client's socket file descriptor for sending a response.
 * @param line The null-terminated command string received from the client.
 * @return 0 on success, -1 on parsing or execution error.
 */
static int parse_command(int cfd, const char *line)
{
    int z, t;
    if (sscanf(line, "ZONE=%d TIME=%d", &z, &t) != 2) {                         // Expect command format: "ZONE=<n> TIME=<seconds>"
        return -1;                                                              // Syntax error
    }

    if (z < 1 || z > MAX_ZONE || t < 0) {                                       // Validate parameter ranges
        return -1;                                                              // Invalid input
    }

    if (t == 0) {                                                               // TIME=0: Turn the zone OFF immediately. No new thread is needed.
        set_zone_state(z, 0);
        const char ok[] = "OK\n";
        (void)write_all(cfd, ok, sizeof(ok) - 1);                                // Send confirmation using write_all
        return 0;
    } else {                                                                    // TIME>0: Spawn a worker thread to handle the timed activation.
        pthread_t tid;
        int *args = malloc(2 * sizeof(int));
        if (!args) {
            perror("malloc");
            return -1;
        }
        args[0] = z;                                                            // what zone to control
        args[1] = t;                                                            // for how many seconds

        if (pthread_create(&tid, NULL, worker_thread, args) != 0) {             // Create the worker thread
            perror("pthread_create");
            free(args);                                                         // Clean up on failure
            return -1;
        }
        /*
         * Do NOT detach. We instead keep the thread handle so we can join it on shutdown.
         * This ensures a clean shutdown where the kernel resources are reclaimed after thread exit.
         */
        if (add_worker(tid) != 0) {
            pthread_detach(tid);                                                // If we cannot track the thread, detach it as a fallback to avoid leaks.
        }

        const char ok[] = "OK\n";
        (void)write_all(cfd, ok, sizeof(ok) - 1);                                // Send confirmation using write_all
        return 0;
    }
}

/**
 * @brief Helper: read a line (terminated by newline) from socket into buffer.
 * @description Reads from `fd` until a newline is found or buffer is full. Handles
 * EINTR and short reads correctly.
 * @param fd socket file descriptor to read from.
 * @param buf buffer to fill; will always be null-terminated on success.
 * @param sz size of buf (must be >= 2).
 * @return number of bytes placed into buf (excluding terminating NUL), or -1 on error.
 */
static ssize_t read_line(int fd, char *buf, size_t sz)
{
    if (sz < 2) return -1;
    size_t used = 0;
    while (used < sz - 1) {
        ssize_t n = read(fd, buf + used, 1);
        if (n == 0) {                                                           // EOF
            break;
        } else if (n < 0) {
            if (errno == EINTR) continue;                                       // try again
            return -1;                                                          // other error
        } else {
            if (buf[used] == '\r') {                                            // skip CR
                continue;
            }
            used++;
            if (buf[used - 1] == '\n') {                                        // newline terminator found
                break;
            }
        }
    }
    buf[used] = '\0';
    return (ssize_t)used;
}

/**
 * @brief Pthread entry point to handle a single client connection.
 * @description Reads a command from the client socket, passes it to the command
 * parser, and then closes the connection.
 * @param arg A pointer to the dynamically allocated client file descriptor (int).
 * The function takes ownership of this memory and frees it.
 * @return Always returns NULL.
 */
static void *client_thread(void *arg)
{
    int cfd = *(int *)arg;
    free(arg);
    char buf[CMD_BUF_SZ];
    ssize_t n = read_line(cfd, buf, sizeof(buf));
    if (n > 0) {
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';                     // strip trailing newline for robust parsing
        if (n > 1 && buf[n - 2] == '\r') buf[n - 2] = '\0';

        if (parse_command(cfd, buf) < 0) {                                      // If command parsing fails, send a helpful error message
            const char err[] = "ERR syntax (use: ZONE=<1..14> TIME=<seconds>)\n";
            (void)write_all(cfd, err, sizeof(err) - 1);
        }
    } else if (n == 0) {
        // no data from client; ignore quietly
    } else {
        perror("read");
    }
    close(cfd);                                                                 // Ensure the client connection is always closed
    return NULL;
}

// Signal handler: close the listen socket and mark running=0.
// Only uses async-signal-safe functions (close, write to STDERR_FILENO if needed).
 
static void shutdown_signal_handler(int signo)
{
    (void)signo;
    running = 0;
    if (listen_fd >= 0) {
        close(listen_fd);                                                       // close is async-signal-safe per POSIX
        listen_fd = -1;
    }
}

/* Join tracked worker threads and perform final cleanup:
 *  - join worker threads recorded in worker_list
 *  - free worker_list
 *  - ensure all zones are turned off
 *  - close mcp i2c and other resources
 */
static void join_workers_and_cleanup(void)
{
    /* Join worker threads */
    if (pthread_mutex_lock(&workers_lock) == 0) {
        for (size_t i = 0; i < worker_count; ++i) {
            pthread_join(worker_list[i], NULL);
        }
        free(worker_list);
        worker_list = NULL;
        worker_count = 0;
        worker_capacity = 0;
        pthread_mutex_unlock(&workers_lock);
    }

    /* Turn off all zones (ensure hardware and software mirrors are consistent) */
    mcp_lock();
    for (int z = 1; z <= MAX_ZONE; ++z) {
        if (zone_state[z] != 0) {                                               // Only change if currently ON to reduce I2C traffic
            mcp_set_zone_state(z, 0);
            zone_state[z] = 0;
        }
    }
    mcp_unlock();
    mcp_i2c_close();                                                            // Close I2C bus
}

/**
 * @brief Main program entry point.
 * @description Initializes the MCP23017 hardware, sets up a TCP listening
 * socket, and enters an infinite loop to accept and handle
 * incoming client connections. Each client is handled in a
 * separate thread which is tracked for orderly shutdown.
 * @return 0 on successful shutdown, or EXIT_FAILURE on a fatal initialization error.
 */
int main(void)
{
    /* Install signal handlers for graceful shutdown */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* --- Hardware Initialization --- */
    if (mcp_i2c_open("/dev/i2c-1") < 0) {
        fprintf(stderr, "ERROR: Failed to open I2C bus\n");
        exit(EXIT_FAILURE);
    }
    if (mcp_config_outputs() < 0) {
        fprintf(stderr, "ERROR: Failed to configure MCP23017 outputs\n");
        mcp_i2c_close();
        exit(EXIT_FAILURE);
    }
    mcp_enable_thread_safety(&zone_lock);                                       // Share the main application's mutex with the hardware driver for thread safety

    /* --- TCP Server Setup --- */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        mcp_i2c_close();
        exit(EXIT_FAILURE);
    }

    // Allow immediate reuse of the port after the daemon is restarted
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(listen_fd);
        mcp_i2c_close();
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(SERVER_PORT);
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

    /* --- Main Accept Loop --- */
    while (running) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);

        int *cfd = malloc(sizeof(int));
        if (!cfd) {
            perror("malloc");
            sleep(1);                                                           // If malloc fails, short sleep to avoid tight loop then continue
            continue;
        }

        *cfd = accept(listen_fd, (struct sockaddr *)&cli, &len);
        if (*cfd < 0) {
            int err = errno;
            free(cfd);
            if (!running) break;                                                // shutdown requested
            if (err == EINTR) continue;                                         // interrupted by signal
            perror("accept");
            continue;
        }

        // Create a new thread to handle the client connection.
        // We do not detach: threads are tracked and joined at shutdown.
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, cfd) != 0) {
            perror("pthread_create");
            close(*cfd);                                                        // Clean up on failure
            free(cfd);
            continue;
        }
        if (add_worker(tid) != 0) {
            pthread_detach(tid);                                                // If we cannot track this thread, detach as a fallback to avoid leaks.
        }
    }

    if (listen_fd >= 0) {
        close(listen_fd);                                                       // Listen socket may already be closed by signal handler; ensure it's closed
        listen_fd = -1;
    }
    join_workers_and_cleanup();                                                 // Shutdown: join threads and cleanup hardware

    return 0;
}