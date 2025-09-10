/*
 * irrigationd.c – Irrigation Control Daemon (with MCP23017 backend)
 * -----------------------------------------------------------------------------------------------
 * Listens on TCP port 4242 and accepts commands in the form:
 *
 *     ZONE=<n> TIME=<seconds>
 *
 * Where:
 *     - <n> is the irrigation zone number (1..14).
 *     - <seconds> is the duration to activate the zone.
 *
 * Behavior:
 *     - If TIME > 0, the zone is turned ON for <seconds>, then turned OFF automatically.
 *     - If TIME = 0, the zone is turned OFF immediately (NEW FEATURE).
 *
 * Example usage:
 *     echo "ZONE=1 TIME=30" | nc <host> 4242    # Turn ON zone 1 for 30 seconds
 *     echo "ZONE=1 TIME=0"  | nc <host> 4242    # Turn OFF zone 1 immediately
 *
 * Requires: mcp23017.c / mcp23017.h (driver for MCP23017 I²C expander).
 * -----------------------------------------------------------------------------------------------
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

#include "mcp23017.h"                                                                   /* MCP23017 driver */

#define MAX_ZONE        14                                                               /* Maximum zones   */
#define SERVER_PORT     4242                                                             /* TCP port        */
#define BACKLOG         5                                                                /* Listen backlog  */

static int zone_state[MAX_ZONE + 1];                                                     /* Software state  */
static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;                            /* Protects state  */

/* ------------------------------------------------------------------------------------------------
 * set_zone_state() – Turn a zone ON or OFF (thread-safe, writes MCP23017).
 * ------------------------------------------------------------------------------------------------
 */
static void set_zone_state(int zone, int state)
{
    pthread_mutex_lock(&zone_lock);
    zone_state[zone] = state;
    pthread_mutex_unlock(&zone_lock);

    if (mcp_set_zone_state(zone, state) != 0) {                                          /* Write hardware  */
        fprintf(stderr, "ERROR: Failed to set zone %d -> %s\n", zone, state ? "ON" : "OFF");
    }

    printf("Zone %d -> %s\n", zone, state ? "ON" : "OFF");                               /* Debug log       */
}

/* ------------------------------------------------------------------------------------------------
 * worker_thread() – Activates a zone for a fixed duration, then turns it OFF.
 * ------------------------------------------------------------------------------------------------
 */
static void *worker_thread(void *arg)
{
    int zone   = ((int *)arg)[0];
    int time_s = ((int *)arg)[1];
    free(arg);

    set_zone_state(zone, 1);                                                             /* Turn ON         */
    sleep(time_s);                                                                       /* Wait duration   */
    set_zone_state(zone, 0);                                                             /* Turn OFF        */

    return NULL;
}

/* ------------------------------------------------------------------------------------------------
 * parse_command() – Parse a command line, update zones accordingly.
 * Returns 0 on success, -1 on syntax error.
 * ------------------------------------------------------------------------------------------------
 */
static int parse_command(int cfd, const char *line)
{
    int z, t;

    /* Expect command format: "ZONE=<n> TIME=<seconds>" */
    if (sscanf(line, "ZONE=%d TIME=%d", &z, &t) != 2) {
        return -1;
    }

    if (z < 1 || z > MAX_ZONE || t < 0) {                                                /* Validate input  */
        return -1;
    }

    if (t == 0) {
        /* ----------------------------------------------------------------------------------------
         * TIME == 0 → Turn OFF the zone immediately, no worker thread created.
         * ----------------------------------------------------------------------------------------
         */
        set_zone_state(z, 0);
        const char ok[] = "OK\n";
        (void)write(cfd, ok, sizeof(ok) - 1);
        return 0;
    } else {
        /* ----------------------------------------------------------------------------------------
         * TIME > 0 → Spawn worker thread to activate the zone for <t> seconds.
         * ----------------------------------------------------------------------------------------
         */
        int *args = malloc(2 * sizeof(int));
        if (!args) {
            perror("malloc");
            return -1;
        }
        args[0] = z;
        args[1] = t;

        pthread_t tid;
        if (pthread_create(&tid, NULL, worker_thread, args) != 0) {
            perror("pthread_create");
            free(args);
            return -1;
        }
        pthread_detach(tid);

        const char ok[] = "OK\n";
        (void)write(cfd, ok, sizeof(ok) - 1);
        return 0;
    }
}

/* ------------------------------------------------------------------------------------------------
 * client_thread() – Handles a single TCP client.
 * ------------------------------------------------------------------------------------------------
 */
static void *client_thread(void *arg)
{
    int cfd = *(int *)arg;
    free(arg);

    char buf[128];
    ssize_t n = read(cfd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (parse_command(cfd, buf) < 0) {
            const char err[] = "ERR syntax (use: ZONE=<1..14> TIME=<seconds>)\n";
            (void)write(cfd, err, sizeof(err) - 1);
        }
    }
    close(cfd);
    return NULL;
}

/* ------------------------------------------------------------------------------------------------
 * main() – Daemon entry point.
 * ------------------------------------------------------------------------------------------------
 */
int main(void)
{
    /* --------------------------------------------------------------------------------------------
     * Initialize MCP23017 hardware before accepting any commands.
     * --------------------------------------------------------------------------------------------
     */
    if (mcp_i2c_open("/dev/i2c-1") < 0) {                                                /* Open I²C bus    */
        fprintf(stderr, "ERROR: Failed to open I2C bus\n");
        exit(EXIT_FAILURE);
    }
    if (mcp_config_outputs() < 0) {                                                      /* Configure pins  */
        fprintf(stderr, "ERROR: Failed to configure MCP23017 outputs\n");
        exit(EXIT_FAILURE);
    }
    mcp_enable_thread_safety(&zone_lock);                                                /* Share mutex     */

    /* --------------------------------------------------------------------------------------------
     * Standard TCP server setup.
     * --------------------------------------------------------------------------------------------
     */
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

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

    /* --------------------------------------------------------------------------------------------
     * Accept clients in a loop, dispatch each to a new thread.
     * --------------------------------------------------------------------------------------------
     */
    while (1) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
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

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, cfd) != 0) {
            perror("pthread_create");
            close(*cfd);
            free(cfd);
            continue;
        }
        pthread_detach(tid);
    }

    /* --------------------------------------------------------------------------------------------
     * Clean shutdown (rarely reached).
     * --------------------------------------------------------------------------------------------
     */
    mcp_i2c_close();
    close(sfd);
    return 0;
}
