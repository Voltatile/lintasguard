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
 *    sample, was silent for sensor_silence_ms, or reports quality 0, otherwise OK. The gate is
 *    FAILED when it was silent for gate_silence_ms or reports FAULT.
 *  - A gate that has never answered is a WARNING (not a hard event, the boot lock stays) for
 *    gate_boot_grace_ms from the first step; after that it is silent: FAILED and the self check fails.
 *  - A direction is also ACTIVE while the axle count in (approach sensor) differs from the count out
 *    (island sensor), for example after a controller restart with a train between the two: the counts
 *    are cumulative and wrap modulo 65536, so only the difference matters.
 *  - A single stale tick (sample older than LG_SAMPLE_MAX_AGE_MS) only makes that sensor unusable for
 *    that tick; it never changes a verdict or the level. Samples that stay stale for
 *    sensor_silence_ms in a row make the sensor FAILED (they arrive, but are never usable).
 */

#include <stddef.h>
#include <stdint.h>

#include "lg_config.h"
#include "lg_gate_model.h"
#include "lg_mode.h"
#include "lg_proto.h"
#include "lg_voting.h"

#define LG_CTL_UNMET_SAME_DIR (1u << 8) /* extra bit next to the LG_CLEAR_* mask */

typedef struct {
    lg_vote_cfg_t vote;
    lg_clear_cfg_t clear;
    lg_mode_cfg_t mode;
    uint32_t sensor_silence_ms; /* no sample for this long: sensor FAILED */
    uint32_t gate_silence_ms;   /* no gate reply for this long: gate FAILED, self check fails */
    uint32_t gate_boot_grace_ms; /* a gate never heard yet is only a WARNING for this long */
} lg_ctl_cfg_t;

lg_ctl_cfg_t lg_ctl_cfg_default(void);

typedef struct {
    uint8_t seen;
    uint8_t occupied;
    uint8_t quality;
    uint8_t stale_tracking; /* no fresh sample since stale_since_ms */
    uint16_t axles;
    uint8_t silent;       /* latched: nothing received for sensor_silence_ms, cleared by a sample */
    uint8_t stale_failed; /* latched: stale for sensor_silence_ms in a row, cleared by a fresh sample */
    uint32_t stale_since_ms;
    uint32_t sample_ms; /* timestamp the sensor put in the header */
    uint32_t rx_ms;     /* when the controller received it */
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
    uint32_t activations;
    uint32_t cycles; /* all directions idle again after an activation */
    uint32_t last_unmet[LG_DIR_COUNT];
    uint8_t last_gate_target; /* lg_gate_target_t of the last step */
    uint8_t last_lights;
    uint8_t last_bell;
    uint8_t reserved;
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

/* Authorised guard acknowledgement, consumed by the next step (authentication is Step 13). */
void lg_ctl_on_guard_ack(lg_ctl_t *c);

lg_ctl_out_t lg_ctl_step(lg_ctl_t *c, uint32_t now_ms);

/* One line of text for /dev/lg/controller, newline and NUL terminated. Returns the length. */
size_t lg_ctl_text(const lg_ctl_t *c, char *buf, size_t len);

#endif
