/**
 * @file mcp23017.c
 * @author James Buck
 * @date September 10, 2025
 * @brief Driver for the MCP23017 I2C I/O expander for an irrigation system.
 *
 * @description
 * This file provides a hardware abstraction layer for the MCP23017. It handles low-level I2C communication, configuration,
 * and the specific logic for mapping irrigation zones to the expander's GPIO pins.
 *
 * It is designed to be shared by both standalone tools and daemons, ensuring consistent hardware control.
 * Features include optional thread-safety via an external mutex and syslog integration for robust logging.
 */

#include <stdio.h>                                                              // Provides standard I/O functions like fprintf and perror
#include <stdlib.h>                                                             // Provides general utilities like exit()
#include <stdint.h>                                                             // Provides fixed-width integer types like uint8_t for precise control
#include <unistd.h>                                                             // Provides POSIX operating system API, including read, write, and close
#include <fcntl.h>                                                              // Provides file control options, used here for open()
#include <errno.h>                                                              // Provides access to the global `errno` variable for error reporting
#include <string.h>                                                             // Provides string-handling functions, especially strerror to get error messages
#include <sys/ioctl.h>                                                          // Provides the ioctl function for device-specific control operations
#include <linux/i2c-dev.h>                                                      // Provides the I2C-specific definitions and constants for ioctl
#include <pthread.h>                                                            // Provides POSIX threads functions for multi-threading support (mutex)
#include <syslog.h>                                                             // Provides functions for logging messages to the system logger

#include "mcp23017.h"                                                           // Includes the public declarations for this driver module

/* --- Global State --- */

// `static` variables are private to this file, a good practice for encapsulation.
static int g_fd = -1;                                                           // File descriptor for the I2C bus; -1 indicates it's not open.
static pthread_mutex_t *g_mutex = NULL;                                         // Pointer to an external mutex, enabling optional thread safety.
static uint8_t g_debug = 0;                                                     // A flag to enable or disable verbose debug logging.

int mcp_i2c_open(const char *devnode) {
    g_fd = open(devnode, O_RDWR);                                               // Open the specified I2C device file (e.g., "/dev/i2c-1") for reading and writing.
    if (g_fd < 0) {                                                             // System calls return -1 on error, so we must always check the result.
        perror("open(i2c)");                                                    // perror() prints a user-friendly error message to stderr based on `errno`.
        syslog(LOG_ERR, "Failed to open I2C bus %s: %s", devnode, strerror(errno));// syslog() sends a structured message to the system log for robust diagnostics.
        return -1;                                                              // Return a failure code to the caller.
    }

    // After opening the bus, we must specify which slave device we want to talk to.
    if (ioctl(g_fd, I2C_SLAVE, MCPADDR) < 0) {                                  // `ioctl` performs device-specific commands. I2C_SLAVE sets the target address.
        perror("ioctl(I2C_SLAVE)");                                             // Log the error if we can't set the slave address.
        syslog(LOG_ERR, "Failed to set I2C slave address 0x%02x: %s", MCPADDR, strerror(errno));
        close(g_fd);                                                            // It's crucial to clean up (close the file) if a step fails.
        g_fd = -1;                                                              // Reset the global file descriptor to its initial invalid state.
        return -1;                                                              // Propagate the failure.
    }
    return 0;                                                                   // Return 0 to indicate success.
}

void mcp_i2c_close(void) {
    if (g_fd >= 0) {                                                            // Only attempt to close the file if the descriptor is valid.
        close(g_fd);                                                            // Release the file handle.
    }
    g_fd = -1;                                                                  // Invalidate the global descriptor to prevent accidental reuse.
}

static int mcp_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };                                              // An I2C write requires sending the register address followed by the data.
    if (write(g_fd, buf, 2) != 2) {                                             // Attempt to write the 2-byte buffer to the device.
        perror("mcp_write");                                                    // `write` should return the number of bytes written; if it's not 2, an error occurred.
        syslog(LOG_ERR, "I2C write failed to reg 0x%02x: %s", reg, strerror(errno));
        return -1;
    }
    return 0;
}

static int mcp_read(uint8_t reg, uint8_t *out) {
    // An I2C read is a two-step process: first write the register address you want to read...
    if (write(g_fd, &reg, 1) != 1) {
        perror("mcp_read (set addr)");
        syslog(LOG_ERR, "I2C failed to set read address to 0x%02x: %s", reg, strerror(errno));
        return -1;
    }

    // ...then perform a read to get the data from that register.
    if (read(g_fd, out, 1) != 1) {
        perror("mcp_read (read data)");
        syslog(LOG_ERR, "I2C read failed from reg 0x%02x: %s", reg, strerror(errno));
        return -1;
    }

    if (g_debug) {                                                              // If the debug flag is set, print the transaction for visibility.
        fprintf(stderr, "read[0x%02x] => 0x%02x\n", reg, *out);
        syslog(LOG_DEBUG, "read[0x%02x] => 0x%02x", reg, *out);
    }
    return 0;
}

int mcp_config_outputs(void) {
    // IODIRA and IODIRB are the I/O Direction registers for Port A and Port B.
    // Writing 0x00 to them configures all 8 pins on that port as outputs.
    if (mcp_write(IODIRA, 0x00)) return -1;
    if (mcp_write(IODIRB, 0x00)) return -1;
    return 0;
}

int mcp_map_zone(int zone, uint8_t *reg, uint8_t *mask) {
    if (zone >= 1 && zone <= 8) {                                               // Zones 1 through 8 are mapped directly to the 8 pins of Port A.
        *reg  = GPIOA;                                                          // The target register is GPIOA (the data register for Port A).
        *mask = (uint8_t)(1u << (zone - 1));                                    // A bitmask is created by shifting '1' to the correct pin position (0-7).
        return 0;
    } else if (zone >= 9 && zone <= 14) {                                       // Zones 9 through 14 are mapped to a subset of pins on Port B.
        *reg = GPIOB;                                                           // The target register is GPIOB.
        static const uint8_t bmap[] = {                                         // A lookup table is efficient for non-linear or reversed mappings.
            (1u << 5), (1u << 4), (1u << 3),                                    // Zone 9 -> Pin 5, Zone 10 -> Pin 4, etc.
            (1u << 2), (1u << 1), (1u << 0)
        };
        *mask = bmap[zone - 9];                                                 // Calculate the index (0-5) into the map from the zone number (9-14).
        return 0;
    } else {
        return -1;                                                              // If the zone is outside the valid range (1-14), return an error.
    }
}

int mcp_set_zone_state(int zone, int on) {
    uint8_t reg, mask;
    if (mcp_map_zone(zone, &reg, &mask) != 0) {                                 // First, translate the logical zone number into a physical register and bitmask.
        fprintf(stderr, "Invalid zone: %d\n", zone);
        syslog(LOG_ERR, "Attempted to set invalid zone: %d", zone);
        return -1;
    }

    if (g_mutex) pthread_mutex_lock(g_mutex);                                   // If a mutex is configured (it should be), lock it to ensure this entire operation is atomic.

    int rc = -1;                                                                // Initialize return code to failure; it will be updated on success.
    uint8_t current_val;
    // This is a "read-modify-write" operation, -essential- for changing one pin without affecting others.
    if (mcp_read(reg, &current_val) == 0) {                                     // 1. READ the current state of all 8 pins on the port.
        uint8_t new_val = on ? (current_val | mask) : (current_val & ~mask);    // 2. MODIFY only the bit for our target pin.
                                                                                //    - `| mask` (OR) forces the target bit to 1 (ON).
                                                                                //    - `& ~mask` (AND NOT) forces the target bit to 0 (OFF).
        if (g_debug) {                                                          // Log the details of the change if debugging is enabled.
            fprintf(stderr, "zone %d -> %s: 0x%02x -> 0x%02x (reg 0x%02x)\n",
                    zone, on ? "ON" : "OFF", current_val, new_val, reg);
            syslog(LOG_DEBUG, "zone %d -> %s: 0x%02x -> 0x%02x (reg 0x%02x)",
                   zone, on ? "ON" : "OFF", current_val, new_val, reg);
        }
        
        rc = mcp_write(reg, new_val);                                           // 3. WRITE the new 8-bit value back to the register.
        if (rc != 0) {
            syslog(LOG_ERR, "mcp_write failed during state set for zone %d", zone);
        }
    } else {
        syslog(LOG_ERR, "mcp_read failed during state set for zone %d", zone);
    }
    if (g_mutex) pthread_mutex_unlock(g_mutex);                                 // Always unlock the mutex when done, even if an error occurred.
    return rc;                                                                  // Return the final status of the operation.
}

void mcp_enable_thread_safety(pthread_mutex_t *external_mutex) {                // This allows the main application to pass its own mutex to this driver.
    g_mutex = external_mutex;                                                   // Store the pointer to the mutex for use in `mcp_set_zone_state`.
}