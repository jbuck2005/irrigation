// mcp23017.h

#ifndef MCP23017_H
#define MCP23017_H

#include <pthread.h>
#include <stdint.h>

// MCP23017 I2C address (default)
#define MCPADDR 0x20

// MCP23017 Register Addresses (BANK=0 mode)
#define MCP23017_IODIRA   0x00   // I/O direction register for port A
#define MCP23017_IODIRB   0x01   // I/O direction register for port B
#define MCP23017_OLATA    0x14   // Output latch register for port A
#define MCP23017_OLATB    0x15   // Output latch register for port B
#define MCP23017_GPIOA    0x12   // Input register for port A
#define MCP23017_GPIOB    0x13   // Input register for port B

// Maximum zone number
#define MAX_ZONE 14

// Function prototypes
int mcp_i2c_open(const char *devnode);
void mcp_i2c_close(void);
int mcp_set_zone_state(int zone, int on);
int mcp_config_outputs(void);
int mcp_map_zone(int zone, uint8_t *reg, uint8_t *mask);
void mcp_lock(void);
void mcp_unlock(void);
void mcp_enable_thread_safety(pthread_mutex_t *mutex);

#endif // MCP23017_H
