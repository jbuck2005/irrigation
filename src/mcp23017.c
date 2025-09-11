/**
 * @file mcp23017.c
 * @brief Driver for MCP23017 I2C GPIO expander
 *
 * Provides functions to configure MCP23017 pins as outputs and control them.
 * Zones are mapped to pins according to a reverse mapping strategy.
 *
 * Features:
 *   - Safe I2C file descriptor handling (FD_CLOEXEC)
 *   - Optional thread-safety via external mutex
 *   - Syslog logging for open/close and errors
 *   - Debug logging if IRRIGATIOND_DEBUG=1 is set
 */

#include "mcp23017.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <pthread.h>
#include <syslog.h>

// MCP23017 register addresses (BANK=0)
static const uint8_t IODIRA   = 0x00;
static const uint8_t IODIRB   = 0x01;
static const uint8_t OLATA    = 0x14;
static const uint8_t OLATB    = 0x15;

static int i2c_fd = -1;
static pthread_mutex_t *driver_lock = NULL;

static uint8_t olat_a = 0x00;
static uint8_t olat_b = 0x00;

static int g_debug = 0;

// Reverse mapping table: zone -> { bank, bit }
static const struct { char bank; uint8_t bit; } zone_map[MAX_ZONE + 1] = {
    {0,0},             // unused index 0
    {'A',0}, {'A',1}, {'A',2}, {'A',3}, {'A',4}, {'A',5}, {'A',6}, {'A',7},
    {'B',0}, {'B',1}, {'B',2}, {'B',3}, {'B',4}, {'B',5}
};

// --- Internal helpers ---

static void lock_driver(void)   { if (driver_lock) pthread_mutex_lock(driver_lock); }
static void unlock_driver(void) { if (driver_lock) pthread_mutex_unlock(driver_lock); }

static void set_cloexec(int fd) {
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) return;
    (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    if (write(i2c_fd, buf, 2) != 2) {
        syslog(LOG_ERR, "i2c write reg 0x%02X failed: %s", reg, strerror(errno));
        return -1;
    }
    return 0;
}

// --- Public API ---

int mcp_i2c_open(const char *dev) {
    if (!dev) return -1;
    i2c_fd = open(dev, O_RDWR);
    if (i2c_fd < 0) {
        syslog(LOG_ERR, "open %s failed: %s", dev, strerror(errno));
        return -1;
    }
    set_cloexec(i2c_fd);

    if (ioctl(i2c_fd, I2C_SLAVE, 0x20) < 0) {
        syslog(LOG_ERR, "ioctl I2C_SLAVE failed: %s", strerror(errno));
        close(i2c_fd);
        i2c_fd = -1;
        return -1;
    }

    if (getenv("IRRIGATIOND_DEBUG")) g_debug = 1;
    syslog(LOG_INFO, "MCP23017 opened on %s", dev);
    return 0;
}

void mcp_i2c_close(void) {
    if (i2c_fd >= 0) {
        close(i2c_fd);
        syslog(LOG_INFO, "MCP23017 closed");
        i2c_fd = -1;
    }
}

int mcp_config_outputs(void) {
    if (i2c_fd < 0) return -1;
    if (write_reg(IODIRA, 0x00) < 0) return -1;
    if (write_reg(IODIRB, 0x00) < 0) return -1;
    olat_a = 0x00;
    olat_b = 0x00;
    if (write_reg(OLATA, olat_a) < 0) return -1;
    if (write_reg(OLATB, olat_b) < 0) return -1;
    return 0;
}

int mcp_set_zone_state(int zone, int state) {
    if (zone < 1 || zone > MAX_ZONE) return -1;
    lock_driver();

    char bank = zone_map[zone].bank;
    uint8_t bit = zone_map[zone].bit;
    uint8_t mask = 1u << bit;
    int rc = 0;

    if (bank == 'A') {
        if (state) olat_a |= mask;
        else olat_a &= ~mask;
        rc = write_reg(OLATA, olat_a);
    } else {
        if (state) olat_b |= mask;
        else olat_b &= ~mask;
        rc = write_reg(OLATB, olat_b);
    }

    if (g_debug) {
        syslog(LOG_DEBUG, "Zone %d -> %s", zone, state ? "ON" : "OFF");
    }

    unlock_driver();
    return rc;
}

void mcp_enable_thread_safety(pthread_mutex_t *lock) {
    driver_lock = lock;
}

void mcp_lock(void)   { lock_driver(); }
void mcp_unlock(void) { unlock_driver(); }