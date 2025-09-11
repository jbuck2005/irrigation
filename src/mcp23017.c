/**
 * @file mcp23017.c
 * @brief Driver for the MCP23017 I2C I/O expander for irrigation control
 *
 * This module provides a safe and documented interface to the MCP23017
 * 16-bit I/O expander, used in our irrigation controller to drive zone relays.
 *
 * Features:
 *   - Open and configure the I²C device safely
 *   - Configure expander ports as outputs
 *   - Map logical irrigation zones (1..14) to MCP23017 GPIO pins
 *   - Set zone state ON/OFF with proper register updates
 *   - Optional thread safety via external mutex integration
 *   - Integrated syslog logging for errors, warnings, and debug output
 *
 * Design Notes:
 *   - All low-level I²C operations are wrapped in helper functions
 *   - Errors are reported both to stderr (for dev/debug) and syslog
 *   - File descriptors are marked FD_CLOEXEC to avoid leaking across exec()
 *   - If IRRIGATIOND_DEBUG is set in the environment, verbose debug logging is enabled
 */

#include <stdio.h>                                                              // Standard I/O (fprintf, perror)
#include <stdlib.h>                                                             // General utilities (exit, getenv)
#include <stdint.h>                                                             // Fixed-width integer types (uint8_t, etc.)
#include <unistd.h>                                                             // POSIX API (read, write, close)
#include <fcntl.h>                                                              // File control (open, fcntl)
#include <errno.h>                                                              // Access to errno for error reporting
#include <string.h>                                                             // String handling (strerror, memcpy)
#include <sys/ioctl.h>                                                          // ioctl() for device configuration
#include <linux/i2c-dev.h>                                                      // I²C-specific ioctl definitions
#include <pthread.h>                                                            // For optional mutex-based thread safety
#include <syslog.h>                                                             // Syslog logging (LOG_ERR, LOG_INFO, etc.)

#include "mcp23017.h"                                                           // Public declarations for MCP23017 interface

// --- Global State ---
// We keep minimal state here. All functions operate on the file descriptor
// and optionally use a caller-provided mutex for thread safety.

static int g_fd = -1;                                                           // Active file descriptor for /dev/i2c-X
static pthread_mutex_t *g_mutex = NULL;                                         // External mutex pointer (set by irrigationd)
static uint8_t g_debug = 0;                                                     // Debug flag (enabled if IRRIGATIOND_DEBUG is set)

// Utility: set FD_CLOEXEC so child processes won’t inherit our I²C FD
static void set_cloexec(int fd) {
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) return;
    (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// Open the I²C device node and configure for MCP23017 access
int mcp_i2c_open(const char *devnode) {
    openlog("mcp23017", LOG_PID | LOG_CONS, LOG_DAEMON);                        // Open syslog connection tagged “mcp23017”

    g_fd = open(devnode, O_RDWR);
    if (g_fd < 0) {
        perror("open(i2c)");
        syslog(LOG_ERR, "Failed to open I2C bus %s: %s", devnode, strerror(errno));
        return -1;
    }

    set_cloexec(g_fd);                                                          // Ensure FD does not leak across exec()

    if (ioctl(g_fd, I2C_SLAVE, MCPADDR) < 0) {                                  // Bind the FD to MCP23017’s I²C address
        perror("ioctl(I2C_SLAVE)");
        syslog(LOG_ERR, "Failed to set I2C slave address 0x%02x: %s",
               MCPADDR, strerror(errno));
        close(g_fd);
        g_fd = -1;
        return -1;
    }

    if (getenv("IRRIGATIOND_DEBUG")) {                                          // Debug enabled via environment variable
        g_debug = 1;
        syslog(LOG_INFO, "MCP23017 debug enabled via IRRIGATIOND_DEBUG");
    }

    return 0;
}

// Close the I²C device and release resources
void mcp_i2c_close(void) {
    if (g_fd >= 0) {
        close(g_fd);
    }
    g_fd = -1;

    closelog();                                                                 // Close syslog connection on shutdown
}

// Enable/disable locking with external mutex
void mcp_lock(void) {
    if (g_mutex) pthread_mutex_lock(g_mutex);
}

void mcp_unlock(void) {
    if (g_mutex) pthread_mutex_unlock(g_mutex);
}

// Write a single byte to a register
static int mcp_write(uint8_t reg, uint8_t val) {
    if (g_fd < 0) {
        syslog(LOG_ERR, "mcp_write called with invalid file descriptor");
        return -1;
    }

    uint8_t buf[2] = { reg, val };
    ssize_t w = write(g_fd, buf, 2);
    if (w != 2) {
        if (w < 0) perror("mcp_write");
        syslog(LOG_ERR, "I2C write failed to reg 0x%02x: %s", reg, strerror(errno));
        return -1;
    }
    return 0;
}

// Read a single byte from a register
static int mcp_read(uint8_t reg, uint8_t *out) {
    if (g_fd < 0) {
        syslog(LOG_ERR, "mcp_read called with invalid file descriptor");
        return -1;
    }

    uint8_t r = reg;
    if (write(g_fd, &r, 1) != 1) {
        perror("mcp_read (set addr)");
        syslog(LOG_ERR, "I2C failed to set read address to 0x%02x: %s", reg, strerror(errno));
        return -1;
    }

    if (read(g_fd, out, 1) != 1) {
        perror("mcp_read (read data)");
        syslog(LOG_ERR, "I2C read failed from reg 0x%02x: %s", reg, strerror(errno));
        return -1;
    }

    if (g_debug) {
        fprintf(stderr, "read[0x%02x] => 0x%02x\n", reg, *out);
        syslog(LOG_DEBUG, "read[0x%02x] => 0x%02x", reg, *out);
    }
    return 0;
}

// Configure both GPIO ports as outputs
int mcp_config_outputs(void) {
    if (mcp_write(IODIRA, 0x00)) return -1;                                     // All A pins as outputs
    if (mcp_write(IODIRB, 0x00)) return -1;                                     // All B pins as outputs
    return 0;
}

// Map logical zone number to MCP23017 register and bit mask
int mcp_map_zone(int zone, uint8_t *reg, uint8_t *mask) {
    if (zone >= 1 && zone <= 8) {
        *reg  = GPIOA;                                                          // Zones 1–8 map to GPIOA pins 0–7
        *mask = (uint8_t)(1u << (zone - 1));
        return 0;
    } else if (zone >= 9 && zone <= 14) {
        *reg = GPIOB;                                                           // Zones 9–14 map to GPIOB pins 5–0
        static const uint8_t bmap[] = {
            (1u << 5), (1u << 4), (1u << 3),
            (1u << 2), (1u << 1), (1u << 0)
        };
        *mask = bmap[zone - 9];
        return 0;
    } else {
        return -1;                                                              // Invalid zone number
    }
}

// Set the output state (ON/OFF) for a logical zone
int mcp_set_zone_state(int zone, int on) {
    uint8_t reg, mask;
    if (mcp_map_zone(zone, &reg, &mask) != 0) {
        fprintf(stderr, "Invalid zone: %d\n", zone);
        syslog(LOG_ERR, "Attempted to set invalid zone: %d", zone);
        return -1;
    }

    int rc = -1;
    uint8_t current_val;
    if (mcp_read(reg, &current_val) == 0) {
        uint8_t new_val = on ? (current_val | mask) : (current_val & ~mask);

        if (g_debug) {
            fprintf(stderr, "zone %d -> %s: 0x%02x -> 0x%02x (reg 0x%02x)\n",
                    zone, on ? "ON" : "OFF", current_val, new_val, reg);
            syslog(LOG_DEBUG,
                   "zone %d -> %s: 0x%02x -> 0x%02x (reg 0x%02x)",
                   zone, on ? "ON" : "OFF", current_val, new_val, reg);
        }

        rc = mcp_write(reg, new_val);
        if (rc != 0) {
            syslog(LOG_ERR, "mcp_write failed during state set for zone %d", zone);
        }
    } else {
        syslog(LOG_ERR, "mcp_read failed during state set for zone %d", zone);
    }
    return rc;
}

// Allow caller (daemon) to provide a mutex for thread safety
void mcp_enable_thread_safety(pthread_mutex_t *external_mutex) {
    g_mutex = external_mutex;
}