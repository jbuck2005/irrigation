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
    int tokens;
    time_t last_refill;
    struct rl_entry *next;
};

static struct rl_entry *rl_table[RL_HASH_SIZE];
static pthread_mutex_t rl_lock = PTHREAD_MUTEX_INITIALIZER;

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

int rl_check_and_consume(uint32_t ip) {
    time_t now = time(NULL);
    unsigned h = rl_hash(ip);

    pthread_mutex_lock(&rl_lock);
    struct rl_entry *e = rl_table[h];
    while (e && e->ip != ip) e = e->next;
    if (!e) {
        e = malloc(sizeof(*e));
        if (!e) {
            pthread_mutex_unlock(&rl_lock);
            return 0; // out of memory = deny
        }
        e->ip = ip;
        e->tokens = RL_BUCKET_SIZE - 1;
        e->last_refill = now;
        e->next = rl_table[h];
        rl_table[h] = e;
        pthread_mutex_unlock(&rl_lock);
        return 1;
    }

    // refill
    int elapsed = (int)(now - e->last_refill);
    if (elapsed > 0) {
        int refill = elapsed * RL_REFILL_RATE;
        if (refill > 0) {
            e->tokens = (e->tokens + refill > RL_BUCKET_SIZE)
                        ? RL_BUCKET_SIZE : e->tokens + refill;
            e->last_refill = now;
        }
    }

    if (e->tokens > 0) {
        e->tokens--;
        pthread_mutex_unlock(&rl_lock);
        return 1;
    } else {
        pthread_mutex_unlock(&rl_lock);
        return 0;
    }
}
