#include "lg_metrics.h"

static void stat_add(lg_stat_t *s, uint32_t ms) {
    s->count++;
    s->last_ms = ms;
    if (ms > s->max_ms) {
        s->max_ms = ms;
    }
    s->sum_ms += ms;
}

static void stat_clear(lg_stat_t *s) {
    s->count = 0;
    s->last_ms = 0;
    s->max_ms = 0;
    s->sum_ms = 0;
}

void lg_metrics_init(lg_metrics_t *m, uint32_t now_ms) {
    m->start_ms = now_ms;
    m->last_ms = now_ms;
    m->started = 0;
    m->prev_gate_state = LG_GATE_DOWN;
    m->prev_lights_on = 0;
    m->prev_island = 0;
    m->prev_wrong_side = 0;
    m->lights_on_since_ms = 0;
    m->gate_down_since_ms = now_ms;
    m->lights_on_valid = 0;
    m->gate_down_valid = 0; /* the gate boots DOWN, but only observed closures are counted */
    stat_clear(&m->warning);
    stat_clear(&m->closure);
    stat_clear(&m->recovery);
    m->trains = 0;
    m->trains_without_warning = 0;
    m->closed_ms = 0;
    m->observed_ms = 0;
    m->deadline_misses = 0;
    m->wrong_side_events = 0;
    m->wrong_side_ticks = 0;
    m->fault_start_ms = 0;
    m->fault_open = 0;
}

void lg_metrics_tick(lg_metrics_t *m, uint32_t now_ms, uint8_t gate_state, uint8_t lights_on,
                     uint8_t island_truth_occupied, uint8_t tick_late) {
    uint8_t wrong_side;

    if (!m->started) {
        /* Observation starts at the first tick, so closed time and elapsed time share a base. */
        m->start_ms = now_ms;
        m->last_ms = now_ms;
    }
    if (m->started) {
        /* Time since the previous tick is spent in the previous gate state. */
        uint32_t dt = (uint32_t)(now_ms - m->last_ms);

        m->observed_ms += dt;
        if (m->prev_gate_state != LG_GATE_UP) {
            m->closed_ms += dt;
        }
    }
    m->started = 1;

    if (tick_late) {
        m->deadline_misses++;
    }

    /* Warning time: lights on until the train reaches the island. */
    if (lights_on && !m->prev_lights_on) {
        m->lights_on_since_ms = now_ms;
        m->lights_on_valid = 1;
    }
    if (!lights_on) {
        m->lights_on_valid = 0;
    }
    if (island_truth_occupied && !m->prev_island) {
        m->trains++;
        if (m->lights_on_valid) {
            stat_add(&m->warning, (uint32_t)(now_ms - m->lights_on_since_ms));
        } else {
            m->trains_without_warning++;
        }
    }

    /* Closure time: gate DOWN until it starts rising. */
    if (gate_state == LG_GATE_DOWN && m->prev_gate_state != LG_GATE_DOWN) {
        m->gate_down_since_ms = now_ms;
        m->gate_down_valid = 1;
    }
    if (gate_state != LG_GATE_DOWN && m->prev_gate_state == LG_GATE_DOWN && m->gate_down_valid) {
        stat_add(&m->closure, (uint32_t)(now_ms - m->gate_down_since_ms));
        m->gate_down_valid = 0;
    }

    /* Wrong side: train on the island while the gate is not DOWN. */
    wrong_side = (uint8_t)(island_truth_occupied && gate_state != LG_GATE_DOWN);
    if (wrong_side) {
        m->wrong_side_ticks++;
        if (!m->prev_wrong_side) {
            m->wrong_side_events++;
        }
    }

    m->prev_gate_state = gate_state;
    m->prev_lights_on = lights_on;
    m->prev_island = island_truth_occupied;
    m->prev_wrong_side = wrong_side;
    m->last_ms = now_ms;
}

void lg_metrics_fault_begin(lg_metrics_t *m, uint32_t now_ms) {
    if (!m->fault_open) {
        m->fault_start_ms = now_ms;
        m->fault_open = 1;
    }
}

void lg_metrics_fault_end(lg_metrics_t *m, uint32_t now_ms) {
    if (m->fault_open) {
        stat_add(&m->recovery, (uint32_t)(now_ms - m->fault_start_ms));
        m->fault_open = 0;
    }
}

int lg_tick_late(uint64_t prev_ns, uint64_t now_ns) {
    uint64_t limit_ns = (uint64_t)(LG_TICK_MS + LG_TICK_TOLERANCE_MS) * 1000000u;

    return (now_ns - prev_ns) > limit_ns;
}

uint64_t lg_metrics_closure_ms_per_hour(const lg_metrics_t *m) {
    if (m->observed_ms == 0) {
        return 0;
    }
    return m->closed_ms * 3600000u / m->observed_ms;
}

int lg_metrics_wrong_side_ok(const lg_metrics_t *m) {
    return m->wrong_side_events == 0 && m->wrong_side_ticks == 0;
}
