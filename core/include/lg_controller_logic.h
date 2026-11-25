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
 *  - The gate target is DOWN in level 4, while a direction is ACTIVE and its delay is due, and while a
 *    usable island sensor reports a train. It is UP only when none of these applies.
 *  - Level 3 (Step 12, README 8.4, Decisions log): a direction without a trustworthy approach sensor is
 *    ACTIVE while one of its timetable windows is OPEN (lg_win_*). A window opens at eta - lead only while
 *    the direction is in that state; it is confirmed when the island sensor of the direction saw a train,
 *    the island axle count of the direction moved since the window opened and the clearing rules hold
 *    (level 3 rules: sighting, T_clear, minimum down time, no other direction approaching), or when a guard
 *    confirms the direction. An OPEN window keeps the direction ACTIVE until it is confirmed, also past its
 *    end (overdue: one notification to the guard) and also after the level went down again. A direction
 *    whose island sensor is lost clears only with a guard confirmation. schedule_ok for the mode manager:
 *    a reply with LG_SCHED_INFO for the direction within info_timeout_ms and lg-schedule not DEAD.
 *    Notifications: entering level 3 tells the station, the guard and the road sign; leaving it (to level
 *    0..2) tells the station and the road sign; level 3 to 4 tells all three that the crossing is closed.
 *  - Lights and bell stay on until the gate is UP again.
 *  - A direction is cleared only when lg_clear_step says so AND no train of the same direction was
 *    seen approaching during the last T_clear (a following train, sensor flicker).
 *  - Health verdicts of lg-health (Step 11) are merged in: the effective verdict of a sensor, the gate
 *    and the battery is the worse of the rules below and the latest lg-health verdict. A WARNING gives
 *    level 1 and changes nothing in the gate cycle (README 3: road impact none). The rules below stay
 *    as a fast safety net at the controller's priority (Decisions log, Step 11). lg-health silent for
 *    health_silence_ms after it was heard, or not heard yet while health_required: treated like a health
 *    process that is restarting (level 2; with health_required the boot lock stays).
 *  - Interim health (kept as the controller's own rules in Step 11): a sensor is FAILED when it never sent a
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
#include "lg_timetable.h"
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
    LG_CTL_DIAG_PROC_DEAD = 7,      /* the supervisor gave up on the sensor process (restart budget) */
    LG_CTL_DIAG_GATE_FAULT = 8,     /* the gate reports FAULT (a movement took far too long) */
    /* Step 12, level 3 notifications (to station, guard, road sign). */
    LG_CTL_DIAG_DETECTION_LOST = 9, /* level 3: no trustworthy detection in a direction, timetable windows */
    LG_CTL_DIAG_NO_SCHEDULE = 10,   /* level 3 to 4: the schedule information was lost, crossing closed */
    LG_CTL_DIAG_TRAIN_OVERDUE = 11, /* a level 3 window ended without island confirmation */
    LG_CTL_DIAG_CROSSING_CLOSED = 12, /* level 3 to 4 for another reason than lost schedule information */
    LG_CTL_DIAG_HEALTH_BASE = 16    /* 16 + lg_diag_t: the diagnosis of an lg-health verdict (Step 11) */
} lg_ctl_diag_t;

/* Who a NOTIFY event is for (Step 12). Text key to=... */
typedef enum {
    LG_CTL_TO_MAINTENANCE = 0,
    LG_CTL_TO_STATION = 1,
    LG_CTL_TO_GUARD = 2,
    LG_CTL_TO_ROAD_SIGN = 3
} lg_ctl_target_t;

/*
 * Controller events (README 7.2 LG_MSG_EVENT, Step 9): level changes, and a maintenance ticket when a
 * sensor that had been OK becomes FAILED (URGENT), plus an INFO event when it is OK again. Kept in a
 * fixed ring inside lg_ctl_t; the oldest entry is overwritten. lg-bridge (Step 10) reads the same ring.
 * Step 11 (early warning): a component (sensor, gate, battery) that was OK and gets a WARNING raises a
 * WARNING notification with the health diagnosis and trend (once per episode); the gate and the battery
 * raise an URGENT ticket when FAILED; a WARNING episode that ends in FAILED adds a METRIC event
 * lead_time_ms = time of the FAILED minus time of the first WARNING of the episode.
 */
#define LG_CTL_EVENT_RING 32
#define LG_CTL_EVENT_NO_SENSOR 0xFFu
#define LG_CTL_EVENT_TEXT_MAX 256 /* every event text line fits (test_controller_health) */

typedef struct {
    uint32_t seq;  /* 1, 2, ... since init; 0 = empty slot */
    uint32_t t_ms; /* controller time of the step that raised it */
    uint8_t kind;  /* lg_event_kind_t: LEVEL_CHANGE or NOTIFY */
    uint8_t severity; /* lg_severity_t */
    uint8_t level_from; /* LEVEL_CHANGE: old level. NOTIFY: the level when the ticket was raised, */
    uint8_t level_to;   /* before the level change it causes (that one follows as its own event) */
    uint8_t dir;  /* lg_dir_t, or LG_CTL_EVENT_NO_SENSOR */
    uint8_t id;   /* lg_sensor_id_t, or LG_CTL_EVENT_NO_SENSOR */
    uint8_t code; /* NOTIFY: lg_ctl_diag_t; LEVEL_CHANGE: lg_mode_reason_t; METRIC: lg_ctl_metric_t */
    uint8_t comp; /* lg_component_t of a NOTIFY or METRIC, or LG_CTL_EVENT_NO_SENSOR */
    int32_t value; /* NOTIFY: trend in seconds (-1 unknown); METRIC: the value */
    uint8_t target; /* NOTIFY: lg_ctl_target_t (Step 12). Station, guard and road sign events carry the
                     * direction in dir (or LG_CTL_EVENT_NO_SENSOR) and no component. */
    uint8_t reserved_e[3];
} lg_ctl_event_t;

typedef enum { LG_CTL_METRIC_LEAD_TIME_MS = 1 } lg_ctl_metric_t;

#define LG_CTL_UNMET_SAME_DIR (1u << 8) /* extra bit next to the LG_CLEAR_* mask */
#define LG_CTL_UNMET_WINDOW (1u << 9)   /* Step 12: an OPEN window, no train of this direction passed the island yet */

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
    uint32_t health_silence_ms;  /* lg-health heard once, then no verdict this long: health lost */
    uint8_t health_required;     /* lg-health is installed (-H): no boot release before its verdicts */
    uint8_t restarted;           /* started by the supervisor after an earlier controller: no boot release */
    uint8_t reserved_cfg[2];
    uint32_t info_timeout_ms;    /* Step 12: no schedule information for a direction this long: level 4 */
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

    /* Health verdicts from lg-health (Step 11). The effective verdict of a component is the worse of
     * the controller's own rules and the latest lg-health verdict (lg-health can only make it worse). */
    uint8_t health_seen[LG_COMP_COUNT];
    uint8_t health_verdict[LG_COMP_COUNT]; /* lg_verdict_t, unknown values stored as FAILED */
    uint8_t health_score[LG_COMP_COUNT];
    uint8_t health_armed;  /* a verdict was received: from now on lg-health must keep talking */
    uint8_t health_silent; /* latched: armed and silent for health_silence_ms, cleared by a verdict */
    uint8_t reserved_h[2];
    uint16_t health_diag[LG_COMP_COUNT];   /* lg_diag_t */
    int32_t health_trend_s[LG_COMP_COUNT];
    uint32_t health_rx_ms;
    uint32_t health_rx;
    uint32_t health_rejected;
    uint32_t health_unassessed; /* OK verdicts not stored because they were not assessed (review M1) */

    /* Early-warning episodes per component (Step 11). */
    uint8_t comp_verdict[LG_COMP_COUNT]; /* effective verdict of the last step */
    uint8_t comp_ever_ok[LG_COMP_COUNT];
    uint8_t warn_open[LG_COMP_COUNT];    /* a WARNING episode is running since warn_since_ms */
    uint8_t comp_ticket[LG_COMP_COUNT];  /* gate/battery: an URGENT ticket is open */
    uint8_t comp_from_health[LG_COMP_COUNT]; /* the current non-OK verdict came from lg-health */
    uint32_t warn_since_ms[LG_COMP_COUNT];
    uint32_t warnings;          /* WARNING notifications raised */
    uint32_t lead_times;        /* lead_time_ms metrics raised */
    uint32_t last_lead_time_ms; /* the latest of them */

    /* Level 3 (Step 12): schedule information and timetable windows per direction. */
    lg_win_dir_t win[LG_DIR_COUNT];
    uint8_t sched_heard[LG_DIR_COUNT];  /* a reply with LG_SCHED_INFO was received for this direction */
    uint8_t sched_ok[LG_DIR_COUNT];     /* schedule_ok given to the mode manager in the last step */
    uint8_t guard_confirm_pending[LG_DIR_COUNT]; /* a guard confirmed the direction clear (consumed by a step) */
    uint8_t uncertain[LG_DIR_COUNT];    /* no trustworthy approach sensor in the last step */
    uint8_t win_active[LG_DIR_COUNT];   /* the direction is active because of an OPEN window */
    uint8_t l3_dir;                     /* direction named in the level 3 notifications */
    uint8_t reserved_s[3];
    uint32_t sched_rx_ms[LG_DIR_COUNT]; /* last reply with LG_SCHED_INFO */
    uint32_t sched_rx;
    uint32_t sched_rejected;
    uint32_t guard_confirms;
    uint32_t guard_confirms_used; /* confirmations that confirmed a window or cleared a level 3 cycle */
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

/*
 * A verdict from lg-health (Step 11). Returns 0 when stored and -1 when refused (component outside the
 * table, score above 100). A verdict outside lg_verdict_t is stored as FAILED (fails closed); a trend
 * below -1 is stored as -1. Any accepted verdict arms the health silence check.
 */
int lg_ctl_on_health_verdict(lg_ctl_t *c, uint8_t comp, uint8_t score, uint8_t verdict, uint16_t diag,
                             int32_t trend_s, uint32_t now_ms);

/*
 * The same with the verdict flags (LG_HV_*). A verdict without LG_HV_ASSESSED that would replace an earlier
 * WARNING or FAILED by OK is not stored (it still counts as a sign of life): a restarted lg-health has not
 * looked yet, which is no repair (review M1). lg_ctl_on_health_verdict = this with LG_HV_ASSESSED.
 */
int lg_ctl_on_health_verdict_flags(lg_ctl_t *c, uint8_t comp, uint8_t score, uint8_t verdict, uint16_t diag,
                                   int32_t trend_s, uint8_t flags, uint32_t now_ms);

/* Authorised guard acknowledgement, consumed by the next step (authentication is Step 13). */
void lg_ctl_on_guard_ack(lg_ctl_t *c);

/*
 * Authorised guard confirmation that a direction is clear (Step 12, lgctl guard-confirm), consumed by the
 * next step. It confirms the OPEN windows of that direction and replaces the island condition of a level 3
 * clearing (README 8.2 rule 5); it does nothing in level 4 or when nothing waits for it. -1 for a bad dir.
 */
int lg_ctl_on_guard_confirm(lg_ctl_t *c, uint8_t dir);

/*
 * Schedule information from lg-schedule (Step 12). eta_ns is CLOCK_MONOTONIC; the controller's now_ms is
 * CLOCK_MONOTONIC in ms (truncated to 32 bits), so eta_ms = eta_ns / 10^6 truncated. Returns 0 when stored
 * and -1 when refused (nothing stored, counted): bad direction, count above LG_SCHED_ENTRIES, an entry with
 * id 0, unknown source, confidence 0 or above 100, lead above LG_TT_LEAD_MAX_MS or tolerance above
 * LG_TT_TOL_MAX_MS. A reply without LG_SCHED_INFO updates the windows but does not refresh the information
 * timer: after info_timeout_ms without information, a direction without trustworthy detection is level 4.
 */
int lg_ctl_on_sched_reply(lg_ctl_t *c, const lg_sched_reply_t *r, uint32_t now_ms);

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
const char *lg_ctl_target_name(uint8_t target); /* maintenance, station, guard, road_sign */

/* One line of text for /dev/lg/controller, newline and NUL terminated. Returns the length. */
size_t lg_ctl_text(const lg_ctl_t *c, char *buf, size_t len);

#endif
