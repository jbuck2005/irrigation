/*
 * mcp23017.c                                                                                     // Implementation of MCP23017 irrigation helpers
 *
 * This file isolates all MCP23017-specific logic so that both irrigation.c                        //
 * (stand-alone tool) and irrigationd.c (daemon) share the same trusted code.                      //
 *
 * Features:                                                                                      // - I²C open/close, read/write
 *                                                                                                // - Configure all pins as outputs
 *                                                                                                // - Zone mapping 1..14 → MCP register/bit
 *                                                                                                // - Set zone state with optional thread safety
 *                                                                                                // - Debug + syslog integration for visibility
 */

#include <stdio.h>                                                                                // fprintf, perror
#include <stdlib.h>                                                                               // exit
#include <stdint.h>                                                                               // uint8_t
#include <unistd.h>                                                                               // read, write, close
#include <fcntl.h>                                                                                // open
#include <errno.h>                                                                                // errno
#include <string.h>                                                                               // strerror
#include <sys/ioctl.h>                                                                            // ioctl
#include <linux/i2c-dev.h>                                                                        // I²C definitions
#include <pthread.h>                                                                              // pthread_mutex_t
#include <syslog.h>                                                                               // syslog logging

#include "mcp23017.h"                                                                             // Public header

// ------------------------------ Globals --------------------------------------------------------

static int g_fd = -1;                                                                             // I²C file descriptor (active bus handle)
static pthread_mutex_t *g_mutex = NULL;                                                           // Optional external mutex for thread safety
static uint8_t g_debug = 0;                                                                       // Debug flag (set manually or via env)

// ------------------------------ I²C helpers ----------------------------------------------------

int mcp_i2c_open(const char *devnode) {                                                           // Open I²C bus and select MCP23017
    g_fd = open(devnode, O_RDWR);                                                                 // Open char device
    if (g_fd < 0) {                                                                               // Error check
        perror("open(i2c)");                                                                      // Log to stderr
        syslog(LOG_ERR, "open(%s): %s", devnode, strerror(errno));                                // Log to syslog
        return -1;                                                                                // Fail
    }
    if (ioctl(g_fd, I2C_SLAVE, MCPADDR) < 0) {                                                    // Set slave address
        perror("ioctl(I2C_SLAVE)");                                                               // Log to stderr
        syslog(LOG_ERR, "ioctl(I2C_SLAVE 0x%02x): %s", MCPADDR, strerror(errno));                  // Log to syslog
        close(g_fd);                                                                              // Cleanup
        g_fd = -1;                                                                                // Invalidate
        return -1;                                                                                // Fail
    }
    return 0;                                                                                     // Success
}

void mcp_i2c_close(void) {                                                                        // Close I²C device
    if (g_fd >= 0) close(g_fd);                                                                   // Close fd if valid
    g_fd = -1;                                                                                    // Invalidate
}

static int mcp_write(uint8_t reg, uint8_t val) {                                                  // Write one MCP register
    uint8_t buf[2] = { reg, val };                                                                // [register, value]
    if (write(g_fd, buf, 2) != 2) {                                                               // Must write 2 bytes
        perror("mcp_write");                                                                      // Log to stderr
        syslog(LOG_ERR, "mcp_write(0x%02x,0x%02x): %s", reg, val, strerror(errno));                // Log to syslog
        return -1;                                                                                // Fail
    }
    return 0;                                                                                     // Success
}

static int mcp_read(uint8_t reg, uint8_t *out) {                                                  // Read one MCP register
    if (write(g_fd, &reg, 1) != 1) {                                                              // Prime register pointer
        perror("mcp_addr");                                                                       // Log to stderr
        syslog(LOG_ERR, "mcp_addr(0x%02x): %s", reg, strerror(errno));                            // Log to syslog
        return -1;                                                                                // Fail
    }
    if (read(g_fd, out, 1) != 1) {                                                                // Read back one byte
        perror("mcp_read");                                                                       // Log to stderr
        syslog(LOG_ERR, "mcp_read(0x%02x): %s", reg, strerror(errno));                            // Log to syslog
        return -1;                                                                                // Fail
    }
    if (g_debug) {                                                                                // Optional debug
        fprintf(stderr, "read[0x%02x] => 0x%02x\n", reg, *out);                                   // To stderr
        syslog(LOG_DEBUG, "read[0x%02x] => 0x%02x", reg, *out);                                   // To syslog
    }
    return 0;                                                                                     // Success
}

int mcp_config_outputs(void) {                                                                    // Make all pins outputs
    if (mcp_write(IODIRA, 0x00)) return -1;                                                       // All port A pins = outputs
    if (mcp_write(IODIRB, 0x00)) return -1;                                                       // All port B pins = outputs
    return 0;                                                                                     // Success
}

// ------------------------------ Zone mapping ---------------------------------------------------

int mcp_map_zone(int zone, uint8_t *reg, uint8_t *mask) {                                         // Zone → (register, bitmask)
    if (zone >= 1 && zone <= 8) {                                                                 // Zones 1..8 → GPIOA bits 0..7
        *reg  = GPIOA;                                                                            // Register = GPIOA
        *mask = (uint8_t)(1u << (zone - 1));                                                      // Bitmask = bitN
        return 0;                                                                                 // Valid
    } else if (zone >= 9 && zone <= 14) {                                                         // Zones 9..14 → GPIOB reversed mapping
        *reg = GPIOB;                                                                             // Register = GPIOB
        static const uint8_t bmap[15] = {                                                         // Reverse mapping table
            0,0,0,0,0,0,0,0,0,                                                                     // 0..8 placeholders
            (1u<<5), (1u<<4), (1u<<3), (1u<<2), (1u<<1), (1u<<0)                                  // 9..14 → bits 5..0
        };
        *mask = bmap[zone];                                                                       // Lookup bitmask
        return (*mask == 0) ? -1 : 0;                                                             // Validate
    } else {                                                                                      // Out of range
        return -1;                                                                                // Invalid zone
    }
}

// ------------------------------ Zone control ---------------------------------------------------

int mcp_set_zone_state(int zone, int on) {                                                        // Set a zone ON (1) or OFF (0)
    uint8_t reg, mask;                                                                            // Target register/bit
    if (mcp_map_zone(zone, &reg, &mask)) {                                                        // Validate zone
        fprintf(stderr, "Invalid zone: %d\n", zone);                                              // Log to stderr
        syslog(LOG_ERR, "Invalid zone: %d", zone);                                                // Log to syslog
        return -1;                                                                                // Fail
    }

    if (g_mutex) pthread_mutex_lock(g_mutex);                                                     // Lock critical section if multithreaded

    uint8_t val;                                                                                  // Current register value
    if (mcp_read(reg, &val)) {                                                                    // Read register
        if (g_mutex) pthread_mutex_unlock(g_mutex);                                               // Unlock if locked
        syslog(LOG_ERR, "mcp_read failed (zone %d)", zone);                                       // Log to syslog
        return -1;                                                                                // Fail
    }
    uint8_t newval = on ? (val | mask) : (val & ~mask);                                           // Apply change
    if (g_debug) {                                                                                // Optional debug
        fprintf(stderr, "zone %d -> %s: 0x%02x -> 0x%02x (reg 0x%02x)\n",                         // To stderr
                zone, on ? "ON" : "OFF", val, newval, reg);
        syslog(LOG_DEBUG, "zone %d -> %s: 0x%02x -> 0x%02x (reg 0x%02x)",                         // To syslog
               zone, on ? "ON" : "OFF", val, newval, reg);
    }
    int rc = mcp_write(reg, newval);                                                              // Write back new value
    if (rc != 0) syslog(LOG_ERR, "mcp_write failed (zone %d)", zone);                             // Log error

    if (g_mutex) pthread_mutex_unlock(g_mutex);                                                   // Unlock critical section
    return rc;                                                                                    // Return result
}

// ------------------------------ Thread-safety setup --------------------------------------------

void mcp_enable_thread_safety(pthread_mutex_t *external_mutex) {                                  // Enable thread safety
    g_mutex = external_mutex;                                                                     // Store pointer to external mutex
}
