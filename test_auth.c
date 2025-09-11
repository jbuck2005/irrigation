#include <stdio.h>
#include <string.h>
#include "auth.h"

// Simple test harness for auth.[ch]
int main(void) {
    const char *token = "secret123";

    // Test safe_str_equals
    printf("Test equal: %d (expected 1)\n", safe_str_equals(token, "secret123"));
    printf("Test wrong: %d (expected 0)\n", safe_str_equals(token, "badtoken"));
    printf("Test length mismatch: %d (expected 0)\n", safe_str_equals(token, "secret1234"));

    // Test sanitize_cmd_for_log
    const char *cmd = "ZONE=1 TIME=30 TOKEN=secret123 EXTRA=foo";
    char out[128];
    sanitize_cmd_for_log(cmd, out, sizeof(out));
    printf("Sanitized: %s\n", out);

    // Ensure the token was redacted
    if (strstr(out, "secret123"))
        printf("FAIL: token leaked!\n");
    else
        printf("PASS: token redacted\n");

    return 0;
}

// gcc -Wall -Wextra -o test_rate_limit test_rate_limit.c rate_limit.c -lpthread
// ./test_rate_limit