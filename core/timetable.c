#include "lg_timetable.h"

lg_tt_window_t lg_timetable_window(uint8_t dir, uint32_t now_ms) {
    lg_tt_window_t w;

    (void)dir;
    (void)now_ms;
    w.start_ms = 0;
    w.end_ms = 0;
    w.valid = 0;
    return w;
}
