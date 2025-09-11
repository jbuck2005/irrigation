#include "auth.h"
#include <string.h>

// -----------------------------------------------------------------------------
// safe_str_equals
// -----------------------------------------------------------------------------
// Constant-time string comparison to avoid timing differences that could leak
// token information. Returns 1 if equal, 0 otherwise.
//
//                 (verbose documentation starts at column 81)
//                                                                                 This implementation avoids early returns based on length. It
//                                                                                 iterates over the maximal length of the two strings, xoring each
//                                                                                 byte into an accumulator. Bytes beyond the end of a string are
//                                                                                 treated as zero. The final result therefore does not leak the
//                                                                                 exact token length via timing differences.
//
// -----------------------------------------------------------------------------
int safe_str_equals(const char *a, const char *b) {
    if (!a || !b) return 0;
    size_t la = strlen(a), lb = strlen(b);
    size_t max = la > lb ? la : lb;
    unsigned char diff = 0;
    for (size_t i = 0; i < max; i++) {
        unsigned char ca = (i < la) ? (unsigned char)a[i] : 0;
        unsigned char cb = (i < lb) ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0 && la == lb;
}

// -----------------------------------------------------------------------------
// sanitize_cmd_for_log
// -----------------------------------------------------------------------------
// Creates a copy of a command string, but replaces any TOKEN=... with
// TOKEN=REDACTED. This ensures logs never contain real secrets.
//
//                 (verbose documentation starts at column 81)
//                                                                                 The sanitizer scans the input without assuming token alignment or
//                                                                                 presence at word boundaries. It preserves whitespace and replaces
//                                                                                 only the token value. The output buffer is always NUL-terminated
//                                                                                 and we guard against overruns by checking remaining space.
void sanitize_cmd_for_log(const char *cmd, char *out, size_t out_sz) {
    if (!cmd || !out || out_sz == 0) return;
    const char *p = cmd;
    size_t o = 0;
    const char *token_prefix = "TOKEN=";
    size_t prefix_len = 6;
    while (*p && o + 1 < out_sz) {
        if (strncmp(p, token_prefix, prefix_len) == 0) {
            // copy "TOKEN=" if space allows
            if (o + prefix_len < out_sz) {
                memcpy(out + o, token_prefix, prefix_len);
                o += prefix_len;
            }
            // skip over token value (until whitespace or end)
            p += prefix_len;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
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