#include "lg_controller_logic.h"

#include <stdio.h>
#include <string.h>

#include "lg_gate_model.h"
#include "lg_time.h"

lg_ctl_cfg_t lg_ctl_cfg_default(void) {
    lg_ctl_cfg_t c;

    c.vote = lg_vote_cfg_default();
    c.clear = lg_clear_cfg_default();
    c.mode = lg_mode_cfg_default();
    c.sensor_silence_ms = LG_HEARTBEAT_TIMEOUT_MS;
    c.gate_silence_ms = LG_GATE_WATCHDOG_MS;
    c.gate_boot_grace_ms = LG_CTL_GATE_BOOT_GRACE_MS;
    return c;
}

void lg_ctl_init(lg_ctl_t *c, const lg_ctl_cfg_t *cfg, uint32_t now_ms) {
    int d;

    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    /* The gate boots DOWN. Until it has answered, level 4 (boot) holds it down anyway. */
    c->gate_state = LG_GATE_DOWN;
    c->last_gate_target = LG_GATE_TARGET_DOWN;
    c->last_lights = 1;
    c->last_bell = 1;
    for (d = 0; d < LG_DIR_COUNT; d++) {
        lg_clear_init(&c->clear[d]);
    }
    lg_mode_init(&c->mode, now_ms);
}

int lg_ctl_on_sample(lg_ctl_t *c, uint8_t dir, uint8_t id, uint8_t occupied, uint8_t quality,
                     uint16_t axles, uint32_t sample_ms, uint32_t now_ms) {
    lg_ctl_sensor_t *s;

    if (dir >= LG_DIR_COUNT || id >= LG_SENSOR_ID_COUNT || occupied > 1 || quality > 100) {
        c->samples_rejected++;
        return -1;
    }
    s = &c->sensor[dir][id];
    s->seen = 1;
    s->occupied = occupied;
    s->quality = quality;
    s->axles = axles;
    s->sample_ms = sample_ms;
    s->rx_ms = now_ms;
    s->silent = 0;
    if (!lg_elapsed_ge(now_ms, sample_ms, LG_SAMPLE_MAX_AGE_MS + 1u)) {
        s->stale_failed = 0; /* only a fresh sample clears a stale failure */
    }
    c->samples++;
    return 0;
}

int lg_ctl_on_gate_reply(lg_ctl_t *c, uint8_t gate_state, uint32_t now_ms) {
    if (gate_state > LG_GATE_FAULT) {
        return -1;
    }
    c->gate_seen = 1;
    c->gate_state = gate_state;
    c->gate_rx_ms = now_ms;
    c->gate_replies++;
    return 0;
}

void lg_ctl_note_gate_timeout(lg_ctl_t *c) {
    c->gate_timeouts++;
}

void lg_ctl_note_late(lg_ctl_t *c) {
    c->late_ticks++;
}

void lg_ctl_on_guard_ack(lg_ctl_t *c) {
    c->guard_ack_pending = 1;
}

/* Sample younger than LG_SAMPLE_MAX_AGE_MS (by the sensor's own header time). */
static uint8_t sensor_fresh(const lg_ctl_sensor_t *s, uint32_t now_ms) {
    return (uint8_t)(s->seen && !lg_elapsed_ge(now_ms, s->sample_ms, LG_SAMPLE_MAX_AGE_MS + 1u));
}

static uint8_t sensor_verdict(const lg_ctl_t *c, lg_ctl_sensor_t *s, uint32_t now_ms) {
    /* Time spent without a fresh sample: one stalled tick is short, a long IPC delay is not. */
    if (sensor_fresh(s, now_ms)) {
        s->stale_tracking = 0;
    } else if (!s->stale_tracking) {
        s->stale_tracking = 1;
        s->stale_since_ms = now_ms;
    }
    /*
     * The two failures are LATCHED when first seen and cleared only by a new sample: the 32-bit
     * millisecond arithmetic reads a timestamp older than 2^31 ms (24 days) as "in the future", and a
     * sensor that has been dead that long must not look alive again.
     */
    if (s->seen && lg_elapsed_ge(now_ms, s->rx_ms, c->cfg.sensor_silence_ms)) {
        s->silent = 1;
    }
    if (s->stale_tracking && lg_elapsed_ge(now_ms, s->stale_since_ms, c->cfg.sensor_silence_ms)) {
        s->stale_failed = 1;
    }
    /*
     * FAILED when nothing was ever received, nothing for sensor_silence_ms, the sensor is blind, or
     * its samples have been stale for sensor_silence_ms in a row (they arrive, but are never usable).
     * A single stale tick only makes the sensor unusable for that tick.
     */
    if (!s->seen || s->silent || s->stale_failed || s->quality < LG_SENSOR_QUALITY_USABLE_MIN) {
        return LG_VERDICT_FAILED;
    }
    return LG_VERDICT_OK;
}

static lg_vsensor_t to_vsensor(const lg_ctl_sensor_t *s, uint8_t verdict, uint32_t now_ms) {
    lg_vsensor_t v;

    v.verdict = verdict;
    v.fresh = sensor_fresh(s, now_ms);
    v.quality = s->quality;
    v.occupied = s->occupied;
    return v;
}

lg_ctl_out_t lg_ctl_step(lg_ctl_t *c, uint32_t now_ms) {
    lg_ctl_out_t out;
    lg_mode_in_t in;
    lg_mode_result_t mr;
    lg_vsensor_t vs[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    lg_vote_t vote[LG_DIR_COUNT];
    uint8_t gate_silent;
    uint8_t gate_unheard;
    uint8_t gate_state;
    uint8_t gate_down;
    uint8_t any_active = 0;
    uint8_t island_occupied = 0;
    uint8_t hold_down;
    uint8_t lower_due;
    uint8_t want_down;
    uint8_t counting_ok[LG_DIR_COUNT];
    uint16_t axles_in[LG_DIR_COUNT];
    uint16_t axles_out[LG_DIR_COUNT];
    uint8_t transit[LG_DIR_COUNT];
    int d;
    int i;

    memset(&out, 0, sizeof out);
    if (!c->started) {
        c->started = 1;
        c->first_step_ms = now_ms; /* the process is set up by now: the gate clocks start here */
    }
    c->ticks++;

    /* Interim health: sensor verdicts from the samples, gate verdict from its replies. */
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            lg_ctl_sensor_t *s = &c->sensor[d][i];

            vs[d][i] = to_vsensor(s, sensor_verdict(c, s, now_ms), now_ms);
        }
    }
    if (c->gate_seen) {
        gate_unheard = 0;
        gate_silent = (uint8_t)lg_elapsed_ge(now_ms, c->gate_rx_ms, c->cfg.gate_silence_ms);
    } else {
        /* Never heard: a WARNING during the grace period, silent (hard) after it. */
        gate_silent = (uint8_t)lg_elapsed_ge(now_ms, c->first_step_ms, c->cfg.gate_boot_grace_ms);
        gate_unheard = (uint8_t)!gate_silent;
    }
    c->gate_silent = gate_silent;
    /* A gate that stays silent is not known to be UP: treat it as FAULT (never as UP). */
    gate_state = gate_silent ? (uint8_t)LG_GATE_FAULT : c->gate_state;
    gate_down = (uint8_t)(gate_state == LG_GATE_DOWN);
    if (gate_down && !c->gate_down_tracking) {
        c->gate_down_since_ms = now_ms;
    }
    c->gate_down_tracking = gate_down;

    /* Level. */
    lg_mode_in_all_healthy(&in);
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            in.verdict[d * LG_SENSOR_ID_COUNT + i] = vs[d][i].verdict;
        }
        in.schedule_ok[d] = 0; /* no lg-schedule before Step 12 */
    }
    if (gate_silent || gate_state == LG_GATE_FAULT) {
        in.verdict[LG_COMP_GATE] = LG_VERDICT_FAILED;
    } else if (gate_unheard) {
        in.verdict[LG_COMP_GATE] = LG_VERDICT_WARNING;
    } else {
        in.verdict[LG_COMP_GATE] = LG_VERDICT_OK;
    }
    in.self_check_ok = (uint8_t)!gate_silent;
    in.guard_ack = c->guard_ack_pending;
    c->guard_ack_pending = 0;
    mr = lg_mode_step(&c->mode, &c->cfg.mode, &in, now_ms);
    out.level = c->mode.level;
    out.level_changed = mr.changed;
    out.level_from = mr.from;
    out.level_reason = mr.reason;

    /* Voting and activation. */
    for (d = 0; d < LG_DIR_COUNT; d++) {
        lg_vdir_t vd;
        const lg_ctl_sensor_t *sa = &c->sensor[d][LG_SENSOR_A];
        const lg_ctl_sensor_t *sb = &c->sensor[d][LG_SENSOR_B];
        int a_ok = lg_vsensor_usable(&vs[d][LG_SENSOR_A]);
        int b_ok = lg_vsensor_usable(&vs[d][LG_SENSOR_B]);

        vd.a = vs[d][LG_SENSOR_A];
        vd.b = vs[d][LG_SENSOR_B];
        vote[d] = lg_vote_direction(&vd);
        /*
         * Axle counts are cumulative and wrap modulo 65536: a train is between the approach sensor
         * and the island exit while they differ (also after a controller restart).
         */
        counting_ok[d] = (uint8_t)((a_ok || b_ok) && lg_vsensor_usable(&vs[d][LG_SENSOR_ISLAND]));
        axles_in[d] = a_ok ? sa->axles : sb->axles;
        axles_out[d] = c->sensor[d][LG_SENSOR_ISLAND].axles;
        transit[d] = (uint8_t)(counting_ok[d] && (uint16_t)(axles_in[d] - axles_out[d]) != 0);
        if (vote[d].approaching) {
            c->last_approach_ms[d] = now_ms;
        }
        if (vote[d].approaching || transit[d]) {
            if (!c->active[d]) {
                if (!c->was_active) {
                    c->active_since_ms = now_ms;
                    c->was_active = 1;
                }
                c->active[d] = 1;
                lg_clear_reset(&c->clear[d]);
                c->activations++;
                out.activated_mask |= (uint8_t)(1u << d);
            }
        }
    }
    for (d = 0; d < LG_DIR_COUNT; d++) {
        if (lg_vsensor_usable(&vs[d][LG_SENSOR_ISLAND]) && vs[d][LG_SENSOR_ISLAND].occupied) {
            island_occupied = 1;
        }
    }

    /* Clearing. */
    for (d = 0; d < LG_DIR_COUNT; d++) {
        lg_clear_in_t ci;
        uint32_t unmet;

        if (!c->active[d]) {
            continue;
        }
        memset(&ci, 0, sizeof ci);
        ci.now_ms = now_ms;
        ci.level = c->mode.level;
        ci.island = vs[d][LG_SENSOR_ISLAND];
        ci.axle_counting_healthy = counting_ok[d];
        ci.axles_in = axles_in[d];
        ci.axles_out = axles_out[d];
        ci.other_dir_approaching = vote[1 - d].approaching;
        ci.gate_down = gate_down;
        ci.gate_down_since_ms = c->gate_down_since_ms;
        ci.guard_confirm = 0; /* guard confirmation arrives with lgctl (Step 8, authenticated in 13) */
        unmet = lg_clear_step(&c->clear[d], &c->cfg.clear, &ci);
        if (vote[d].approaching ||
            !lg_elapsed_ge(now_ms, c->last_approach_ms[d], c->cfg.clear.t_clear_ms)) {
            unmet |= LG_CTL_UNMET_SAME_DIR;
        }
        c->last_unmet[d] = unmet;
        if (unmet == 0) {
            c->active[d] = 0;
            out.cleared_mask |= (uint8_t)(1u << d);
        }
    }
    for (d = 0; d < LG_DIR_COUNT; d++) {
        any_active = (uint8_t)(any_active || c->active[d]);
    }
    if (!any_active && c->was_active) {
        c->was_active = 0;
        c->cycles++;
    }

    /* Gate, lights and bell. */
    hold_down = (uint8_t)(c->mode.level >= LG_LEVEL_UNCERTAIN);
    lower_due = (uint8_t)(any_active &&
                          (gate_state != LG_GATE_UP ||
                           lg_elapsed_ge(now_ms, c->active_since_ms,
                                         lg_lower_delay_ms(&c->cfg.vote, c->mode.level))));
    want_down = (uint8_t)(hold_down || lower_due || island_occupied);
    out.gate_target = want_down ? LG_GATE_TARGET_DOWN : LG_GATE_TARGET_UP;
    out.lights = (uint8_t)(want_down || any_active || gate_state != LG_GATE_UP);
    out.bell = out.lights;
    c->last_gate_target = out.gate_target;
    c->last_lights = out.lights;
    c->last_bell = out.bell;
    return out;
}

size_t lg_ctl_text(const lg_ctl_t *c, char *buf, size_t len) {
    int n;

    if (buf == NULL || len == 0) {
        return 0;
    }
    n = snprintf(buf, len,
                 "level=%u boot_lock=%u gate=%s gate_silent=%u target=%s lights=%u bell=%u active=%u,%u "
                 "activations=%u cycles=%u ticks=%u late_ticks=%u samples=%u rejected=%u "
                 "gate_replies=%u gate_timeouts=%u\n",
                 (unsigned)c->mode.level, (unsigned)c->mode.boot_lock,
                 lg_gate_state_name(c->gate_state), (unsigned)c->gate_silent,
                 c->last_gate_target == LG_GATE_TARGET_UP ? "UP" : "DOWN", (unsigned)c->last_lights,
                 (unsigned)c->last_bell, (unsigned)c->active[0], (unsigned)c->active[1],
                 (unsigned)c->activations, (unsigned)c->cycles, (unsigned)c->ticks,
                 (unsigned)c->late_ticks, (unsigned)c->samples, (unsigned)c->samples_rejected,
                 (unsigned)c->gate_replies, (unsigned)c->gate_timeouts);
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    if ((size_t)n >= len) {
        buf[len - 1] = '\0';
        return len - 1;
    }
    return (size_t)n;
}
