// irrigationd.c - Irrigation Controller Daemon
// ----------------------------------------------------
// This daemon listens on TCP port 4242 for commands
// like:  ZONE=1 TIME=30 TOKEN=changeme-very-secret-token
//
// It now brings up the TCP listener first, then
// initializes I²C in a background thread, so the
// service is always responsive even if hardware is slow.
//
// ----------------------------------------------------

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

// ------------------------------------------------------------------
// Configuration from environment
// ------------------------------------------------------------------
static const char *listen_addr;
static const char *auth_token;

// I²C globals
static int i2c_fd = -1;
static int i2c_ready = 0;
pthread_mutex_t i2c_lock = PTHREAD_MUTEX_INITIALIZER;

// ------------------------------------------------------------------
// Signal handling
// ------------------------------------------------------------------
static volatile int running = 1;

void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

// ------------------------------------------------------------------
// Simple command parser (stub for demo)
// ------------------------------------------------------------------
static void handle_command(int client_fd, const char *cmd) {
    if (auth_token && !strstr(cmd, auth_token)) {
        dprintf(client_fd, "ERR unauthorized\n");
        return;
    }

    // Example: "ZONE=1 TIME=10"
    if (strstr(cmd, "ZONE=") && strstr(cmd, "TIME=")) {
        dprintf(client_fd, "OK scheduling command: %s\n", cmd);

        // If I²C not ready yet, warn but don’t crash
        pthread_mutex_lock(&i2c_lock);
        if (i2c_ready) {
            // TODO: send actual command to MCP23017
            fprintf(stderr, "[I2C] would execute: %s\n", cmd);
        } else {
            fprintf(stderr, "[I2C] not ready, ignoring: %s\n", cmd);
        }
        pthread_mutex_unlock(&i2c_lock);

    } else {
        dprintf(client_fd, "ERR malformed command\n");
    }
}

// ------------------------------------------------------------------
// Networking loop
// ------------------------------------------------------------------
static void *network_loop(void *arg) {
    int server_fd = *(int *)arg;

    while (running) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);

        int client_fd = accept(server_fd, (struct sockaddr *)&cliaddr, &clilen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        char buf[512];
        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            handle_command(client_fd, buf);
        }
        close(client_fd);
    }

    close(server_fd);
    return NULL;
}

// ------------------------------------------------------------------
// Background I²C init thread
// ------------------------------------------------------------------
static void *i2c_init_thread(void *arg) {
    (void)arg;

    fprintf(stderr, "[I2C] initializing...\n");

    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd < 0) {
        perror("[I2C] open");
        return NULL;
    }

    // Test communication with MCP23017 at address 0x20
    if (ioctl(fd, I2C_SLAVE, 0x20) < 0) {
        perror("[I2C] ioctl");
        close(fd);
        return NULL;
    }

    pthread_mutex_lock(&i2c_lock);
    i2c_fd = fd;
    i2c_ready = 1;
    pthread_mutex_unlock(&i2c_lock);

    fprintf(stderr, "[I2C] ready\n");
    return NULL;
}

// ------------------------------------------------------------------
// Main
// ------------------------------------------------------------------
int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    auth_token = getenv("IRRIGATIOND_TOKEN");
    listen_addr = getenv("IRRIGATIOND_BIND_ADDR");
    if (!auth_token || !listen_addr) {
        fprintf(stderr, "IRRIGATIOND_TOKEN and IRRIGATIOND_BIND_ADDR must be set\n");
        return 1;
    }

    // ------------------ Setup TCP listener ------------------
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(4242);
    if (inet_pton(AF_INET, listen_addr, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        return 1;
    }

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        return 1;
    }

    fprintf(stderr, "irrigationd listening on %s:4242\n", listen_addr);

    // ------------------ Start networking thread ------------------
    pthread_t net_thread;
    if (pthread_create(&net_thread, NULL, network_loop, &server_fd) != 0) {
        perror("pthread_create net_thread");
        return 1;
    }

    // ------------------ Start I²C init thread ------------------
    pthread_t i2c_thread;
    if (pthread_create(&i2c_thread, NULL, i2c_init_thread, NULL) != 0) {
        perror("pthread_create i2c_thread");
        return 1;
    }

    // ------------------ Wait for shutdown ------------------
    pthread_join(net_thread, NULL);
    pthread_join(i2c_thread, NULL);

    pthread_mutex_lock(&i2c_lock);
    if (i2c_fd >= 0) close(i2c_fd);
    pthread_mutex_unlock(&i2c_lock);

    fprintf(stderr, "irrigationd shutting down\n");
    return 0;
}

