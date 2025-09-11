/**
 * @file mcp23017.h
 * @brief Interface for MCP23017 I2C GPIO expander driver
 *
 * This header exposes a simplified API for controlling irrigation zones
 * connected via an MCP23017 16-bit GPIO expander.
 *
 * Key points:
 *   - Provides initialization, configuration, and per-zone state control
 *   - Zones map to MCP23017 pins via a fixed reverse mapping table
 *   - Optional thread-safety: driver functions can use an external mutex
 *   - Intended to be used by irrigationd for zone management
 *
 * Safety improvements:
 *   - FD_CLOEXEC applied to I2C file descriptor
 *   - Syslog used for logging (errors, info, optional debug)
 *   - Optional debug logging enabled by setting environment variable
 *     IRRIGATIOND_DEBUG=1
 */

#ifndef MCP23017_H
#define MCP23017_H

#include <stdint.h>
#include <pthread.h>

#define MAX_ZONE 14

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open the MCP23017 I2C device.
 *
 * @param dev Path to I2C device (e.g. "/dev/i2c-1")
 * @return 0 on success, -1 on failure (logs to syslog)
 */
int mcp_i2c_open(const char *dev);

/**
 * @brief Close the MCP23017 device.
 *
 * Safe to call multiple times. Logs closure to syslog.
 */
void mcp_i2c_close(void);

/**
 * @brief Configure all MCP23017 pins as outputs and clear them.
 *
 * Writes to IODIRA/IODIRB to set pins as outputs,
 * initializes OLATA/OLATB to 0 (all zones off).
 *
 * @return 0 on success, -1 on failure
 */
int mcp_config_outputs(void);

/**
 * @brief Set a zone state (ON or OFF).
 *
 * @param zone Zone number (1..MAX_ZONE)
 * @param state 0=OFF, 1=ON
 * @return 0 on success, -1 on error
 */
int mcp_set_zone_state(int zone, int state);

/**
 * @brief Enable thread safety for driver operations.
 *
 * Pass a pointer to a mutex owned by the caller (usually irrigationd).
 * If provided, all state-changing driver functions will lock/unlock it.
 *
 * @param lock Pointer to pthread_mutex_t to use for synchronization
 */
void mcp_enable_thread_safety(pthread_mutex_t *lock);

/**
 * @brief Explicitly acquire the driver lock (if enabled).
 *
 * Normally used in conjunction with irrigationd’s zone_state operations
 * when atomic updates are required.
 */
void mcp_lock(void);

/**
 * @brief Explicitly release the driver lock (if enabled).
 */
void mcp_unlock(void);

#ifdef __cplusplus
}
#endif

#endif // MCP23017_H