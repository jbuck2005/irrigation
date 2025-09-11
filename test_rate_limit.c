#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include "rate_limit.h"

// Simple test harness for rate_limit.[ch]
int main(void) {
    uint32_t ip1 = 0x7f000001; // 127.0.0.1 in host order
    uint32_t ip2 = 0x0a000001; // 10.0.0.1

    rl_init();

    printf("Testing rate limiter (bucket=%d, refill=%d/sec)\n",
           RL_BUCKET_SIZE, RL_REFILL_RATE);

    // Consume all tokens for ip1
    for (int i = 0; i < RL_BUCKET_SIZE; i++) {
        int allowed = rl_check_and_consume(ip1);
        printf("ip1 attempt %d: %d\n", i+1, allowed);
    }

    // One more should fail
    printf("ip1 extra attempt: %d (expected 0)\n", rl_check_and_consume(ip1));

    // Different IP still allowed
    printf("ip2 attempt: %d (expected 1)\n", rl_check_and_consume(ip2));

    // Wait for refill
    printf("Sleeping 2 seconds for refill...\n");
    sleep(2);

    printf("ip1 after sleep: %d (expected 1)\n", rl_check_and_consume(ip1));

    rl_cleanup();
    return 0;
}

// gcc -Wall -Wextra -o test_rate_limit test_rate_limit.c rate_limit.c -lpthread
// ./test_rate_limit