// Change 1: Add this define for clock_gettime() and CLOCK_MONOTONIC
#define _POSIX_C_SOURCE 199309L

#include "rate_limit.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>

// -----------------------------------------------------------------------------
// Internal state
// -----------------------------------------------------------------------------
struct rl_entry {
    uint32_t ip;
    double tokens;            // allow fractional tokens for smooth refill
    double last_refill;       // monotonic time in seconds
    time_t last_seen;         // wall clock for TTL (optional)
    struct rl_entry *next;
};

static struct rl_entry *rl_table[RL_HASH_SIZE];
static pthread_mutex_t rl_lock = PTHREAD_MUTEX_INITIALIZER;

// Tunables
#ifndef RL_MAX_ENTRIES
#define RL_MAX_ENTRIES 10000   // cap table entries to avoid memory exhaustion
#endif

#ifndef RL_ENTRY_TTL
#define RL_ENTRY_TTL 300       // seconds after which an unused entry can be pruned
#endif

// Helper: get monotonic time as double seconds
static double now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// hash helper
static unsigned rl_hash(uint32_t ip) {
    return (ip ^ (ip >> 16)) % RL_HASH_SIZE;
}

// -----------------------------------------------------------------------------
// API implementation
// -----------------------------------------------------------------------------
void rl_init(void) {
    memset(rl_table, 0, sizeof(rl_table));
}

void rl_cleanup(void) {
    pthread_mutex_lock(&rl_lock);
    for (int i = 0; i < RL_HASH_SIZE; i++) {
        struct rl_entry *e = rl_table[i];
        while (e) {
            struct rl_entry *next = e->next;
            free(e);
            e = next;
        }
        rl_table[i] = NULL;
    }
    pthread_mutex_unlock(&rl_lock);
}

// Helper: count total entries (protected by rl_lock)
static size_t rl_count_entries_locked(void) {
    size_t cnt = 0;
    for (int i = 0; i < RL_HASH_SIZE; i++) {
        struct rl_entry *e = rl_table[i];
        while (e) { cnt++; e = e->next; }
    }
    return cnt;
}

// Prune entries older than TTL (caller holds rl_lock)
static void rl_prune_locked(time_t now_wall) {
    for (int i = 0; i < RL_HASH_SIZE; i++) {
        struct rl_entry **pp = &rl_table[i];
        while (*pp) {
            struct rl_entry *e = *pp;
            if ((now_wall - e->last_seen) > RL_ENTRY_TTL) {
                *pp = e->next;
                free(e);
            } else {
                pp = &e->next;
            }
        }
    }
}

// Returns 1 if allowed (consumed token), 0 if denied
int rl_check_and_consume(uint32_t ip) {
    double now = now_monotonic();
    time_t now_wall = time(NULL);
    unsigned h = rl_hash(ip);

    pthread_mutex_lock(&rl_lock);

    // Change 2: Call the prune function to clean up old entries.
    // This fixes the bug and removes the "unused function" warning.
    // The incorrect check that was here has also been removed.
    rl_prune_locked(now_wall);

    // find entry
    struct rl_entry *e = rl_table[h];
    while (e && e->ip != ip) e = e->next;

    if (!e) {
        // create new entry only if under cap
        if (rl_count_entries_locked() >= RL_MAX_ENTRIES) {
            pthread_mutex_unlock(&rl_lock);
            return 0; // Table is full, deny creation of new entry
        }
        e = malloc(sizeof(*e));
        if (!e) { pthread_mutex_unlock(&rl_lock); return 0; }
        e->ip = ip;
        e->tokens = RL_BUCKET_SIZE;    // start full, consume one below
        e->last_refill = now;
        e->last_seen = now_wall;
        e->next = rl_table[h];
        rl_table[h] = e;
    }

    // refill based on monotonic time (allow fractional refill)
    double elapsed = now - e->last_refill;
    if (elapsed > 0.0) {
        double refill = elapsed * (double)RL_REFILL_RATE;
        if (refill > 0.0) {
            e->tokens += refill;
            if (e->tokens > RL_BUCKET_SIZE) e->tokens = RL_BUCKET_SIZE;
            e->last_refill = now;
        }
    }

    e->last_seen = now_wall;

    if (e->tokens >= 1.0) {
        e->tokens -= 1.0;
        pthread_mutex_unlock(&rl_lock);
        return 1;
    } else {
        pthread_mutex_unlock(&rl_lock);
        return 0;
    }
}