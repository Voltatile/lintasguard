#ifndef LG_METRICS_H
#define LG_METRICS_H

/*
 * Run metrics (README 8.7). Portable and pure: the caller feeds one observation per
 * tick. Island occupancy here is GROUND TRUTH from the plant, used for evaluation only;
 * the controller itself never sees it. Times are in milliseconds.
 */

#include <stdint.h>

#include "lg_config.h"
#include "lg_proto.h"

typedef struct {
    uint32_t count;
    uint32_t last_ms;
    uint32_t max_ms;
    uint64_t sum_ms;
} lg_stat_t;

typedef struct {
    uint32_t start_ms;
    uint32_t last_ms;
    uint8_t started;

    /* Previous observation, for edge detection. */
    uint8_t prev_gate_state;
    uint8_t prev_lights_on;
    uint8_t prev_island;
    uint8_t prev_wrong_side;

    uint32_t lights_on_since_ms;
    uint32_t gate_down_since_ms;
    uint8_t lights_on_valid;
    uint8_t gate_down_valid;

    /* Results. */
    lg_stat_t warning;  /* lights on until the train reaches the island */
    lg_stat_t closure;  /* gate DOWN until the gate starts rising, per train */
    lg_stat_t recovery; /* fault injected until normal operation resumes */
    uint32_t trains;
    uint32_t trains_without_warning; /* train on the island with the lights off */
    uint64_t closed_ms;              /* total time the gate was not UP (road blocked) */
    uint64_t observed_ms;            /* total observed time, from the first tick */
    uint32_t deadline_misses;
    uint32_t wrong_side_events; /* train on the island while the gate is not DOWN (edges) */
    uint32_t wrong_side_ticks;

    uint32_t fault_start_ms;
    uint8_t fault_open;
} lg_metrics_t;

void lg_metrics_init(lg_metrics_t *m, uint32_t now_ms);

/* Feed one observation. The gate starts DOWN at boot, so pass LG_GATE_DOWN until it moves. */
void lg_metrics_tick(lg_metrics_t *m, uint32_t now_ms, uint8_t gate_state, uint8_t lights_on,
                     uint8_t island_truth_occupied, uint8_t tick_late);

void lg_metrics_fault_begin(lg_metrics_t *m, uint32_t now_ms);
void lg_metrics_fault_end(lg_metrics_t *m, uint32_t now_ms);

/* True when the controller tick came later than LG_TICK_MS + LG_TICK_TOLERANCE_MS. */
int lg_tick_late(uint64_t prev_ns, uint64_t now_ns);

/* Total closure scaled to one hour of observed time, in ms per hour (0 when nothing observed). */
uint64_t lg_metrics_closure_ms_per_hour(const lg_metrics_t *m);

/* Hard check used by every scenario and campaign: wrong-side events must be 0. */
int lg_metrics_wrong_side_ok(const lg_metrics_t *m);

#endif
