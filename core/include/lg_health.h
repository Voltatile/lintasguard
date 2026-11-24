#ifndef LG_HEALTH_H
#define LG_HEALTH_H

/*
 * Health scoring (README 8.3, Step 11). Portable and pure: the caller (lg-health on the target, the
 * replays on the host) reads the cumulative counters of each device and passes them in with the time;
 * nothing is read from the system, nothing is allocated, no floating point (milli units).
 *
 * Inputs are the devices' own cumulative counters, so a statistic always covers every 10 ms sample
 * even when lg-health reads only every 200 ms, and a late read (lg-health runs at priority 20 and
 * can be held up) only makes one window longer, it never invents a dropout. The noise window length
 * is taken from the sensor's own tick count, not from lg-health's clock.
 *
 * Per sensor (component 0..5):
 *  - noise: mean |raw - nominal level of the reading| per window / 4 = noise units (the sensor model
 *    spreads raw uniformly over +- 8 counts per unit). EWMA (1/8) and a one-sided CUSUM against a
 *    baseline learned during learn_ms (between base noise and base + learn_max).
 *  - dropouts: ticks without a usable sample (none sent, or quality 0) per mille, EWMA (1/4).
 *  - approach pairs A/B: response delay (this rise minus the partner's rise of the same train,
 *    EWMA 1/2) and disagreement: the partner completed a plausible run (LG_T_DISAGREE_MS to
 *    LG_SENSOR_MAX_OCCUPIED_MS, the same bounds as the controller's MISSED_TRAIN rule) and this
 *    sensor did not rise; only a sensor that is itself OK can accuse; a run that never ends (stuck
 *    occupied) or a blip accuses nobody. Latched until disagree_clear agreeing trains.
 * Gate: per completed movement, travel time and peak current against nominal (EWMA 1/2, CUSUM); one
 * movement at or above the failure ratio is FAILED at once. Battery: voltage EWMA, slope, time to empty.
 *
 * Score 0..100 = the lowest sub-score; verdict from lg_health_verdict_from_score. A CUSUM alarm caps
 * the score at 69 (WARNING before the level threshold is reached: the early warning). The verdict gets
 * worse at once and better only after the better verdict held for ok_hold_ms (and score >= ok_score
 * for OK). Trend: seconds until the deciding statistic reaches its failure threshold at the current
 * slope, -1 when unknown or when the verdict is OK.
 */

#include <stddef.h>
#include <stdint.h>

#include "lg_config.h"
#include "lg_json_mini.h"
#include "lg_proto.h"

/* Maps a score 0..100 to OK / WARNING / FAILED using the thresholds in lg_config.h. */
lg_verdict_t lg_health_verdict_from_score(uint8_t score);

typedef struct {
    uint32_t poll_ms;      /* read period of a component with verdict OK */
    uint32_t poll_fast_ms; /* read period of a component that is not OK */
    uint32_t learn_ms;
    uint32_t sensor_period_ms; /* one sensor tick, for the window length */
    int32_t noise_base_milli;  /* baseline floor */
    int32_t noise_learn_max_milli;
    int32_t noise_fail_milli;
    int32_t cusum_k_milli;
    int32_t cusum_h_milli; /* milli unit-seconds */
    uint32_t dropout_warn_pm;
    uint32_t dropout_fail_pm;
    uint32_t delay_zero_ms;
    uint32_t delay_fail_ms;
    uint32_t pair_window_ms;
    uint32_t disagree_clear;
    uint32_t gate_travel_nominal_ms;
    uint32_t gate_travel_fail_pct;
    uint32_t gate_current_nominal_ma;
    uint32_t gate_current_fail_pct;
    uint32_t gate_cusum_k_pct;
    uint32_t gate_cusum_h_pct;
    int32_t battery_full_mv;
    int32_t battery_empty_mv;
    int32_t battery_warn_mv;
    int32_t battery_fail_mv;
    uint32_t battery_tte_warn_s;
    uint32_t battery_tte_fail_s;
    uint8_t ok_score;
    uint8_t reserved[3];
    uint32_t ok_hold_ms;
    uint32_t nodata_polls;
} lg_health_cfg_t;

lg_health_cfg_t lg_health_cfg_default(void);

/* Cumulative counters of one sensor (from DCMD_LG_SENSOR_STATS on the target). */
typedef struct {
    uint32_t ticks;      /* sensor ticks */
    uint32_t no_sample;  /* ticks without a sample (DROPOUT, HANG) */
    uint32_t blind;      /* samples with quality 0 */
    uint32_t dev_count;  /* samples that went into dev_sum */
    uint64_t dev_sum;    /* sum of |raw - nominal level of the reading| */
    uint32_t rise_count; /* clear -> occupied transitions */
    uint32_t last_rise_ms; /* time of the latest rise (sensor clock = CLOCK_MONOTONIC ms) */
    uint32_t last_fall_ms; /* time of the latest fall (occupied -> clear) */
} lg_health_sensor_in_t;

/* Gate and battery counters (from DCMD_LG_GATE_STATS). */
typedef struct {
    uint32_t moves_done;     /* completed movements */
    uint32_t last_travel_ms; /* duration of the latest completed movement */
    uint32_t last_peak_ma;   /* peak motor current of the latest completed movement */
    uint32_t battery_mv;
} lg_health_gate_in_t;

typedef struct {
    uint8_t score;
    uint8_t verdict; /* lg_verdict_t */
    uint16_t diag;   /* lg_diag_t */
    int32_t trend_s; /* -1 = unknown */
    uint8_t assessed; /* the verdict rests on data or a maintenance reset (LG_HV_ASSESSED) */
    uint8_t reserved[3];
} lg_health_out_t;

typedef struct {
    /* Verdict state. */
    uint8_t score;
    uint8_t verdict;
    uint16_t diag;
    int32_t trend_s;
    uint8_t have_prev;  /* a previous reading exists: deltas can be taken */
    uint8_t better_tracking;
    uint8_t better_target; /* the better verdict being held since better_since_ms */
    uint8_t assessed;      /* judged on data since init (a reset by maintenance counts as judged) */
    uint8_t learning;
    uint8_t nodata;     /* failed reads in a row */
    uint32_t start_ms;  /* first reading after init or reset */
    uint32_t better_since_ms;
    uint32_t changes;   /* verdict changes, for the status text */

    /* Sensor. */
    uint32_t p_ticks, p_no_sample, p_blind, p_dev_count, p_rise_count;
    uint64_t p_dev_sum;
    int32_t noise_milli;      /* latest window */
    int32_t noise_ewma_milli;
    int32_t noise_base_milli;
    int32_t noise_slope_milli; /* milli units per second, EWMA */
    int64_t learn_sum;        /* sum of window means times samples, during learning */
    uint32_t learn_n;
    int32_t cusum_milli;      /* milli unit-seconds */
    uint8_t cusum_alarm;
    uint8_t have_noise;
    uint8_t rise_pending; /* a rise not yet matched with the partner */
    uint8_t disagree;     /* latched: the partner saw a train this sensor did not */
    uint32_t agree_run;
    uint32_t rise_ms;
    uint32_t fall_ms;     /* latest fall of this sensor (from its counters) */
    uint8_t fell;         /* the run of the pending rise has ended (fall_ms belongs to it) */
    uint32_t drop_ewma_pm;
    uint32_t delay_ewma_ms;
    uint32_t delay_samples;
    uint32_t disagreements;

    /* Gate. */
    uint32_t p_moves;
    uint8_t have_move;
    uint8_t travel_alarm;
    uint8_t current_alarm;
    uint8_t latest_fail; /* the latest movement alone was at or above a failure ratio */
    int32_t travel_ewma_pm;  /* per mille of nominal */
    int32_t current_ewma_pm;
    int32_t travel_cusum_pm;
    int32_t current_cusum_pm;
    int32_t gate_slope_pm;   /* per mille of nominal per 1000 s, EWMA, of the deciding statistic */
    uint32_t last_move_ms;
    uint32_t moves;

    /* Battery. */
    uint8_t have_batt;
    uint8_t reserved_b[3];
    int64_t batt_ewma_uv;   /* microvolts, EWMA (1/8) */
    int32_t batt_slope_umv; /* milli mV per second (= uV/s), EWMA; negative = draining */
    int32_t batt_tte_s;     /* time to empty, -1 = not draining */
    uint32_t batt_ms;
} lg_health_comp_t;

typedef struct {
    lg_health_cfg_t cfg;
    lg_health_comp_t c[LG_COMP_COUNT];
} lg_health_t;

void lg_health_init(lg_health_t *h, const lg_health_cfg_t *cfg, uint32_t now_ms);

/* Forgets everything known about one component (maintenance after a repair): verdict OK, learning
 * again, and assessed (the technician's word that it is repaired). LG_COMP_GATE also resets the battery.
 * Out-of-range components are ignored. lg_health_init leaves every component OK but NOT assessed. */
void lg_health_reset(lg_health_t *h, uint8_t comp, uint32_t now_ms);

/*
 * One reading of a sensor (comp 0..5). A counter that went backwards (a restarted sensor process)
 * restarts the deltas: that reading only sets the reference. Returns 0, or -1 for a bad comp.
 */
int lg_health_on_sensor(lg_health_t *h, uint8_t comp, const lg_health_sensor_in_t *in, uint32_t now_ms);

/* One reading of the gate: updates LG_COMP_GATE and LG_COMP_BATTERY. */
void lg_health_on_gate(lg_health_t *h, const lg_health_gate_in_t *in, uint32_t now_ms);

/* A read of the device failed. After nodata_polls in a row the verdict is at least WARNING (NO_DATA).
 * LG_COMP_GATE covers the battery too. */
void lg_health_on_no_data(lg_health_t *h, uint8_t comp, uint32_t now_ms);

lg_health_out_t lg_health_get(const lg_health_t *h, uint8_t comp);

/* How often the component should be read now (faster when it is not OK). */
uint32_t lg_health_poll_period_ms(const lg_health_t *h, uint8_t comp);

const char *lg_health_diag_name(uint16_t diag);
/* Human-readable diagnosis for maintenance notifications (README 8.3). */
const char *lg_health_diag_text(uint16_t diag);
/* "west/a", ..., "gate", "battery"; "?" for anything else. */
const char *lg_health_comp_name(uint8_t comp);

/*
 * Overrides from config/thresholds.json (README 8.3): an object whose members are optional and named
 * after the fields (noise in noise units, may have 3 decimals; everything else whole numbers). An
 * unknown key, a value out of range or an inconsistent order (warn after fail) is refused: -1 and
 * nothing changes. out holds the defaults on entry.
 */
int lg_health_cfg_from_json(const lg_json_doc_t *doc, lg_health_cfg_t *out, char *err, size_t errlen);

/* One text line per component for /dev/lg/health, NUL terminated. Returns the length. */
size_t lg_health_text(const lg_health_t *h, char *buf, size_t len);

#endif
