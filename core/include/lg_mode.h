#ifndef LG_MODE_H
#define LG_MODE_H

/*
 * Mode manager: operating levels 0..4 (README 3, 3.1, 8.5, 8.6). Portable, pure:
 * the caller passes the inputs and the time, nothing is read from the system.
 * Escalation is immediate. De-escalation needs the condition to stay clear for
 * T_stable. Level 4 is locked: it is left only after the boot check or after a
 * guard acknowledgement. An acknowledgement counts only when the inputs demand at
 * most level 1 at that moment, and is voided as soon as they demand more.
 * Unknown enum values in the inputs are treated as the worst case (FAILED / DEAD).
 */

#include <stdint.h>

#include "lg_config.h"
#include "lg_proto.h"

typedef struct {
    uint32_t t_stable_ms;      /* de-escalation window */
    uint32_t t_boot_stable_ms; /* all healthy for this long after boot */
} lg_mode_cfg_t;

lg_mode_cfg_t lg_mode_cfg_default(void);

typedef struct {
    uint8_t verdict[LG_COMP_COUNT];    /* lg_verdict_t per lg_component_t */
    uint8_t proc_state[LG_PROC_COUNT]; /* lg_proc_state_t per lg_proc_id_t */
    uint8_t schedule_ok[LG_DIR_COUNT]; /* timetable or station information usable */
    uint8_t self_check_ok;             /* controller can verify its own state */
    uint8_t guard_ack;                 /* an authorised guard acknowledged this tick (event) */
} lg_mode_in_t;

typedef enum {
    LG_REASON_NONE = 0,
    LG_REASON_BOOT = 1,           /* starting in level 4 */
    LG_REASON_BOOT_RELEASED = 2,  /* all healthy for t_boot_stable */
    LG_REASON_WARNING = 3,        /* a component below the warning threshold */
    LG_REASON_SENSOR_FAILED = 4,  /* one approach sensor lost, redundant one healthy */
    LG_REASON_PROC_RESTART = 5,   /* a driver process is restarting */
    LG_REASON_PROC_DEAD = 6,      /* restart budget exhausted for a non-critical process */
    LG_REASON_DETECT_LOST_SCHEDULE = 7, /* no trustworthy sensor, timetable available */
    LG_REASON_DETECT_LOST_NO_INFO = 8,  /* no trustworthy sensor and no timetable */
    LG_REASON_CRITICAL_PROC_DEAD = 9,   /* controller, supervisor or gate process dead */
    LG_REASON_GATE_FAILED = 10,
    LG_REASON_SELF_CHECK = 11,
    LG_REASON_STABLE = 12,        /* de-escalated after t_stable */
    LG_REASON_GUARD_ACK = 13      /* left level 4 after guard ack, healthy and stable */
} lg_mode_reason_t;

typedef struct {
    uint8_t level;          /* lg_level_t */
    uint8_t boot_lock;      /* level 4 entered by boot, released by the boot check */
    uint8_t guard_latched;  /* valid guard ack: given while the inputs demanded <= level 1 */
    uint8_t rel_kind;       /* which release the running timer belongs to: 1 boot, 2 guard */
    uint8_t prev_required;  /* required level at the previous step */
    uint8_t timing;         /* a stability timer is running */
    uint32_t since_ms;      /* when the running stability timer started */
} lg_mode_t;

typedef struct {
    uint8_t changed; /* the level changed in this step */
    uint8_t from;
    uint8_t to;
    uint8_t reason; /* lg_mode_reason_t of the change (or of the requirement when unchanged) */
} lg_mode_result_t;

/* The level the inputs demand, ignoring hysteresis and locks. Reason is optional (may be NULL). */
uint8_t lg_mode_required(const lg_mode_in_t *in, uint8_t *reason);

/* Starts in level 4 with the boot lock set (gate down). */
void lg_mode_init(lg_mode_t *m, uint32_t now_ms);

/*
 * Starts in level 4 WITHOUT the boot lock: for a controller that was restarted by the supervisor.
 * The previous controller may have died while level 4 was locked, so nothing may release it by itself;
 * a guard acknowledgement (inputs healthy, stable for T_stable) is needed.
 */
void lg_mode_init_restarted(lg_mode_t *m, uint32_t now_ms);

lg_mode_result_t lg_mode_step(lg_mode_t *m, const lg_mode_cfg_t *cfg, const lg_mode_in_t *in,
                              uint32_t now_ms);

/* Convenience for tests and the controller: every component OK and running. */
void lg_mode_in_all_healthy(lg_mode_in_t *in);

#endif
