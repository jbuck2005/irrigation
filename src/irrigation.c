/*
 * irrigation.c                                                                                   // Stand-alone test tool for MCP23017 irrigation board
 *
 * Usage:                                                                                         //   ./irrigation -z <zone> [-t seconds] [-d]
 *                                                                                                //   zone=0 clears all outputs
 */

#include <unistd.h>                                                                               // getopt, sleep
#include <stdio.h>                                                                                // printf, fprintf
#include <stdlib.h>                                                                               // atoi, EXIT_FAILURE
#include "mcp23017.h"                                                                             // Shared MCP helpers

static uint8_t debug = 0;                                                                         // Local debug flag

int main(int argc, char **argv) {
    int opt, zone = -1, duration_s = 0;                                                           // CLI args

    while ((opt = getopt(argc, argv, "z:t:d")) != -1) {                                           // Parse options
        switch (opt) {
            case 'd': debug = 1; break;                                                           // Enable debug
            case 'z': zone = atoi(optarg); break;                                                 // Zone
            case 't': duration_s = atoi(optarg); break;                                           // Time
            default:
                fprintf(stderr, "Usage: %s -z <zone 0..14> [-t seconds] [-d]\n", argv[0]);        // Help
                return EXIT_FAILURE;                                                              // Exit
        }
    }

    if (mcp_i2c_open("/dev/i2c-1")) return EXIT_FAILURE;                                          // Open I²C
    if (mcp_config_outputs()) { mcp_i2c_close(); return EXIT_FAILURE; }                           // Outputs

    int rc = 0;
    if (zone == 0) {                                                                              // Clear all
        if (debug) puts("Clearing all outputs...");
        rc |= mcp_set_zone_state(1, 0);                                                           // Iterate zones
        for (int z=2; z<=MAX_ZONE; z++) rc |= mcp_set_zone_state(z, 0);
    } else if (zone > 0) {
        if (duration_s > 0) {                                                                     // Timed ON
            rc |= mcp_set_zone_state(zone, 1);                                                    // ON
            if (rc == 0) {
                if (debug) printf("Sleeping %d seconds...\n", duration_s);
                sleep((unsigned int)duration_s);                                                  // Wait
                rc |= mcp_set_zone_state(zone, 0);                                                // OFF
            }
        } else {                                                                                  // Toggle
            rc |= mcp_set_zone_state(zone, 1);                                                    // ON
            sleep(1);
            rc |= mcp_set_zone_state(zone, 0);                                                    // OFF
        }
    } else {
        fprintf(stderr, "Missing or invalid -z <zone> (use 0..14)\n");                            // Error
        rc = -1;
    }

    mcp_i2c_close();                                                                              // Close bus
    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;                                               // Return
}
