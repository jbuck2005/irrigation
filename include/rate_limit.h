#ifndef RATE_LIMIT_H
#define RATE_LIMIT_H

#include <stdint.h>

// -----------------------------------------------------------------------------
// Rate limiter API
// -----------------------------------------------------------------------------
//
// Provides a per-IP token bucket limiter to prevent abusive clients from
// exhausting resources by opening too many connections in a short period.
// Call rl_check_and_consume() for each new connection. If it returns 1,
// the connection is allowed; if 0, the client must be rejected.
//
// Tunables (can be overridden at compile time):
//   RL_BUCKET_SIZE  – max burst per client (default 5)
//   RL_REFILL_RATE  – tokens per second replenished (default 1)
//
// -----------------------------------------------------------------------------

#ifndef RL_BUCKET_SIZE
#define RL_BUCKET_SIZE 5
#endif

#ifndef RL_REFILL_RATE
#define RL_REFILL_RATE 1
#endif

#ifndef RL_HASH_SIZE
#define RL_HASH_SIZE 128
#endif

// Initialize rate limiter (call once at startup)
void rl_init(void);

// Check rate limit for given IPv4 address (host byte order).
// Returns 1 if allowed (token consumed), 0 if denied.
int rl_check_and_consume(uint32_t ip);

// Optional: free resources (not strictly required for daemons).
void rl_cleanup(void);

#endif
