#ifndef LG_CONTROLLER_LOGIC_H
#define LG_CONTROLLER_LOGIC_H

/*
 * Decision logic of lg-controller (README 3, 4.3, 8.1, 8.2). Portable and pure: the caller feeds
 * samples, gate replies and the time, and reads back what the gate should be told. Nothing is read
 * from the system, nothing is allocated.
 *
 * Per tick: sensor verdicts -> mode manager (level) -> voting per direction -> clearing per
 * direction -> gate target, lights and bell.
 *
 * Rules (Decisions log, Step 7):
 *  - A direction becomes ACTIVE when a usable approach sensor reports occupied. From then on the
 *    gate cycle is completed: the gate is lowered and only released by the clearing rules.
 *  - Lights and bell go on at activation. The gate is commanded down after lg_lower_delay_ms
 *    (level 0) or at once when it is not UP (a cycle that is still raising, level 2 and above).
 *  - The gate target is DOWN in level 3 and 4 (the level 3 timetable windows arrive in Step 12, until
 *    then level 3 holds the gate down), while a direction is ACTIVE and its delay is due, and while a
 *    usable island sensor reports a train. It is UP only when none of these applies.
 *  - Lights and bell stay on until the gate is UP again.
 *  - A direction is cleared only when lg_clear_step says so AND no train of the same direction was
 *    seen approaching during the last T_clear (a following train, sensor flicker).
 *  - Interim health (replaced by lg-health in Step 11): a sensor is FAILED when it never sent a
 *    sample, was silent for sensor_silence_ms, or reported quality 0 for sensor_silence_ms in a row
 *    (Step 9; a single blind sample only makes the sensor unusable for that tick), otherwise OK. The gate is
 *    FAILED when it was silent for gate_silence_ms or reports FAULT.
 *  - A gate that has never answered is a WARNING (not a hard event, the boot lock stays) for
 *    gate_boot_grace_ms from the first step; after that it is silent: FAILED and the self check fails.
 *  - A direction is also ACTIVE while the axle count in (approach sensor) differs from the count out
 *    (island sensor), for example after a controller restart with a train between the two: the counts
 *    are cumulative and wrap modulo 65536, so only the difference matters.
 *  - A single stale tick (sample older than LG_SAMPLE_MAX_AGE_MS) only makes that sensor unusable for
 *    that tick; it never changes a verdict or the level. Samples that stay stale for
 *    sensor_silence_ms in a row make the sensor FAILED (they arrive, but are never usable).
 *  - Interim stuck rules for the approach sensors A and B of one direction (Step 9, Decisions log;
 *    both sit at the same point, so a healthy pair rises and falls together). Judged only while both
 *    sensors are alive (verdict OK and the last sample not blind; a stale sample that has not yet
 *    made the sensor FAILED still counts as alive, with its last occupancy):
 *      MISSED_TRAIN:   the partner completed a run of t_disagree to max_occupied_ms (shorter is a
 *                      blip, longer is no train) and this sensor was not occupied once from the
 *                      partner's rise until its fall + t_disagree.
 *      STUCK_OCCUPIED: occupied without a break for max_occupied_ms while the partner is clear.
 *    Both are LATCHED (the sensor is FAILED and isolated by the vote) until one run on which the two
 *    sensors agree (rise and fall each within t_disagree), or until the supervisor reports the sensor
 *    process RESTARTING or DEAD (a new process). A silence or a stale stretch (a VM stall) keeps the
 *    latch. The safe side is kept: a disagreement keeps the direction active until it is judged.
 */


#include <stddef.h>
#include <stdint.h>

#include "lg_config.h"
#include "lg_gate_model.h"
#include "lg_mode.h"
#include "lg_proto.h"
#include "lg_voting.h"

/*
 * Why a sensor is FAILED. Values 1 and 2 are the stuck rules (stored in lg_ctl_sensor_t.stuck), the
 * others are only used in events. Not part of the protocol.
 */
typedef enum {
    LG_CTL_DIAG_NONE = 0,
    LG_CTL_DIAG_MISSED_TRAIN = 1,   /* the partner saw a whole train, this sensor nothing */
    LG_CTL_DIAG_STUCK_OCCUPIED = 2, /* occupied far longer than any train, partner clear */
    LG_CTL_DIAG_SILENT = 3,         /* no sample for sensor_silence_ms (crash, hang, restart) */
    LG_CTL_DIAG_STALE = 4,          /* samples arrive but are too old to use */
    LG_CTL_DIAG_BLIND = 5,          /* quality 0 */
    LG_CTL_DIAG_RESTARTING = 6,     /* the supervisor reports the sensor process RESTARTING */
    LG_CTL_DIAG_PROC_DEAD = 7       /* the supervisor gave up on the sensor process (restart budget) */
} lg_ctl_diag_t;

/*
 * Controller events (README 7.2 LG_MSG_EVENT, Step 9): level changes, and a maintenance ticket when a
 * sensor that had been OK becomes FAILED (URGENT), plus an INFO event when it is OK again. Kept in a
 * fixed ring inside lg_ctl_t; the oldest entry is overwritten. lg-bridge (Step 10) reads the same ring.
 */
#define LG_CTL_EVENT_RING 32
#define LG_CTL_EVENT_NO_SENSOR 0xFFu

typedef struct {
    uint32_t seq;  /* 1, 2, ... since init; 0 = empty slot */
    uint32_t t_ms; /* controller time of the step that raised it */
    uint8_t kind;  /* lg_event_kind_t: LEVEL_CHANGE or NOTIFY */
    uint8_t severity; /* lg_severity_t */
    uint8_t level_from; /* LEVEL_CHANGE: old level. NOTIFY: the level when the ticket was raised, */
    uint8_t level_to;   /* before the level change it causes (that one follows as its own event) */
    uint8_t dir;  /* lg_dir_t, or LG_CTL_EVENT_NO_SENSOR */
    uint8_t id;   /* lg_sensor_id_t, or LG_CTL_EVENT_NO_SENSOR */
    uint8_t code; /* NOTIFY: lg_ctl_diag_t; LEVEL_CHANGE: lg_mode_reason_t */
    uint8_t reserved;
} lg_ctl_event_t;

#define LG_CTL_UNMET_SAME_DIR (1u << 8) /* extra bit next to the LG_CLEAR_* mask */

typedef struct {
    lg_vote_cfg_t vote;
    lg_clear_cfg_t clear;
    lg_mode_cfg_t mode;
    uint32_t sensor_silence_ms; /* no sample for this long: sensor FAILED */
    uint32_t gate_silence_ms;   /* no gate reply for this long: gate FAILED, self check fails */
    uint32_t gate_boot_grace_ms; /* a gate never heard yet is only a WARNING for this long */
    uint32_t sup_silence_ms;     /* supervisor heard once, then silent this long: DEAD */
    uint32_t max_occupied_ms;    /* stuck rules: longest plausible occupied run of an approach sensor */
    uint32_t t_disagree_ms;      /* stuck rules: A/B edges this far apart still agree */
    uint32_t stall_ms;           /* a step this late after the previous one: the controller stalled */
    uint8_t restarted;           /* started by the supervisor after an earlier controller: no boot release */
} lg_ctl_cfg_t;

lg_ctl_cfg_t lg_ctl_cfg_default(void);

typedef struct {
    uint8_t seen;
    uint8_t occupied;
    uint8_t quality;
    uint8_t unusable_tracking; /* no usable (fresh, not blind) sample since unusable_since_ms */
    uint16_t axles;
    uint8_t silent;       /* latched: nothing received for sensor_silence_ms, cleared by a sample */
    uint8_t unusable_failed; /* latched: stale or blind for sensor_silence_ms in a row, cleared by a usable sample */
    uint32_t unusable_since_ms;
    uint32_t sample_ms; /* timestamp the sensor put in the header */
    uint32_t rx_ms;     /* when the controller received it */

    /* Stuck rules (approach sensors only). */
    uint8_t run_open; /* occupied run in progress, started at rise_ms */
    uint8_t run_done; /* a run ended at fall_ms and waits to be judged */
    uint8_t saw;      /* occupied at least once while the partner's run was open or being judged */
    uint8_t stuck;    /* lg_ctl_diag_t, latched */
    uint32_t rise_ms;
    uint32_t fall_ms;

} lg_ctl_sensor_t;

typedef struct {
    lg_ctl_cfg_t cfg;
    lg_mode_t mode;

    lg_ctl_sensor_t sensor[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];

    uint8_t started;     /* the first step has run */
    uint8_t gate_silent; /* the gate counted as silent in the last step */
    uint8_t gate_seen;
    uint8_t gate_state; /* last reply */
    uint8_t gate_down_tracking;
    uint8_t guard_ack_pending;
    uint32_t gate_rx_ms;
    uint32_t gate_down_since_ms;
    uint32_t first_step_ms; /* the gate grace and silence clocks start here, not at init */

    /* Supervisor reports (Step 8). All RUNNING until the supervisor says otherwise. */
    uint8_t proc_state[LG_PROC_COUNT]; /* lg_proc_state_t per lg_proc_id_t */
    uint8_t proc_restarts[LG_PROC_COUNT];
    uint8_t sup_armed;  /* a PROC_STATUS was received: from now on the supervisor must keep talking */
    uint8_t sup_silent; /* latched: armed and silent for sup_silence_ms, cleared by a PROC_STATUS */
    uint32_t sup_rx_ms;

    uint8_t active[LG_DIR_COUNT];
    lg_clear_t clear[LG_DIR_COUNT];
    uint32_t last_approach_ms[LG_DIR_COUNT];
    uint32_t active_since_ms; /* when the first direction became active */
    uint8_t was_active;

    /* Counters for the status text. */
    uint32_t ticks;
    uint32_t late_ticks; /* set by the caller through lg_ctl_note_late */
    uint32_t samples;
    uint32_t samples_rejected;
    uint32_t gate_replies;
    uint32_t gate_timeouts;
    uint32_t proc_status_rx;
    uint32_t proc_status_rejected;
    uint32_t proc_status_unknown; /* a state outside lg_proc_state_t, stored as DEAD */
    uint32_t guard_acks;
    uint32_t activations;
    uint32_t cycles; /* all directions idle again after an activation */
    uint32_t last_unmet[LG_DIR_COUNT];
    uint8_t last_gate_target; /* lg_gate_target_t of the last step */
    uint8_t last_lights;
    uint8_t last_bell;
    uint8_t reserved;

    /* Events and tickets (Step 9). */
    uint8_t last_verdict[LG_DIR_COUNT][LG_SENSOR_ID_COUNT]; /* lg_verdict_t of the last step */
    uint8_t ever_ok[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];      /* was OK once: a FAILED is then a ticket */
    uint8_t ticket_open[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];  /* a ticket was raised, not yet resolved */
    uint8_t ticket_code[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];  /* lg_ctl_diag_t of the open ticket */
    uint32_t last_step_ms; /* time of the previous step (stall rule) */
    uint32_t stalls;       /* steps that came stall_ms or more after the previous one */
    uint32_t tickets; /* URGENT maintenance tickets raised */
    uint32_t events;  /* events raised (= seq of the newest) */
    lg_ctl_event_t ev[LG_CTL_EVENT_RING];
} lg_ctl_t;

typedef struct {
    uint8_t gate_target; /* lg_gate_target_t */
    uint8_t lights;
    uint8_t bell;
    uint8_t level;
    uint8_t level_changed;
    uint8_t level_from;
    uint8_t level_reason; /* lg_mode_reason_t */
    uint8_t activated_mask; /* bit per direction that became active in this step */
    uint8_t cleared_mask;   /* bit per direction that was cleared in this step */
    uint8_t reserved[3];
} lg_ctl_out_t;

void lg_ctl_init(lg_ctl_t *c, const lg_ctl_cfg_t *cfg, uint32_t now_ms);

/*
 * Stores a sample. Returns 0 when accepted and -1 when the fields are out of range (nothing
 * stored, counted in samples_rejected).
 */
int lg_ctl_on_sample(lg_ctl_t *c, uint8_t dir, uint8_t id, uint8_t occupied, uint8_t quality,
                     uint16_t axles, uint32_t sample_ms, uint32_t now_ms);

/* The gate answered a command. An out-of-range state is not stored (-1). */
int lg_ctl_on_gate_reply(lg_ctl_t *c, uint8_t gate_state, uint32_t now_ms);
void lg_ctl_note_gate_timeout(lg_ctl_t *c);
void lg_ctl_note_late(lg_ctl_t *c);

/*
 * A PROC_STATUS from the supervisor. Returns 0 when stored and -1 when refused (a process id outside
 * the table, or the id of the supervisor or the controller itself: those are derived here, nobody
 * may report them). A state outside lg_proc_state_t is stored as DEAD (fails closed). Any accepted
 * status also counts as a sign of life of the supervisor and arms its silence check: a setup without
 * supervisor (hand-started processes) is never treated as having a dead one.
 */
int lg_ctl_on_proc_status(lg_ctl_t *c, uint8_t proc, uint8_t state, uint8_t restarts, uint32_t now_ms);

/* Authorised guard acknowledgement, consumed by the next step (authentication is Step 13). */
void lg_ctl_on_guard_ack(lg_ctl_t *c);

lg_ctl_out_t lg_ctl_step(lg_ctl_t *c, uint32_t now_ms);

/* lg_ctl_diag_t of one sensor (always NONE for the island sensors and out-of-range arguments). */
uint8_t lg_ctl_sensor_diag(const lg_ctl_t *c, uint8_t dir, uint8_t id);

/*
 * Copies the events with seq >= from_seq that are still in the ring, oldest first, at most max.
 * Returns the number copied. Events older than the ring are lost (seq shows the gap).
 */
uint32_t lg_ctl_events_since(const lg_ctl_t *c, uint32_t from_seq, lg_ctl_event_t *out, uint32_t max);

/* One event as a key=value text line, newline and NUL terminated. Returns the length. */
size_t lg_ctl_event_text(const lg_ctl_event_t *e, char *buf, size_t len);

const char *lg_ctl_diag_name(uint8_t diag);

/* One line of text for /dev/lg/controller, newline and NUL terminated. Returns the length. */
size_t lg_ctl_text(const lg_ctl_t *c, char *buf, size_t len);

#endif
