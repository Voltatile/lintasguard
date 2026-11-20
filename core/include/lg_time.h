#ifndef LG_TIME_H
#define LG_TIME_H

/* Wrap-safe elapsed-time check for 32-bit millisecond counters. Portable. */

#include <stdint.h>

/*
 * True when at least `need_ms` have passed since `since_ms`. Safe across a counter wrap.
 * A `since_ms` that lies in the future (now is earlier, for example a timestamp taken a
 * moment after `now` was read) counts as NOT elapsed, never as a huge elapsed time.
 * Valid for spans up to 2^31 ms (about 24 days).
 */
static inline int lg_elapsed_ge(uint32_t now_ms, uint32_t since_ms, uint32_t need_ms) {
    int32_t d = (int32_t)(uint32_t)(now_ms - since_ms);

    return d >= 0 && (uint32_t)d >= need_ms;
}

#endif
