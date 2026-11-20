#ifndef LG_TARGET_UTIL_H
#define LG_TARGET_UTIL_H

/* Small helpers shared by the QNX processes and tools. */

#include <stdint.h>
#include <time.h>

/* CLOCK_MONOTONIC in nanoseconds, 0 if the clock cannot be read. */
static inline uint64_t lg_now_ns(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The same in milliseconds, truncated to 32 bits (wraps after about 49 days; see lg_time.h). */
static inline uint32_t lg_now_ms(void) {
    return (uint32_t)(lg_now_ns() / 1000000ull);
}

#endif
