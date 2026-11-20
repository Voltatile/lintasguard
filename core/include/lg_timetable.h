#ifndef LG_TIMETABLE_H
#define LG_TIMETABLE_H

/*
 * Level 3 timetable windows (README 8.4). Step 4 declares the API surface only;
 * the logic arrives in Step 12. Until then no window is ever reported. The mode manager
 * does not read this module: the controller must pass schedule_ok = 0 in lg_mode_in_t
 * until lg-schedule reports usable information (Step 7 and Step 12 integration).
 * Portable.
 */

#include <stdint.h>

typedef struct {
    uint32_t start_ms;
    uint32_t end_ms;
    uint8_t valid; /* 0 = no window known */
} lg_tt_window_t;

/* Not implemented yet: always returns an invalid window. */
lg_tt_window_t lg_timetable_window(uint8_t dir, uint32_t now_ms);

#endif
