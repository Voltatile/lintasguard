#include "lg_battery_model.h"

void lg_battery_model_init(lg_battery_model_t *b, uint32_t now_ms) {
    b->uv = (int64_t)LG_BATTERY_FULL_MV * 1000;
    b->drain_mv_s = 0;
    b->last_ms = now_ms;
}

int lg_battery_model_set_drain(lg_battery_model_t *b, uint32_t drain_mv_s, uint32_t now_ms) {
    if (drain_mv_s > LG_BATTERY_DRAIN_MAX_MV_S) {
        return -1;
    }
    lg_battery_model_step(b, now_ms); /* the old rate applies up to now */
    b->drain_mv_s = drain_mv_s;
    return 0;
}

void lg_battery_model_step(lg_battery_model_t *b, uint32_t now_ms) {
    int32_t d = (int32_t)(uint32_t)(now_ms - b->last_ms);
    int64_t full = (int64_t)LG_BATTERY_FULL_MV * 1000;

    if (d <= 0) {
        return; /* time did not move forward */
    }
    b->last_ms = now_ms;
    if (b->drain_mv_s > 0) {
        b->uv -= (int64_t)b->drain_mv_s * d; /* mV/s times ms = uV */
    } else {
        b->uv += (int64_t)LG_BATTERY_CHARGE_MV_S * d;
    }
    if (b->uv < 0) {
        b->uv = 0;
    }
    if (b->uv > full) {
        b->uv = full;
    }
}

uint32_t lg_battery_model_mv(const lg_battery_model_t *b) {
    return (uint32_t)(b->uv / 1000);
}
