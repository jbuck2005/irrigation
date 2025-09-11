/**
 * @file mcp23017.h
 * @author James Buck
 * @date September 10, 2025
 * @brief Public API for the MCP23017 irrigation driver.
 *
 * @description
 * This header defines constants, macros, and function prototypes
 * for controlling an MCP23017 I/O expander in the context of an irrigation system.
 *
 * The MCP23017 is an I2C device with two 8-bit GPIO ports (A and B).
 * In this system:
 *   - Zones 1–8 are mapped to GPA0–GPA7
 *   - Zones 9–14 are mapped to GPB5–GPB0 (reversed order for wiring convenience)
 *
 * Features:
 *   - Initialization and configuration of MCP23017 registers
 *   - Zone mapping logic (abstract zone numbers to registers/pins)
 *   - Zone ON/OFF control
 *   - Optional thread safety (external mutex integration)
 *
 * Usage:
 *   - Call mcp_i2c_open("/dev/i2c-X") to open the I²C bus
 *   - Call mcp_config_outputs() to configure all pins as outputs
 *   - Call mcp_enable_thread_safety(&mutex) if multi-threading is used
 *   - Use mcp_set_zone_state(zone, on) to control zones
 *   - Always call mcp_i2c_close() at shutdown
 */

#ifndef MCP23017_H
#define MCP23017_H

#include <stdint.h>                                                             // For fixed-width integer types like uint8_t
#include <pthread.h>                                                            // For pthread_mutex_t when enabling thread safety

// --- MCP23017 I2C Address ---
#define MCPADDR   0x20                                                          // Default I2C address for MCP23017 (configurable with hardware pins)

// --- MCP23017 Register Map (BANK=0 mode) ---
#define IODIRA    0x00                                                          // I/O direction register for Port A (1 = input, 0 = output)
#define IODIRB    0x01                                                          // I/O direction register for Port B
#define GPIOA     0x12                                                          // General Purpose I/O register for Port A (read/write pin states)
#define GPIOB     0x13                                                          // General Purpose I/O register for Port B

// --- Irrigation system logical constants ---
#define MAX_ZONE   14                                                            // Maximum number of supported zones (1..14)

// --- Function Prototypes ---

/**
 * @brief Open I²C device and configure slave address.
 *
 * @param devnode Path to I²C device node (e.g., "/dev/i2c-1")
 * @return 0 on success, -1 on failure
 */
int mcp_i2c_open(const char *devnode);

/**
 * @brief Close the I²C device.
 */
void mcp_i2c_close(void);

/**
 * @brief Configure all MCP23017 pins as outputs.
 *
 * @return 0 on success, -1 on failure
 */
int mcp_config_outputs(void);

/**
 * @brief Map a logical zone to the corresponding MCP23017 register and bit mask.
 *
 * @param zone Zone number (1–14)
 * @param reg Output parameter: target register (GPIOA or GPIOB)
 * @param mask Output parameter: bitmask for the target pin
 * @return 0 on success, -1 on invalid zone
 */
int mcp_map_zone(int zone, uint8_t *reg, uint8_t *mask);

/**
 * @brief Set the ON/OFF state of a zone.
 *
 * Important: This function does NOT perform internal locking.
 * The caller should call mcp_lock() / mcp_unlock() if thread safety is required.
 *
 * @param zone Zone number (1–14)
 * @param on 1 = ON, 0 = OFF
 * @return 0 on success, -1 on failure
 */
int mcp_set_zone_state(int zone, int on);

/**
 * @brief Enable thread safety by passing an external mutex.
 *
 * After calling this, all internal locking is done using the provided mutex.
 * This allows consistent atomicity between driver operations and higher-level
 * application state (e.g., zone_state[] array in irrigationd).
 *
 * Example:
 *   pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;
 *   mcp_enable_thread_safety(&zone_lock);
 */
void mcp_enable_thread_safety(pthread_mutex_t *external_mutex);

/**
 * @brief Lock the driver's mutex (if configured).
 *
 * This function should be used by applications when they need to ensure
 * that a sequence of operations on both driver and application state
 * is atomic.
 */
void mcp_lock(void);

/**
 * @brief Unlock the driver's mutex (if configured).
 *
 * Complements mcp_lock(). Safe to call even if no mutex was configured.
 */
void mcp_unlock(void);

#endif // MCP23017_H