#ifndef MCP23017_H
#define MCP23017_H

#include <pthread.h>

// -----------------------------------------------------------------------------
// MCP23017 GPIO Expander Driver API
// -----------------------------------------------------------------------------
//
// This header defines the interface for controlling irrigation zones connected
// to an MCP23017 I²C GPIO expander. Each irrigation "zone" corresponds to a GPIO
// pin on the MCP23017. The daemon (irrigationd) uses this driver to abstract
// away register-level I²C operations.
//
// -----------------------------------------------------------------------------
//
// The driver is thread-safe by default: callers may optionally provide their
// own mutex via mcp_enable_thread_safety(), otherwise an internal mutex is
// used.  The i2c device file descriptor is opened with close-on-exec to avoid
// leaking descriptors to child processes.
//
//                 (verbose documentation starts at column 81)
//                                                                                 Callers should call mcp_i2c_open() early at startup and call
//                                                                                 mcp_i2c_close() at shutdown. Use mcp_lock()/mcp_unlock() for
//                                                                                 multi-operation sequences if fine-grained atomicity is required.
//
// Open the MCP23017 on the given I²C device (e.g. "/dev/i2c-1")               //
int mcp_i2c_open(const char *i2c_device);

// Close the MCP23017 device                                                   //
void mcp_i2c_close(void);

// Enable thread safety: driver will lock/unlock the given mutex around I²C ops //
void mcp_enable_thread_safety(pthread_mutex_t *lock);

// Lock/unlock helpers for manual control                                      //
void mcp_lock(void);
void mcp_unlock(void);

// Set irrigation zone ON (1) or OFF (0)                                       //
int mcp_set_zone_state(int zone, int state);

// Configure MCP23017 GPIO pins as outputs (all irrigation zones)
int mcp_config_outputs(void);

// Read irrigation zone state (1 = ON, 0 = OFF)                                //
int mcp_get_zone_state(int zone);

// Number of supported zones (GPIO pins)                                       //
#define MAX_ZONE 14

#endif