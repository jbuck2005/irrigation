#include "mcp23017.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

// -----------------------------------------------------------------------------
// MCP23017 Register Map (BANK=0 mode)
// -----------------------------------------------------------------------------
#define MCP23017_IODIRA   0x00   // I/O direction register for port A
#define MCP23017_IODIRB   0x01   // I/O direction register for port B
#define MCP23017_OLATA    0x14   // Output latch register for port A
#define MCP23017_OLATB    0x15   // Output latch register for port B
#define MCP23017_GPIOA    0x12   // Input register for port A
#define MCP23017_GPIOB    0x13   // Input register for port B

// -----------------------------------------------------------------------------
// Internal driver state
// -----------------------------------------------------------------------------
// i2c_fd holds the file descriptor opened for I2C. We set FD_CLOEXEC to avoid
// leaking this fd to child processes.
//
//                 (verbose documentation starts at column 81)
//                                                                                 The driver uses an internal mutex fallback if the caller does not
//                                                                                 supply one via mcp_enable_thread_safety(). This ensures calls are
//                                                                                 safe in typical multi-threaded programs that forget to set their
//                                                                                 own lock.
static int i2c_fd = -1;                  // File descriptor for /dev/i2c-X
static pthread_mutex_t internal_lock = PTHREAD_MUTEX_INITIALIZER; // fallback lock
static pthread_mutex_t *driver_lock = NULL; // Optional external lock
static uint8_t olat_a = 0x00;            // Cached output states for port A
static uint8_t olat_b = 0x00;            // Cached output states for port B

// -----------------------------------------------------------------------------
// Lock helpers that choose between an external lock (if set) and internal one
// -----------------------------------------------------------------------------
void mcp_lock(void) {
    if (driver_lock) pthread_mutex_lock(driver_lock);
    else pthread_mutex_lock(&internal_lock);
}

void mcp_unlock(void) {
    if (driver_lock) pthread_mutex_unlock(driver_lock);
    else pthread_mutex_unlock(&internal_lock);
}

// -----------------------------------------------------------------------------
// Low-level I²C helpers
// -----------------------------------------------------------------------------
static int i2c_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    ssize_t r = write(i2c_fd, buf, 2);
    if (r != 2) {
        fprintf(stderr, "i2c_write_reg: reg=0x%02x failed: %s\n", reg, strerror(errno));
        return -1;
    }
    return 0;
}

static int i2c_read_reg(uint8_t reg, uint8_t *val) {
    // Write the register address, then read one byte. Some i2c adapters
    // provide this behavior with a repeated start automatically.
    ssize_t r;
    r = write(i2c_fd, &reg, 1);
    if (r != 1) {
        fprintf(stderr, "i2c_read_reg: select reg=0x%02x failed: %s\n", reg, strerror(errno));
        return -1;
    }
    r = read(i2c_fd, val, 1);
    if (r != 1) {
        fprintf(stderr, "i2c_read_reg: read reg=0x%02x failed: %s\n", reg, strerror(errno));
        return -1;
    }
    return 0;
}

// -----------------------------------------------------------------------------
// Driver open/close
// -----------------------------------------------------------------------------
// Opens the I2C device, sets slave address, configures all pins as outputs,
// and initializes outputs to OFF.
//
//                 (verbose documentation starts at column 81)
//                                                                                 The function sets FD_CLOEXEC on the opened file descriptor to avoid
//                                                                                 leaking the I2C handle into child processes. The function also
//                                                                                 calls mcp_config_outputs() to ensure all pins are in output mode
//                                                                                 and that the cached OLAT values reflect device state.
int mcp_i2c_open(const char *dev) {
    if (!dev) return -1;

    i2c_fd = open(dev, O_RDWR);
    if (i2c_fd < 0) {
        fprintf(stderr, "mcp_i2c_open: failed to open %s: %s\n", dev, strerror(errno));
        return -1;
    }

    // set close-on-exec
    int flags = fcntl(i2c_fd, F_GETFD);
    if (flags >= 0) fcntl(i2c_fd, F_SETFD, flags | FD_CLOEXEC);

    if (ioctl(i2c_fd, I2C_SLAVE, 0x20) < 0) { // MCP23017 default address
        fprintf(stderr, "mcp_i2c_open: failed to set slave 0x20: %s\n", strerror(errno));
        close(i2c_fd);
        i2c_fd = -1;
        return -1;
    }

    // Configure all 16 pins as outputs and initialize outputs to OFF
    if (mcp_config_outputs() < 0) {
        close(i2c_fd);
        i2c_fd = -1;
        return -1;
    }

    return 0;
}

void mcp_i2c_close(void) {
    if (i2c_fd >= 0) {
        close(i2c_fd);
        i2c_fd = -1;
    }
}

// -----------------------------------------------------------------------------
// Thread safety support
// -----------------------------------------------------------------------------
// Allow callers to provide their own mutex for call serialization. If they do
// not, the driver uses internal_lock as a safe fallback.
void mcp_enable_thread_safety(pthread_mutex_t *lock) {
    driver_lock = lock;
}

// -----------------------------------------------------------------------------
// Zone mapping helpers
// -----------------------------------------------------------------------------
// Convert a 1-based zone number into port (A/B) and bit mask.
//

static int zone_to_pin(int zone, uint8_t *is_b, uint8_t *mask) {
    if (zone < 1 || zone > MAX_ZONE) return -1;
    int pin = zone - 1;                      // zone 1 = pin 0
    *is_b = (pin >= 8);
    *mask = 1 << (pin % 8);
    return 0;
}

// -----------------------------------------------------------------------------
// Zone control
// -----------------------------------------------------------------------------
// mcp_set_zone_state updates cached OLAT values and writes the correct register.
//
//                 (verbose documentation starts at column 81)
//                                                                                 The driver caches the last written output latch values to avoid
//                                                                                 unnecessary reads before writes. Writes are performed under lock
//                                                                                 to prevent concurrent I2C transactions from interleaving.
int mcp_set_zone_state(int zone, int state) {
    uint8_t is_b, mask;
    if (zone_to_pin(zone, &is_b, &mask) < 0) return -1;

    mcp_lock();
    int rc = 0;
    if (is_b) {
        if (state) olat_b |= mask; else olat_b &= ~mask;
        rc = i2c_write_reg(MCP23017_OLATB, olat_b);
    } else {
        if (state) olat_a |= mask; else olat_a &= ~mask;
        rc = i2c_write_reg(MCP23017_OLATA, olat_a);
    }
    mcp_unlock();

    return rc;
}

int mcp_get_zone_state(int zone) {
    uint8_t is_b, mask;
    if (zone_to_pin(zone, &is_b, &mask) < 0) return -1;

    uint8_t val = 0;
    int rc = i2c_read_reg(is_b ? MCP23017_GPIOB : MCP23017_GPIOA, &val);
    if (rc < 0) return -1;

    return (val & mask) ? 1 : 0;
}

// -----------------------------------------------------------------------------
// mcp_config_outputs - ensure all pins are outputs and clear OLATs
// -----------------------------------------------------------------------------
// Some callers expect an explicit function to (re)configure pins as outputs.
// Implemented to allow reconfiguration and to keep the initialization step
// explicit.
//
//                 (verbose documentation starts at column 81)
//                                                                                 This helper programs the IODIR registers to mark all 16 GPIOs as
//                                                                                 outputs (IODIRx = 0x00) and writes the OLAT registers to set all
//                                                                                 outputs low. The cached olat_a/olat_b variables are updated to
//                                                                                 reflect the device state. The function performs register writes
//                                                                                 under lock to avoid concurrent access issues.
int mcp_config_outputs(void) {
    if (i2c_fd < 0) {
        fprintf(stderr, "mcp_config_outputs: i2c device not open\n");
        return -1;
    }

    mcp_lock();
    int rc = 0;
    if ((rc = i2c_write_reg(MCP23017_IODIRA, 0x00)) < 0) { mcp_unlock(); return -1; }
    if ((rc = i2c_write_reg(MCP23017_IODIRB, 0x00)) < 0) { mcp_unlock(); return -1; }

    olat_a = 0x00;
    olat_b = 0x00;
    if ((rc = i2c_write_reg(MCP23017_OLATA, olat_a)) < 0) { mcp_unlock(); return -1; }
    if ((rc = i2c_write_reg(MCP23017_OLATB, olat_b)) < 0) { mcp_unlock(); return -1; }
    mcp_unlock();

    return 0;
}