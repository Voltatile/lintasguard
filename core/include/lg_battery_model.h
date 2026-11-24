#ifndef LG_BATTERY_MODEL_H
#define LG_BATTERY_MODEL_H

/*
 * Simulated backup battery of the crossing (README 8.3, Step 11), run inside lg-gate. Portable and
 * pure: the caller passes the time. A charger keeps it at LG_BATTERY_FULL_MV; an injected drain
 * (a failing charger or a cell going bad) pulls it down at drain mV per second, and while a drain
 * is set the charger does not help. Clearing the drain lets the charger bring it back up at
 * LG_BATTERY_CHARGE_MV_S. Kept in microvolts so that slow rates are exact.
 */

#include <stdint.h>

#include "lg_config.h"

#define LG_BATTERY_DRAIN_MAX_MV_S 1000u

typedef struct {
    int64_t uv;
    uint32_t drain_mv_s;
    uint32_t last_ms;
} lg_battery_model_t;

void lg_battery_model_init(lg_battery_model_t *b, uint32_t now_ms);
/* 0 on success, -1 above LG_BATTERY_DRAIN_MAX_MV_S (nothing changes). */
int lg_battery_model_set_drain(lg_battery_model_t *b, uint32_t drain_mv_s, uint32_t now_ms);
void lg_battery_model_step(lg_battery_model_t *b, uint32_t now_ms);
uint32_t lg_battery_model_mv(const lg_battery_model_t *b);

#endif
