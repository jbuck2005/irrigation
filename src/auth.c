#include "auth.h"
#include <string.h>

// -----------------------------------------------------------------------------
// safe_str_equals
// -----------------------------------------------------------------------------
// Constant-time string comparison to avoid timing differences that could leak
// token information. Returns 1 if equal, 0 otherwise.
// -----------------------------------------------------------------------------
int safe_str_equals(const char *a, const char *b) {
    if (!a || !b) return 0;
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return 0;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

// -----------------------------------------------------------------------------
// sanitize_cmd_for_log
// -----------------------------------------------------------------------------
// Creates a copy of a command string, but replaces any TOKEN=... with
// TOKEN=REDACTED. This ensures logs never contain real secrets.
// -----------------------------------------------------------------------------
void sanitize_cmd_for_log(const char *cmd, char *out, size_t out_sz) {
    const char *p = cmd;
    size_t o = 0;
    while (*p && o + 1 < out_sz) {
        if (strncmp(p, "TOKEN=", 6) == 0) {
            if (o + 6 < out_sz) {
                memcpy(out + o, "TOKEN=", 6);
                o += 6;
            }
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
                p++;
            const char *red = "REDACTED";
            size_t rlen = strlen(red);
            if (o + rlen < out_sz) {
                memcpy(out + o, red, rlen);
                o += rlen;
            }
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
}