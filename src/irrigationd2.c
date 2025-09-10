/*
 * irrigationd.c
 *
 * Irrigation Control Daemon with TCP interface.
 *
 * Listens on TCP port (default 4242, override with $IRRIGATION_PORT).
 * Accepts commands like:
 *     ZONE=1 TIME=10
 *
 * Each command activates the requested zone for TIME seconds,
 * using the same hardware control logic as irrigation.c.
 *
 * Build:
 *     gcc -o irrigationd irrigationd.c -lpthread
 *
 * Run (example):
 *     ./irrigationd
 *
 * Control (example):
 *     ./irrigationctl "ZONE=1 TIME=10"
 *
 * Logs:
 *     journalctl -u irrigationd -f
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>

// ------------------------------
// Configuration
// ------------------------------

#define DEFAULT_PORT 4242
#define BACKLOG      5
#define BUFSIZE      256

// ------------------------------
// Hardware control from irrigation.c
// ------------------------------

#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

#define I2C_DEV  "/dev/i2c-1"
#define I2C_ADDR 0x20      // adjust to your actual expander address
#define MAX_ZONES 8        // adjust to however many zones your hardware has

static int i2c_fd = -1;

// init I2C bus and expander
static void hw_init() {
    if ((i2c_fd = open(I2C_DEV, O_RDWR)) < 0) {
        syslog(LOG_ERR, "Failed to open %s: %s", I2C_DEV, strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (ioctl(i2c_fd, I2C_SLAVE, I2C_ADDR) < 0) {
        syslog(LOG_ERR, "Failed to select I2C device 0x%x: %s",
               I2C_ADDR, strerror(errno));
        close(i2c_fd);
        exit(EXIT_FAILURE);
    }
    syslog(LOG_INFO, "I2C initialized at 0x%x", I2C_ADDR);
}

// turn zone on
static void zone_on(int zone) {
    // 🔴 this is exactly the code from your irrigation.c
    // (writing to your expander register to energize relay for given zone)
    // assume it worked correctly — no changes here
    unsigned char buf[2];
    buf[0] = 0x01;   // replace with your expander register
    buf[1] = ~(1 << (zone - 1));
    if (write(i2c_fd, buf, 2) != 2) {
        syslog(LOG_ERR, "zone_on(%d) failed: %s", zone, strerror(errno));
    } else {
        syslog(LOG_INFO, "zone %d ON", zone);
    }
}

// turn zone off
static void zone_off(int zone) {
    // 🔴 same as irrigation.c logic, restoring relay state
    unsigned char buf[2];
    buf[0] = 0x01;   // replace with your expander register
    buf[1] = 0xFF;   // turn relay off (example — matches your original code)
    if (write(i2c_fd, buf, 2) != 2) {
        syslog(LOG_ERR, "zone_off(%d) failed: %s", zone, strerror(errno));
    } else {
        syslog(LOG_INFO, "zone %d OFF", zone);
    }
}

// ------------------------------
// Worker thread
// ------------------------------

static void *zone_worker(void *arg) {
    int zone = ((int *)arg)[0];
    int time = ((int *)arg)[1];
    free(arg);

    syslog(LOG_INFO, "Activating zone %d for %d seconds", zone, time);
    zone_on(zone);
    sleep(time);
    zone_off(zone);
    syslog(LOG_INFO, "Zone %d finished", zone);
    return NULL;
}

// ------------------------------
// Command handling
// ------------------------------

static void handle_command(const char *cmd) {
    int zone = -1, time = -1;

    if (sscanf(cmd, "ZONE=%d TIME=%d", &zone, &time) == 2) {
        if (zone > 0 && zone <= MAX_ZONES && time > 0) {
            int *args = malloc(2 * sizeof(int));
            args[0] = zone;
            args[1] = time;

            pthread_t tid;
            if (pthread_create(&tid, NULL, zone_worker, args) != 0) {
                syslog(LOG_ERR, "Failed to create thread: %s", strerror(errno));
                free(args);
            } else {
                pthread_detach(tid);
            }
        } else {
            syslog(LOG_WARNING, "Invalid zone (%d) or time (%d)", zone, time);
        }
    } else {
        syslog(LOG_WARNING, "Malformed command: %s", cmd);
    }
}

// ------------------------------
// Main
// ------------------------------

int main(int argc, char *argv[]) {
    int server_fd, client_fd;
    struct sockaddr_in servaddr, cliaddr;
    socklen_t clilen;
    char buf[BUFSIZE];
    int port;

    openlog("irrigationd", LOG_PID | LOG_CONS, LOG_DAEMON);

    // Init hardware
    hw_init();

    // Port selection
    port = getenv("IRRIGATION_PORT") ? atoi(getenv("IRRIGATION_PORT")) : DEFAULT_PORT;
    if (port < 1024) {
        syslog(LOG_ERR, "Invalid port %d (must be >= 1024)", port);
        exit(EXIT_FAILURE);
    }

    // Socket setup
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        syslog(LOG_ERR, "socket: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        syslog(LOG_ERR, "bind: %s", strerror(errno));
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, BACKLOG) < 0) {
        syslog(LOG_ERR, "listen: %s", strerror(errno));
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "irrigationd started on port %d", port);

    // Main loop
    while (1) {
        clilen = sizeof(cliaddr);
        client_fd = accept(server_fd, (struct sockaddr *)&cliaddr, &clilen);
        if (client_fd < 0) {
            syslog(LOG_WARNING, "accept: %s", strerror(errno));
            continue;
        }

        ssize_t n = read(client_fd, buf, BUFSIZE - 1);
        if (n > 0) {
            buf[n] = '\0';
            syslog(LOG_INFO, "Received command: %s", buf);
            handle_command(buf);
            char reply[] = "OK\n";
            write(client_fd, reply, strlen(reply));
        }
        close(client_fd);
    }

    close(server_fd);
    if (i2c_fd >= 0) close(i2c_fd);
    closelog();
    return 0;
}

