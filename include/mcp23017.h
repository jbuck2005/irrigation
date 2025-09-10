/*
 * mcp23017.h                                                                                     // Header for MCP23017 irrigation control helper
 *
 * Provides:                                                                                      // - Register definitions
 *                                                                                                // - Zone mapping 1..14 → MCP register/bit
 *                                                                                                // - Public functions for opening/closing I²C,
 *                                                                                                //   configuring outputs, and setting zone state
 */

#ifndef MCP23017_H
#define MCP23017_H

#include <stdint.h>                                                                               // uint8_t
#include <pthread.h>                                                                              // pthread_mutex_t (daemon build)

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------ MCP23017 register addresses (BANK=0) ---------------------------

static const uint8_t IODIRA = 0x00;                                                               // Direction register A (1=input, 0=output)
static const uint8_t IODIRB = 0x01;                                                               // Direction register B
static const uint8_t GPIOA  = 0x12;                                                               // Port A data register
static const uint8_t GPIOB  = 0x13;                                                               // Port B data register
static const uint8_t OLATA  = 0x14;                                                               // Output latch A
static const uint8_t OLATB  = 0x15;                                                               // Output latch B

#define MCPADDR 0x20                                                                              // MCP23017 I²C address (A2..A0 = 000)
#define MAX_ZONE 14                                                                               // Valid zones: 1..14

// ------------------------------ API ------------------------------------------------------------

// Open and close the I²C device                                                                  //
int     mcp_i2c_open(const char *devnode);                                                        // Returns 0 on success, -1 on error
void    mcp_i2c_close(void);                                                                      // Safe to call multiple times

// Configure all MCP23017 pins as outputs                                                          //
int     mcp_config_outputs(void);                                                                 // Returns 0 on success

// Map a zone number (1..14) to a register address and bitmask                                     //
int     mcp_map_zone(int zone, uint8_t *reg, uint8_t *mask);                                      // Returns 0 if valid, -1 if invalid

// Set a zone ON (1) or OFF (0)                                                                   //
int     mcp_set_zone_state(int zone, int on);                                                     // Returns 0 on success

// Optional: initialize mutex protection for multithreaded daemons                                //
void    mcp_enable_thread_safety(pthread_mutex_t *external_mutex);                                // Pass a pointer to daemon’s mutex

#ifdef __cplusplus
}
#endif

#endif // MCP23017_H
