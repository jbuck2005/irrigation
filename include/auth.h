#ifndef AUTH_H
#define AUTH_H

// -----------------------------------------------------------------------------
// Authentication helpers for irrigationd
// -----------------------------------------------------------------------------
//
// Provides:
//   - safe_str_equals() : constant-time string compare for tokens
//   - sanitize_cmd_for_log() : redact TOKEN values before logging
//
// These are separated into their own module so that authentication logic is
// easy to review and modify (e.g., in the future supporting multiple tokens,
// rotating tokens, or stronger schemes).
// -----------------------------------------------------------------------------

#include <stddef.h>

// Constant-time compare of two strings (same length, equal = 1, else 0)
int safe_str_equals(const char *a, const char *b);

// Copy input command into out[] (size out_sz), but redact TOKEN=... values.
// Example: "ZONE=1 TIME=30 TOKEN=secret" -> "ZONE=1 TIME=30 TOKEN=REDACTED"
void sanitize_cmd_for_log(const char *cmd, char *out, size_t out_sz);

#endif
