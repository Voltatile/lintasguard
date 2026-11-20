#ifndef LG_GATE_MODEL_H
#define LG_GATE_MODEL_H

/*
 * Simulated gate: barrier motor, lights, bell and the watchdog (README 4.2, 7.5, 8.5, 8.6).
 * Portable and pure: no QNX headers, no allocation, no clock reads. The caller passes now_ms
 * (CLOCK_MONOTONIC in ms) to every call. Position is in millidegrees: 0 = up, 90000 = down.
 *
 * Watchdog: the controller keeps the gate informed with commands or heartbeats. When the
 * controller wants the gate up (target UP) and nothing arrived for more than the watchdog
 * time, the gate lowers itself and switches lights and bell on. The check runs in
 * lg_gate_model_step, so with a step period of T ms the lowering starts at most T ms after
 * the watchdog time has expired.
 *
 * Time must not go backwards: a step with an older timestamp is ignored completely (watchdog
 * included) until the clock catches up. CLOCK_MONOTONIC never goes back; callers that cannot
 * guarantee that must not use this model. The same applies to gaps above 2^31 ms (24 days).
 */

#include <stddef.h>
#include <stdint.h>

#include "lg_config.h"
#include "lg_proto.h"

#define LG_GATE_POS_UP_MDEG 0
#define LG_GATE_POS_DOWN_MDEG 90000
#define LG_GATE_FACTOR_MAX_PCT 1000 /* fault factors are percent: 100 = nominal, 250 = 2.5x */
#define LG_GATE_TRAVEL_MS_MAX 1000000u /* configured travel times above this are clamped */

typedef enum { LG_GATE_TARGET_DOWN = 0, LG_GATE_TARGET_UP = 1 } lg_gate_target_t;

typedef struct {
    uint32_t travel_ms;         /* nominal lowering or raising time */
    uint32_t current_moving_ma; /* motor current while moving */
    uint32_t current_hold_ma;   /* holding current at an end position */
    uint32_t watchdog_ms;
    uint32_t fault_timeout_factor; /* a movement taking this many times nominal is a FAULT */
} lg_gate_cfg_t;

lg_gate_cfg_t lg_gate_cfg_default(void);

typedef struct {
    lg_gate_cfg_t cfg;

    uint8_t state;  /* lg_gate_state_t */
    uint8_t target; /* lg_gate_target_t: what the controller last asked for */
    uint8_t lights;
    uint8_t bell;
    uint8_t fault; /* lg_gate_fault_t (never CRASH: that is a process-level fault) */
    uint8_t moving;  /* the motor is turning this step */
    uint8_t pending; /* a movement was requested and is not complete yet */
    uint8_t reserved;

    uint32_t fault_pct; /* factor for SLOW and HIGH_CURRENT, percent */
    int32_t position_mdeg;
    uint32_t current_ma;
    uint32_t last_travel_ms; /* duration of the last completed movement */

    uint32_t last_step_ms;
    uint32_t last_feed_ms;
    uint32_t move_start_ms; /* when the movement was requested: travel_ms measures from here */
    uint32_t stall_ref_ms;  /* start of the FAULT timeout: movement start or last fault change */
    uint64_t move_acc; /* fixed-point remainder of the position integration */

    uint32_t watchdog_trips;
    uint32_t rejected; /* commands rejected as invalid */
} lg_gate_model_t;

/* Starts DOWN, lights flashing and bell on (README 8.6), target DOWN. */
void lg_gate_model_init(lg_gate_model_t *m, const lg_gate_cfg_t *cfg, uint32_t now_ms);

/* Returns 0 when accepted, -1 when the command is unknown. A valid command feeds the watchdog. */
int lg_gate_model_command(lg_gate_model_t *m, uint8_t cmd, uint32_t now_ms);

/* A heartbeat from the controller: feeds the watchdog without changing anything else. */
void lg_gate_model_feed(lg_gate_model_t *m, uint32_t now_ms);

/*
 * Fault injection. NONE clears it. SLOW and HIGH_CURRENT need a factor in
 * [100, LG_GATE_FACTOR_MAX_PCT] percent. CRASH and unknown modes are rejected (-1).
 */
int lg_gate_model_set_fault(lg_gate_model_t *m, uint8_t mode, uint32_t factor_pct, uint32_t now_ms);

/* Advance the simulation to now_ms: watchdog first, then the motor. */
void lg_gate_model_step(lg_gate_model_t *m, uint32_t now_ms);

/* Fills the payload of LG_MSG_GATE_STATE (the caller fills hdr). */
void lg_gate_model_fill_state(const lg_gate_model_t *m, lg_gate_state_msg_t *out);

/* One line for /dev/lg/gate, newline terminated and NUL terminated. Returns the length. */
size_t lg_gate_model_text(const lg_gate_model_t *m, char *buf, size_t len);

const char *lg_gate_state_name(uint8_t state);
const char *lg_gate_fault_name(uint8_t fault);

#endif
