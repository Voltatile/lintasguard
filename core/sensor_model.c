#include "lg_sensor_model.h"

#include <string.h>

static uint32_t next_rng(lg_sensor_model_t *m) {
    uint32_t x = m->rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m->rng = x;
    return x;
}

static void hist_push(lg_sensor_model_t *m, uint32_t t_ms, uint8_t occupied, uint16_t axles) {
    if (m->hist_count > 0) {
        const lg_sensor_hist_t *newest = &m->hist[m->hist_head];

        if (newest->occupied == occupied && newest->axles == axles) {
            return; /* only changes are stored */
        }
        m->hist_head = (m->hist_head + 1) % LG_SENSOR_HISTORY;
    } else {
        m->hist_head = 0;
    }
    m->hist[m->hist_head].t_ms = t_ms;
    m->hist[m->hist_head].occupied = occupied;
    m->hist[m->hist_head].axles = axles;
    m->hist[m->hist_head].reserved = 0;
    if (m->hist_count < LG_SENSOR_HISTORY) {
        m->hist_count++;
    }
}

/* The truth as it was at target_ms: the newest change at or before that time. Wrap safe. */
static void hist_lookup(const lg_sensor_model_t *m, uint32_t target_ms, uint8_t *occupied,
                        uint16_t *axles) {
    uint32_t k;

    for (k = 0; k < m->hist_count; k++) {
        uint32_t idx = (m->hist_head + LG_SENSOR_HISTORY - k) % LG_SENSOR_HISTORY;
        int32_t d = (int32_t)(uint32_t)(target_ms - m->hist[idx].t_ms);

        if (d >= 0) {
            *occupied = m->hist[idx].occupied;
            *axles = m->hist[idx].axles;
            return;
        }
    }
    *occupied = 0; /* before anything is known the track is reported clear */
    *axles = 0;
}

void lg_sensor_model_init(lg_sensor_model_t *m, uint32_t seed, uint32_t now_ms) {
    int i;

    memset(m, 0, sizeof *m);
    m->rng = seed != 0 ? seed : 0x9E3779B9u;
    for (i = 0; i < 4; i++) {
        (void)next_rng(m); /* a poor seed (1, 2, 3) needs a few rounds before it looks random */
    }
    m->fault = LG_SFAULT_NONE;
    m->fault_since_ms = now_ms;
    hist_push(m, now_ms, 0, 0);
}

const char *lg_sensor_fault_name(uint8_t mode) {
    switch (mode) {
    case LG_SFAULT_NONE: return "NONE";
    case LG_SFAULT_NOISE: return "NOISE";
    case LG_SFAULT_DROPOUT: return "DROPOUT";
    case LG_SFAULT_STUCK_OCCUPIED: return "STUCK_OCCUPIED";
    case LG_SFAULT_STUCK_CLEAR: return "STUCK_CLEAR";
    case LG_SFAULT_DELAY: return "DELAY";
    case LG_SFAULT_DRIFT: return "DRIFT";
    case LG_SFAULT_CRASH: return "CRASH";
    case LG_SFAULT_HANG: return "HANG";
    default: return "UNKNOWN";
    }
}

int lg_sensor_model_set_fault(lg_sensor_model_t *m, uint8_t mode, uint32_t param, uint32_t now_ms) {
    switch (mode) {
    case LG_SFAULT_NONE:
    case LG_SFAULT_STUCK_OCCUPIED:
    case LG_SFAULT_STUCK_CLEAR:
    case LG_SFAULT_HANG:
        param = 0; /* these modes take no parameter */
        break;
    case LG_SFAULT_NOISE:
    case LG_SFAULT_DROPOUT:
    case LG_SFAULT_DRIFT:
        if (param > 100u) {
            return -1;
        }
        break;
    case LG_SFAULT_DELAY:
        if (param > LG_SENSOR_DELAY_MAX_MS) {
            return -1;
        }
        break;
    default:
        return -1; /* CRASH is a process-level fault, anything else is unknown */
    }

    /* Repeating the same fault changes nothing (a DRIFT keeps its start); a new one starts now. */
    if (mode != m->fault || param != m->param) {
        m->fault_since_ms = now_ms;
    }
    m->fault = mode;
    m->param = param;
    return 0;
}

uint32_t lg_sensor_model_noise(const lg_sensor_model_t *m, uint32_t now_ms) {
    switch (m->fault) {
    case LG_SFAULT_NOISE:
        return m->param;
    case LG_SFAULT_DRIFT: {
        int32_t elapsed = (int32_t)(uint32_t)(now_ms - m->fault_since_ms);
        uint64_t n;

        if (elapsed < 0) {
            return LG_SENSOR_BASE_NOISE;
        }
        n = LG_SENSOR_BASE_NOISE + (uint64_t)m->param * (uint64_t)elapsed / 1000u;
        return n > 100u ? 100u : (uint32_t)n;
    }
    default:
        return LG_SENSOR_BASE_NOISE;
    }
}

int lg_sensor_model_step(lg_sensor_model_t *m, uint32_t now_ms, uint8_t truth_occupied,
                         uint16_t truth_axles, lg_sensor_out_t *out) {
    uint8_t occupied = truth_occupied ? 1 : 0;
    uint16_t axles = truth_axles;
    uint32_t noise;
    int32_t amp;
    int32_t raw;

    hist_push(m, now_ms, occupied, axles); /* the history advances whatever the fault */

    if (m->fault == LG_SFAULT_HANG) {
        return 0;
    }
    if (m->fault == LG_SFAULT_DROPOUT && (next_rng(m) % 100u) < m->param) {
        return 0;
    }
    if (m->fault == LG_SFAULT_DELAY) {
        hist_lookup(m, now_ms - m->param, &occupied, &axles);
    } else if (m->fault == LG_SFAULT_STUCK_OCCUPIED) {
        occupied = 1;
    } else if (m->fault == LG_SFAULT_STUCK_CLEAR) {
        occupied = 0;
    }

    noise = lg_sensor_model_noise(m, now_ms);
    amp = (int32_t)(noise * LG_SENSOR_COUNTS_PER_NOISE);
    raw = occupied ? LG_SENSOR_OCCUPIED_LEVEL : LG_SENSOR_CLEAR_LEVEL;
    if (amp > 0) {
        raw += (int32_t)(next_rng(m) % (uint32_t)(2 * amp + 1)) - amp;
    }
    if (raw < 0) {
        raw = 0;
    }
    if (raw > LG_SENSOR_RAW_MAX) {
        raw = LG_SENSOR_RAW_MAX;
    }

    out->raw_level = (uint16_t)raw;
    out->occupied = raw > LG_SENSOR_THRESHOLD ? 1 : 0;
    out->quality = (uint8_t)(100u - noise);
    out->axle_count = axles;
    return 1;
}
