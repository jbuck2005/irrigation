#ifndef MCP23017_H
#define MCP23017_H

#include <stdint.h>

// MCP23017 I2C address
#define MCPADDR 0x20  // Default I2C address for MCP23017

// MCP23017 Register Addresses
#define IODIRA   0x00   // I/O direction register for port A
#define IODIRB   0x01   // I/O direction register for port B
#define OLATA    0x14   // Output latch register for port A
#define OLATB    0x15   // Output latch register for port B
#define GPIOA    0x12   // Input register for port A
#define GPIOB    0x13   // Input register for port B

// Function Prototypes
int mcp_i2c_open(const char *devnode);
void mcp_i2c_close(void);
void mcp_lock(void);
void mcp_unlock(void);
int mcp_set_zone_state(int zone, int on);
int mcp_config_outputs(void);
int mcp_map_zone(int zone, uint8_t *reg, uint8_t *mask);
void mcp_enable_thread_safety(pthread_mutex_t *external_mutex);

#endif // MCP23017_H

