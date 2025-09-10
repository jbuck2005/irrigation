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

#include "mcp23017.h"                                                           // MCP23017 driver

#define MAX_ZONE        14                                                      // Maximum number of irrigation zones
#define SERVER_PORT     4242                                                    // TCP port for listening to commands
#define BACKLOG         5                                                       // Connection queue size for listen()

/* --- Global State --- */
static int zone_state[MAX_ZONE + 1];                                            // Software mirror of zone states (0=OFF, 1=ON)
static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;                   // Mutex to protect access to zone_state and hardware

/**
 * @brief Sets the state of a specific irrigation zone.
 * @description This function updates both the software state array and the
 * physical hardware state via the MCP23017 driver. It is
 * thread-safe, using a mutex to protect the shared zone_state array.
 * @param zone The zone number (1-based index).
 * @param state The desired state: 1 for ON, 0 for OFF.
 * @note This function assumes that the underlying `mcp_set_zone_state` function
 * is also thread-safe, which is configured in `main()` by passing it a
 * pointer to the `zone_lock` mutex.
 */
static void set_zone_state(int zone, int state)
{
    pthread_mutex_lock(&zone_lock);                                             // Lock the mutex to protect shared state
    zone_state[zone] = state;
    pthread_mutex_unlock(&zone_lock);

    // Update the physical hardware state. This call is made outside the lock
    // on this side, as the MCP driver is responsible for its own locking.
    if (mcp_set_zone_state(zone, state) != 0) {
        fprintf(stderr, "ERROR: Failed to set zone %d -> %s\n", zone, state ? "ON" : "OFF");
    }
    printf("Zone %d -> %s\n", zone, state ? "ON" : "OFF");
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
        (void)write(cfd, ok, sizeof(ok) - 1);                                   // Send confirmation
        return 0;
    } else {                                                                    // TIME>0: Spawn a detached worker thread to handle the timed activation.
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
        pthread_detach(tid);                                                    // Detach the thread so its resources are automatically freed on exit
        const char ok[] = "OK\n";
        (void)write(cfd, ok, sizeof(ok) - 1);                                   // Send confirmation
        return 0;
    }
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

    char buf[128];
    ssize_t n = read(cfd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';                                                          // Null-terminate the received data
        if (parse_command(cfd, buf) < 0) {                                      // If command parsing fails, send a helpful error message
            const char err[] = "ERR syntax (use: ZONE=<1..14> TIME=<seconds>)\n";
            (void)write(cfd, err, sizeof(err) - 1);
        }
    }
    close(cfd);                                                                 // Ensure the client connection is always closed
    return NULL;
}

/**
 * @brief Main program entry point.
 * @description Initializes the MCP23017 hardware, sets up a TCP listening
 * socket, and enters an infinite loop to accept and handle
 * incoming client connections. Each client is handled in a
 * separate, detached thread.
 * @return 0 on successful shutdown (practically unreachable), or
 * EXIT_FAILURE on a fatal initialization error.
 */
int main(void)
{
    // --- Hardware Initialization ---
    if (mcp_i2c_open("/dev/i2c-1") < 0) {
        fprintf(stderr, "ERROR: Failed to open I2C bus\n");
        exit(EXIT_FAILURE);
    }
    if (mcp_config_outputs() < 0) {
        fprintf(stderr, "ERROR: Failed to configure MCP23017 outputs\n");
        exit(EXIT_FAILURE);
    }
    mcp_enable_thread_safety(&zone_lock);                                       // Share the main application's mutex with the hardware driver for thread safety

    // --- TCP Server Setup ---
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Allow immediate reuse of the port after the daemon is restarted
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sfd);
        exit(EXIT_FAILURE);
    }

    if (listen(sfd, BACKLOG) < 0) {
        perror("listen");
        close(sfd);
        exit(EXIT_FAILURE);
    }

    printf("irrigationd listening on port %d\n", SERVER_PORT);

    // --- Main Accept Loop ---
    while (1) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        
        // Allocate memory for the client fd to pass to the new thread safely
        int *cfd = malloc(sizeof(int));
        if (!cfd) {
            perror("malloc");
            continue;
        }
        
        *cfd = accept(sfd, (struct sockaddr *)&cli, &len);
        if (*cfd < 0) {
            perror("accept");
            free(cfd);
            continue;
        }

        // Create a new thread to handle the client connection
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, cfd) != 0) {
            perror("pthread_create");
            close(*cfd);                                                        // Clean up on failure
            free(cfd);
            continue;
        }
        pthread_detach(tid);                                                    // Detach the thread so its resources are automatically freed upon exit
    }

    // --- Clean Shutdown (rarely reached in a daemon) ---
    mcp_i2c_close();
    close(sfd);
    return 0;
}