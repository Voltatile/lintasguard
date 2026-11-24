#include "lg_controller_logic.h"

#include <stdio.h>
#include <string.h>

#include "lg_gate_model.h"
#include "lg_health.h"
#include "lg_time.h"

lg_ctl_cfg_t lg_ctl_cfg_default(void) {
    lg_ctl_cfg_t c;

    c.vote = lg_vote_cfg_default();
    c.clear = lg_clear_cfg_default();
    c.mode = lg_mode_cfg_default();
    c.sensor_silence_ms = LG_HEARTBEAT_TIMEOUT_MS;
    c.gate_silence_ms = LG_GATE_WATCHDOG_MS;
    c.gate_boot_grace_ms = LG_CTL_GATE_BOOT_GRACE_MS;
    c.sup_silence_ms = LG_CTL_SUP_SILENCE_MS;
    c.max_occupied_ms = LG_SENSOR_MAX_OCCUPIED_MS;
    c.t_disagree_ms = LG_T_DISAGREE_MS;
    c.stall_ms = LG_CTL_STALL_MS;
    c.health_silence_ms = LG_CTL_HEALTH_SILENCE_MS;
    c.health_required = 0;
    c.restarted = 0;
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
        int i;

        lg_clear_init(&c->clear[d]);
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            c->last_verdict[d][i] = LG_VERDICT_FAILED; /* nothing received yet */
        }
    }
    for (d = 0; d < LG_COMP_COUNT; d++) {
        c->comp_verdict[d] = LG_VERDICT_FAILED; /* nothing known yet */
        c->health_trend_s[d] = -1;
    }
    if (cfg->restarted) {
        lg_mode_init_restarted(&c->mode, now_ms);
    } else {
        lg_mode_init(&c->mode, now_ms);
    }
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
    if (!lg_elapsed_ge(now_ms, sample_ms, LG_SAMPLE_MAX_AGE_MS + 1u) && quality >= LG_SENSOR_QUALITY_USABLE_MIN) {
        s->unusable_failed = 0; /* only a usable (fresh, not blind) sample clears the failure */
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

int lg_ctl_on_health_verdict(lg_ctl_t *c, uint8_t comp, uint8_t score, uint8_t verdict, uint16_t diag,
                             int32_t trend_s, uint32_t now_ms) {
    return lg_ctl_on_health_verdict_flags(c, comp, score, verdict, diag, trend_s, LG_HV_ASSESSED, now_ms);
}

int lg_ctl_on_health_verdict_flags(lg_ctl_t *c, uint8_t comp, uint8_t score, uint8_t verdict, uint16_t diag,
                                   int32_t trend_s, uint8_t flags, uint32_t now_ms) {
    if (comp >= LG_COMP_COUNT || score > 100) {
        c->health_rejected++;
        return -1;
    }
    if (verdict != LG_VERDICT_OK && verdict != LG_VERDICT_WARNING) {
        verdict = LG_VERDICT_FAILED; /* FAILED or unknown: fails closed */
    }
    if ((flags & LG_HV_ASSESSED) == 0 && verdict == LG_VERDICT_OK && c->health_seen[comp] &&
        c->health_verdict[comp] != LG_VERDICT_OK) {
        /* Not looked at yet (a restarted lg-health): keep the earlier verdict, count the sign of life. */
        c->health_armed = 1;
        c->health_silent = 0;
        c->health_rx_ms = now_ms;
        c->health_rx++;
        c->health_unassessed++;
        return 0;
    }
    c->health_seen[comp] = 1;
    c->health_verdict[comp] = verdict;
    c->health_score[comp] = score;
    c->health_diag[comp] = diag;
    c->health_trend_s[comp] = trend_s < -1 ? -1 : trend_s;
    c->health_armed = 1;
    c->health_silent = 0;
    c->health_rx_ms = now_ms;
    c->health_rx++;
    return 0;
}

void lg_ctl_on_guard_ack(lg_ctl_t *c) {
    c->guard_ack_pending = 1;
    c->guard_acks++;
}

static void stuck_new_process(lg_ctl_sensor_t *s);

int lg_ctl_on_proc_status(lg_ctl_t *c, uint8_t proc, uint8_t state, uint8_t restarts, uint32_t now_ms) {
    if (proc >= LG_PROC_COUNT || proc == LG_PROC_SUPERVISOR || proc == LG_PROC_CONTROLLER) {
        c->proc_status_rejected++;
        return -1;
    }
    if (state > LG_PROC_DEAD) {
        state = LG_PROC_DEAD;
        c->proc_status_unknown++;
    }
    if (state != LG_PROC_RUNNING && proc >= LG_PROC_SENSOR_BASE &&
        proc < LG_PROC_SENSOR_BASE + LG_DIR_COUNT * LG_SENSOR_ID_COUNT) {
        /* A new process (or none) for this sensor: a latched stuck diagnosis belonged to the old one. */
        stuck_new_process(&c->sensor[(proc - LG_PROC_SENSOR_BASE) / LG_SENSOR_ID_COUNT]
                                    [(proc - LG_PROC_SENSOR_BASE) % LG_SENSOR_ID_COUNT]);
    }
    c->proc_state[proc] = state;
    c->proc_restarts[proc] = restarts;
    c->sup_armed = 1;
    c->sup_silent = 0;
    c->sup_rx_ms = now_ms;
    c->proc_status_rx++;
    return 0;
}

/* Sample younger than LG_SAMPLE_MAX_AGE_MS (by the sensor's own header time). */
static uint8_t sensor_fresh(const lg_ctl_sensor_t *s, uint32_t now_ms) {
    return (uint8_t)(s->seen && !lg_elapsed_ge(now_ms, s->sample_ms, LG_SAMPLE_MAX_AGE_MS + 1u));
}

static uint8_t sensor_verdict(const lg_ctl_t *c, lg_ctl_sensor_t *s, uint32_t now_ms) {
    /*
     * Time spent without a usable sample (stale, or blind with quality 0; Step 9: one clock for both,
     * so a sensor that alternates stale and blind is FAILED too). One stalled or blind tick is short,
     * a long IPC delay or a lost plant link is not.
     */
    if (sensor_fresh(s, now_ms) && s->quality >= LG_SENSOR_QUALITY_USABLE_MIN) {
        s->unusable_tracking = 0;
    } else if (!s->unusable_tracking) {
        s->unusable_tracking = 1;
        s->unusable_since_ms = now_ms;
    }
    /*
     * The two failures are LATCHED when first seen and cleared only by a new (usable) sample: the
     * 32-bit millisecond arithmetic reads a timestamp older than 2^31 ms (24 days) as "in the future",
     * and a sensor that has been dead or blind that long must not look alive again.
     */
    if (s->seen && lg_elapsed_ge(now_ms, s->rx_ms, c->cfg.sensor_silence_ms)) {
        s->silent = 1;
    }
    if (s->unusable_tracking && lg_elapsed_ge(now_ms, s->unusable_since_ms, c->cfg.sensor_silence_ms)) {
        s->unusable_failed = 1;
    }
    /*
     * FAILED when nothing was ever received, nothing for sensor_silence_ms, or no usable sample for
     * sensor_silence_ms in a row (samples arrive, but stale or blind). A single stale or blind tick
     * only makes the sensor unusable for that tick (lg_vsensor_usable needs a fresh sample with
     * quality >= 1), so a blind sensor never counts as a clear track.
     */
    if (!s->seen || s->silent || s->unusable_failed) {
        return LG_VERDICT_FAILED;
    }
    return LG_VERDICT_OK;
}

/* |a - b| for two timestamps less than 2^31 ms apart (32-bit wrap safe). */
static uint32_t ms_dist(uint32_t a, uint32_t b) {
    uint32_t d = a - b;

    return d < 0x80000000u ? d : (uint32_t)(b - a);
}

static void stuck_forget_runs(lg_ctl_sensor_t *s) {
    s->run_done = 0;
    s->saw = 0;
}

/*
 * A sensor that is not alive (silent, stale, blind) forgets its runs but KEEPS a latched diagnosis: a
 * silence (a VM stall) does not prove the fault is gone (Step 9 review). Only an agreeing run or a new
 * process (stuck_new_process) releases it.
 */
static void stuck_forget(lg_ctl_sensor_t *s) {
    s->run_open = 0;
    s->run_done = 0;
    s->saw = 0;
}

/* The supervisor started a new process for the sensor: the old fault went with the old process. */
static void stuck_new_process(lg_ctl_sensor_t *s) {
    stuck_forget(s);
    s->stuck = LG_CTL_DIAG_NONE;
}

/* Rise and fall edges of the occupied run. Returns 1 on a rise. */
static int stuck_edges(lg_ctl_sensor_t *s, uint32_t now_ms) {
    if (s->occupied && !s->run_open) {
        s->run_open = 1;
        s->run_done = 0; /* a run still waiting to be judged is dropped (flicker) */
        s->rise_ms = now_ms;
        return 1;
    }
    if (!s->occupied && s->run_open) {
        s->run_open = 0;
        s->run_done = 1;
        s->fall_ms = now_ms;
    }
    return 0;
}

/* The completed run of y judges x, t_disagree after y fell. */
static void stuck_judge(const lg_ctl_cfg_t *cfg, lg_ctl_sensor_t *x, lg_ctl_sensor_t *y, uint32_t now_ms) {
    if (!y->run_done || !lg_elapsed_ge(now_ms, y->fall_ms, cfg->t_disagree_ms)) {
        return;
    }
    y->run_done = 0;
    if ((uint32_t)(y->fall_ms - y->rise_ms) > cfg->max_occupied_ms) {
        return; /* far longer than a train: y itself was stuck, its run proves nothing */
    }
    if ((uint32_t)(y->fall_ms - y->rise_ms) < cfg->t_disagree_ms) {
        return; /* a blip: no train passes the sensor zone that fast, nobody is blamed or released */
    }
    if (x->run_open) {
        return; /* x is still occupied: it did see something, nothing to blame or to clear */
    }
    if (!x->saw) {
        if (x->stuck == LG_CTL_DIAG_NONE) {
            x->stuck = LG_CTL_DIAG_MISSED_TRAIN;
        }
        return;
    }
    if (x->run_done && (uint32_t)(x->fall_ms - x->rise_ms) <= cfg->max_occupied_ms &&
        ms_dist(x->rise_ms, y->rise_ms) <= cfg->t_disagree_ms &&
        ms_dist(x->fall_ms, y->fall_ms) <= cfg->t_disagree_ms) {
        /* One train seen alike by both: whatever was latched is released. */
        x->run_done = 0;
        x->stuck = LG_CTL_DIAG_NONE;
        y->stuck = LG_CTL_DIAG_NONE;
    }
}

static void stuck_occupied_check(const lg_ctl_cfg_t *cfg, lg_ctl_sensor_t *x, const lg_ctl_sensor_t *y,
                                 uint32_t now_ms) {
    if (x->run_open && !y->run_open && x->stuck == LG_CTL_DIAG_NONE &&
        lg_elapsed_ge(now_ms, x->rise_ms, cfg->max_occupied_ms)) {
        x->stuck = LG_CTL_DIAG_STUCK_OCCUPIED;
    }
}

/*
 * Interim stuck rules for the approach pair of one direction (see the header). alive_a / alive_b:
 * the base verdict of the sensor is OK. Runs are judged only while both are alive.
 */
static void stuck_step(const lg_ctl_cfg_t *cfg, lg_ctl_sensor_t *a, lg_ctl_sensor_t *b, int alive_a,
                       int alive_b, uint32_t now_ms) {
    int rose_a;
    int rose_b;

    if (!alive_a) {
        stuck_forget(a);
    }
    if (!alive_b) {
        stuck_forget(b);
    }
    rose_a = alive_a ? stuck_edges(a, now_ms) : 0;
    rose_b = alive_b ? stuck_edges(b, now_ms) : 0;
    if (!alive_a || !alive_b) {
        /* No partner to compare with: nothing seen in this time may be judged later. */
        stuck_forget_runs(a);
        stuck_forget_runs(b);
        return;
    }
    if (rose_a) {
        b->saw = 0; /* a new run of a: does b see it too? */
    }
    if (rose_b) {
        a->saw = 0;
    }
    if (a->occupied && (b->run_open || b->run_done)) {
        a->saw = 1;
    }
    if (b->occupied && (a->run_open || a->run_done)) {
        b->saw = 1;
    }
    stuck_judge(cfg, b, a, now_ms);
    stuck_judge(cfg, a, b, now_ms);
    stuck_occupied_check(cfg, a, b, now_ms);
    stuck_occupied_check(cfg, b, a, now_ms);
}

static void ev_push(lg_ctl_t *c, uint32_t now_ms, uint8_t kind, uint8_t sev, uint8_t from, uint8_t to,
                    uint8_t dir, uint8_t id, uint8_t code) {
    lg_ctl_event_t *e;

    c->events++;
    e = &c->ev[(c->events - 1u) % LG_CTL_EVENT_RING];
    memset(e, 0, sizeof *e);
    e->seq = c->events;
    e->t_ms = now_ms;
    e->kind = kind;
    e->severity = sev;
    e->level_from = from;
    e->level_to = to;
    e->dir = dir;
    e->id = id;
    e->code = code;
    e->comp = (dir < LG_DIR_COUNT && id < LG_SENSOR_ID_COUNT) ? (uint8_t)(dir * LG_SENSOR_ID_COUNT + id)
                                                              : (uint8_t)LG_CTL_EVENT_NO_SENSOR;
    e->value = kind == LG_EVT_NOTIFY ? -1 : 0; /* NOTIFY: no trend known (-1), not "0 s to failure" */
}

/* An event about a component (sensor, gate or battery) with a value. */
static void ev_push_comp(lg_ctl_t *c, uint32_t now_ms, uint8_t kind, uint8_t sev, uint8_t comp, uint8_t code,
                         int32_t value) {
    uint8_t dir = LG_CTL_EVENT_NO_SENSOR;
    uint8_t id = LG_CTL_EVENT_NO_SENSOR;
    lg_ctl_event_t *e;

    if (comp <= LG_COMP_SENSOR_LAST) {
        dir = (uint8_t)(comp / LG_SENSOR_ID_COUNT);
        id = (uint8_t)(comp % LG_SENSOR_ID_COUNT);
    }
    ev_push(c, now_ms, kind, sev, c->mode.level, c->mode.level, dir, id, code);
    e = &c->ev[(c->events - 1u) % LG_CTL_EVENT_RING];
    e->comp = comp;
    e->value = value;
}

/* The diagnosis code of the lg-health verdict of a component, as an lg_ctl_diag_t. */
static uint8_t health_cause(const lg_ctl_t *c, uint8_t comp) {
    uint16_t d = c->health_diag[comp];

    if (d >= LG_DIAG_COUNT) {
        d = LG_DIAG_NONE;
    }
    return (uint8_t)(LG_CTL_DIAG_HEALTH_BASE + d);
}

/* The worse of the controller's own verdict and the lg-health verdict of a component. */
static uint8_t merge_health(const lg_ctl_t *c, uint8_t comp, uint8_t own) {
    uint8_t hv;

    if (!c->health_armed || !c->health_seen[comp]) {
        return own;
    }
    hv = c->health_verdict[comp];
    if (c->health_silent && hv != LG_VERDICT_FAILED) {
        hv = LG_VERDICT_OK; /* a silent monitor's WARNING lapses; its FAILED stays (silence is not a repair) */
    }
    return hv > own ? hv : own;
}

/*
 * Early-warning episodes (Step 11), for every component, on the effective verdicts:
 *  - OK -> WARNING (the component was OK once after the boot release): WARNING notification with diagnosis and trend; the
 *    episode starts.
 *  - -> FAILED: a running episode ends with the METRIC lead_time_ms; the gate and the battery raise an
 *    URGENT ticket (the sensors get theirs from sensor_tickets).
 *  - -> OK: an INFO event when a WARNING episode or a gate/battery ticket was open.
 * gate_own_cause: the cause the controller's own rules give for a gate that is FAILED.
 */
static void health_events(lg_ctl_t *c, const uint8_t eff[LG_COMP_COUNT], uint8_t gate_own_cause,
                          uint32_t now_ms) {
    uint8_t k;

    for (k = 0; k < LG_COMP_COUNT; k++) {
        uint8_t v = eff[k];
        uint8_t prev = c->comp_verdict[k];
        uint8_t from_health = (uint8_t)(c->health_armed && c->health_seen[k] && c->health_verdict[k] == v);
        uint8_t code = from_health ? health_cause(c, k)
                                   : (k == LG_COMP_GATE ? gate_own_cause : (uint8_t)LG_CTL_DIAG_NONE);

        if (c->health_silent && v == LG_VERDICT_OK && prev != LG_VERDICT_OK && c->comp_from_health[k]) {
            /* OK only because lg-health went silent: not a repair. Keep the episode and the ticket
             * (no false INFO now, no second ticket when lg-health comes back; found by the tester). */
            continue;
        }
        if (v == LG_VERDICT_OK) {
            if (prev != LG_VERDICT_OK && c->comp_ever_ok[k] && (c->warn_open[k] || c->comp_ticket[k])) {
                ev_push_comp(c, now_ms, LG_EVT_NOTIFY, LG_SEV_INFO, k, LG_CTL_DIAG_NONE, -1);
            }
            c->warn_open[k] = 0;
            c->comp_ticket[k] = 0;
            /* "Was fine once" counts only after the boot release: a start-up transient (blind samples
             * until the plant answers) is no maintenance case, level 4 holds the gate down anyway. */
            if (!c->mode.boot_lock) {
                c->comp_ever_ok[k] = 1;
            }
        } else if (v == LG_VERDICT_WARNING) {
            if (prev == LG_VERDICT_OK && c->comp_ever_ok[k] && !c->warn_open[k]) {
                c->warn_open[k] = 1;
                c->warn_since_ms[k] = now_ms;
                c->warnings++;
                ev_push_comp(c, now_ms, LG_EVT_NOTIFY, LG_SEV_WARNING, k, code,
                             from_health ? c->health_trend_s[k] : -1);
            }
        } else if (prev != LG_VERDICT_FAILED) {
            /* The ticket first, then the lead time (the same order as for a sensor). */
            if (k > LG_COMP_SENSOR_LAST && c->comp_ever_ok[k] && !c->comp_ticket[k]) {
                c->comp_ticket[k] = 1;
                c->tickets++;
                ev_push_comp(c, now_ms, LG_EVT_NOTIFY, LG_SEV_URGENT, k, code, -1);
            }
            if (c->warn_open[k]) {
                uint32_t lead = now_ms - c->warn_since_ms[k];

                c->warn_open[k] = 0;
                c->lead_times++;
                c->last_lead_time_ms = lead;
                ev_push_comp(c, now_ms, LG_EVT_METRIC, LG_SEV_INFO, k, LG_CTL_METRIC_LEAD_TIME_MS,
                             lead > 0x7FFFFFFFu ? 0x7FFFFFFF : (int32_t)lead);
            }
        }
        c->comp_verdict[k] = v;
        c->comp_from_health[k] = (uint8_t)(v != LG_VERDICT_OK && from_health);
    }
}

/* Why a FAILED sensor is FAILED (the first cause that applies). */
static uint8_t sensor_cause(const lg_ctl_sensor_t *s, uint8_t id) {
    if (!s->seen || s->silent) {
        return LG_CTL_DIAG_SILENT;
    }
    if (s->unusable_failed) {
        return s->quality < LG_SENSOR_QUALITY_USABLE_MIN ? (uint8_t)LG_CTL_DIAG_BLIND : (uint8_t)LG_CTL_DIAG_STALE;
    }
    return id != LG_SENSOR_ISLAND ? s->stuck : (uint8_t)LG_CTL_DIAG_NONE;
}

/*
 * Maintenance tickets: a sensor that was fine once and then goes bad raises an URGENT ticket; when it
 * is fine again an INFO event closes it. "Bad" is a FAILED verdict or a sensor process that the
 * supervisor reports RESTARTING or DEAD (a fast restart never looks silent, README 3 still wants the
 * ticket). An open ticket escalates once with a new ticket when the process is given up (DEAD). A
 * sensor that never was fine (boot) raises nothing: the boot lock already keeps level 4.
 */
static void sensor_tickets(lg_ctl_t *c, const lg_vsensor_t vs[LG_DIR_COUNT][LG_SENSOR_ID_COUNT],
                           uint32_t now_ms) {
    int d;
    int i;

    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            uint8_t v = vs[d][i].verdict;
            uint8_t proc = c->proc_state[LG_PROC_SENSOR_BASE + d * LG_SENSOR_ID_COUNT + i];
            uint8_t cause;

            if (proc == LG_PROC_DEAD) {
                cause = LG_CTL_DIAG_PROC_DEAD;
            } else if (proc != LG_PROC_RUNNING) {
                cause = LG_CTL_DIAG_RESTARTING;
            } else if (v == LG_VERDICT_FAILED) {
                cause = sensor_cause(&c->sensor[d][i], (uint8_t)i);
                if (cause == LG_CTL_DIAG_NONE) {
                    cause = health_cause(c, (uint8_t)(d * LG_SENSOR_ID_COUNT + i)); /* FAILED by lg-health */
                }
            } else {
                cause = LG_CTL_DIAG_NONE;
            }
            if (cause == LG_CTL_DIAG_NONE && v == LG_VERDICT_OK && c->health_silent && c->ticket_open[d][i] &&
                c->ticket_code[d][i] >= LG_CTL_DIAG_HEALTH_BASE) {
                /* OK only because lg-health went silent: not a repair, the ticket stays open (tester). */
                c->last_verdict[d][i] = v;
                continue;
            }
            if (cause == LG_CTL_DIAG_NONE && v == LG_VERDICT_OK) {
                if (c->ticket_open[d][i]) {
                    c->ticket_open[d][i] = 0;
                    ev_push(c, now_ms, LG_EVT_NOTIFY, LG_SEV_INFO, c->mode.level, c->mode.level,
                            (uint8_t)d, (uint8_t)i, LG_CTL_DIAG_NONE);
                }
                c->ever_ok[d][i] = 1;
            } else if (cause != LG_CTL_DIAG_NONE && c->ever_ok[d][i] &&
                       (!c->ticket_open[d][i] ||
                        (cause == LG_CTL_DIAG_PROC_DEAD && c->ticket_code[d][i] != LG_CTL_DIAG_PROC_DEAD))) {
                c->ticket_open[d][i] = 1;
                c->ticket_code[d][i] = cause;
                c->tickets++;
                ev_push(c, now_ms, LG_EVT_NOTIFY, LG_SEV_URGENT, c->mode.level, c->mode.level, (uint8_t)d,
                        (uint8_t)i, cause);
            }
            c->last_verdict[d][i] = v;
        }
    }
}

/*
 * The controller itself stalled (a step came stall_ms or more after the previous one, typically the
 * whole VM paused): little was received in that time because nothing ran, not because the peers went
 * quiet, and nothing was observed either. Every clock that measures "how long since" moves forward by
 * gap_ms = the delay minus stall_ms, so the stall counts as exactly stall_ms:
 *  - silence clocks (sensor receive time and unusable start, gate reply, supervisor status): a peer
 *    that stays quiet after the stall is still FAILED, silence before the stall still counts, and a
 *    controller that is late on EVERY step still adds stall_ms of silence per step;
 *  - "healthy/clear for so long" clocks (the mode manager's stability and boot-release timer, the
 *    island clear window, the gate-down time, the last approach): a stall must not count as observed
 *    health. Without this, level 4 was left and the gate commanded UP at the first step after a long
 *    stall, before any new input (found by the reviewer, Step 9; test_stall_is_not_stable_time).
 * Only clocks at or before the previous step are moved: a reply or sample that arrived after the
 * resume (handled before this step) is fresh and must not be pushed into the future. Sample
 * timestamps are never moved: an old sample stays stale and unusable.
 */
static void shift_old(uint32_t *x, uint32_t last_step_ms, uint32_t gap_ms) {
    if (!lg_elapsed_ge(*x, last_step_ms, 1u)) { /* *x <= last_step_ms (32-bit wrap safe) */
        *x += gap_ms;
    }
}

static void stall_shift(lg_ctl_t *c, uint32_t gap_ms) {
    uint32_t last = c->last_step_ms;
    int d;
    int i;

    c->stalls++;
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            lg_ctl_sensor_t *s = &c->sensor[d][i];

            if (s->seen) {
                shift_old(&s->rx_ms, last, gap_ms);
            }
            if (s->unusable_tracking) {
                shift_old(&s->unusable_since_ms, last, gap_ms);
            }
        }
        if (c->clear[d].island_clear_tracking) {
            shift_old(&c->clear[d].island_clear_since_ms, last, gap_ms);
        }
        shift_old(&c->last_approach_ms[d], last, gap_ms);
    }
    if (c->gate_seen) {
        shift_old(&c->gate_rx_ms, last, gap_ms);
    } else {
        shift_old(&c->first_step_ms, last, gap_ms); /* the boot grace of a gate never heard */
    }
    if (c->gate_down_tracking) {
        shift_old(&c->gate_down_since_ms, last, gap_ms);
    }
    if (c->sup_armed) {
        shift_old(&c->sup_rx_ms, last, gap_ms);
    }
    if (c->health_armed) {
        shift_old(&c->health_rx_ms, last, gap_ms);
    }
    if (c->mode.timing) {
        shift_old(&c->mode.since_ms, last, gap_ms);
    }
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
    } else if (lg_elapsed_ge(now_ms, c->last_step_ms, c->cfg.stall_ms)) {
        /* A late step counts as exactly stall_ms of silence: see stall_shift. */
        stall_shift(c, (now_ms - c->last_step_ms) - c->cfg.stall_ms);
    }
    c->last_step_ms = now_ms;
    c->ticks++;

    /* lg-health silent after it was heard: its verdicts no longer count (latched until the next one). */
    if (c->health_armed && lg_elapsed_ge(now_ms, c->health_rx_ms, c->cfg.health_silence_ms)) {
        c->health_silent = 1;
    }

    /*
     * Interim health: sensor verdicts from the samples (silence, staleness, blindness), then the stuck
     * rules on the approach pairs, gate verdict from its replies.
     */
    for (d = 0; d < LG_DIR_COUNT; d++) {
        uint8_t base[LG_SENSOR_ID_COUNT];

        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            base[i] = sensor_verdict(c, &c->sensor[d][i], now_ms);
        }
        /* Alive for the stuck rules: OK and not blind (a blind sample says occupied 0 without seeing). */
        /* A sensor that lg-health declared FAILED is not alive either: its phantom trains (noise) must
         * not make the healthy partner look stuck clear (test_controller_health). */
        stuck_step(&c->cfg, &c->sensor[d][LG_SENSOR_A], &c->sensor[d][LG_SENSOR_B],
                   base[LG_SENSOR_A] == LG_VERDICT_OK &&
                       c->sensor[d][LG_SENSOR_A].quality >= LG_SENSOR_QUALITY_USABLE_MIN &&
                       merge_health(c, (uint8_t)(d * LG_SENSOR_ID_COUNT + LG_SENSOR_A), LG_VERDICT_OK) !=
                           LG_VERDICT_FAILED,
                   base[LG_SENSOR_B] == LG_VERDICT_OK &&
                       c->sensor[d][LG_SENSOR_B].quality >= LG_SENSOR_QUALITY_USABLE_MIN &&
                       merge_health(c, (uint8_t)(d * LG_SENSOR_ID_COUNT + LG_SENSOR_B), LG_VERDICT_OK) !=
                           LG_VERDICT_FAILED,
                   now_ms);
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            lg_ctl_sensor_t *s = &c->sensor[d][i];
            uint8_t v = base[i];

            if (i != LG_SENSOR_ISLAND && s->stuck != LG_CTL_DIAG_NONE) {
                v = LG_VERDICT_FAILED; /* isolated: the vote ignores it */
            }
            v = merge_health(c, (uint8_t)(d * LG_SENSOR_ID_COUNT + i), v); /* FAILED: isolated as well */
            vs[d][i] = to_vsensor(s, v, now_ms);
        }
    }
    sensor_tickets(c, (const lg_vsensor_t(*)[LG_SENSOR_ID_COUNT])vs, now_ms);
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
    in.verdict[LG_COMP_GATE] = merge_health(c, LG_COMP_GATE, in.verdict[LG_COMP_GATE]);
    in.verdict[LG_COMP_BATTERY] = merge_health(c, LG_COMP_BATTERY, LG_VERDICT_OK);
    health_events(c, in.verdict, gate_silent ? (uint8_t)LG_CTL_DIAG_SILENT : (uint8_t)LG_CTL_DIAG_GATE_FAULT,
                  now_ms);
    /* Supervisor reports. The supervisor itself is judged by its silence once it has been heard. */
    if (c->sup_armed && lg_elapsed_ge(now_ms, c->sup_rx_ms, c->cfg.sup_silence_ms)) {
        c->sup_silent = 1; /* latched like the sensor silence: the 32-bit clock must not revive it */
    }
    for (i = 0; i < LG_PROC_COUNT; i++) {
        in.proc_state[i] = c->proc_state[i];
    }
    in.proc_state[LG_PROC_SUPERVISOR] = c->sup_silent ? (uint8_t)LG_PROC_DEAD : (uint8_t)LG_PROC_RUNNING;
    /* lg-health lost (silent after it was heard) or, when it is installed, not heard yet: like a health
     * process that is restarting (level 2; the boot lock stays until it reports). */
    if ((c->health_silent || (c->cfg.health_required && !c->health_armed)) &&
        in.proc_state[LG_PROC_HEALTH] == LG_PROC_RUNNING) {
        in.proc_state[LG_PROC_HEALTH] = LG_PROC_RESTARTING;
    }
    in.self_check_ok = (uint8_t)!gate_silent;
    in.guard_ack = c->guard_ack_pending;
    c->guard_ack_pending = 0;
    mr = lg_mode_step(&c->mode, &c->cfg.mode, &in, now_ms);
    if (mr.changed) {
        uint8_t sev = mr.to >= LG_LEVEL_FAILSAFE ? (uint8_t)LG_SEV_URGENT
                      : mr.to > mr.from          ? (uint8_t)LG_SEV_WARNING
                                                 : (uint8_t)LG_SEV_INFO;

        ev_push(c, now_ms, LG_EVT_LEVEL_CHANGE, sev, mr.from, mr.to, LG_CTL_EVENT_NO_SENSOR,
                LG_CTL_EVENT_NO_SENSOR, mr.reason);
    }
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

uint8_t lg_ctl_sensor_diag(const lg_ctl_t *c, uint8_t dir, uint8_t id) {
    if (dir >= LG_DIR_COUNT || id > LG_SENSOR_B) {
        return LG_CTL_DIAG_NONE;
    }
    return c->sensor[dir][id].stuck;
}

uint32_t lg_ctl_events_since(const lg_ctl_t *c, uint32_t from_seq, lg_ctl_event_t *out, uint32_t max) {
    uint32_t oldest;
    uint32_t seq;
    uint32_t n = 0;

    if (out == NULL || max == 0 || c->events == 0) {
        return 0;
    }
    oldest = c->events > LG_CTL_EVENT_RING ? c->events - LG_CTL_EVENT_RING + 1u : 1u;
    seq = from_seq > oldest ? from_seq : oldest;
    for (; seq <= c->events && n < max; seq++) {
        out[n++] = c->ev[(seq - 1u) % LG_CTL_EVENT_RING];
    }
    return n;
}

static const char *dir_name(uint8_t dir) {
    return dir == LG_DIR_WEST ? "west" : dir == LG_DIR_EAST ? "east" : "?";
}

static const char *sensor_id_name(uint8_t id) {
    return id == LG_SENSOR_A ? "a" : id == LG_SENSOR_B ? "b" : id == LG_SENSOR_ISLAND ? "island" : "?";
}

static const char *severity_name(uint8_t sev) {
    return sev == LG_SEV_INFO ? "INFO" : sev == LG_SEV_WARNING ? "WARNING" : sev == LG_SEV_URGENT ? "URGENT" : "?";
}

const char *lg_ctl_diag_name(uint8_t diag) {
    switch (diag) {
    case LG_CTL_DIAG_NONE: return "OK";
    case LG_CTL_DIAG_MISSED_TRAIN: return "MISSED_TRAIN";
    case LG_CTL_DIAG_STUCK_OCCUPIED: return "STUCK_OCCUPIED";
    case LG_CTL_DIAG_SILENT: return "SILENT";
    case LG_CTL_DIAG_STALE: return "STALE";
    case LG_CTL_DIAG_BLIND: return "BLIND";
    case LG_CTL_DIAG_RESTARTING: return "RESTARTING";
    case LG_CTL_DIAG_PROC_DEAD: return "PROC_DEAD";
    case LG_CTL_DIAG_GATE_FAULT: return "GATE_FAULT";
    default:
        if (diag >= LG_CTL_DIAG_HEALTH_BASE && diag < LG_CTL_DIAG_HEALTH_BASE + LG_DIAG_COUNT) {
            return lg_health_diag_name((uint16_t)(diag - LG_CTL_DIAG_HEALTH_BASE));
        }
        return "UNKNOWN";
    }
}

size_t lg_ctl_event_text(const lg_ctl_event_t *e, char *buf, size_t len) {
    int n;

    if (buf == NULL || len == 0) {
        return 0;
    }
    if (e == NULL) {
        buf[0] = '\0';
        return 0;
    }
    if (e->kind == LG_EVT_LEVEL_CHANGE) {
        n = snprintf(buf, len, "seq=%u t_ms=%u kind=LEVEL severity=%s from=%u to=%u reason=%u\n",
                     (unsigned)e->seq, (unsigned)e->t_ms, severity_name(e->severity),
                     (unsigned)e->level_from, (unsigned)e->level_to, (unsigned)e->code);
    } else if (e->kind == LG_EVT_NOTIFY && e->comp <= LG_COMP_SENSOR_LAST) {
        n = snprintf(buf, len, "seq=%u t_ms=%u kind=NOTIFY severity=%s to=maintenance sensor=%s/%s diag=%s "
                     "level=%u component=%s trend_s=%d\n",
                     (unsigned)e->seq, (unsigned)e->t_ms, severity_name(e->severity), dir_name(e->dir),
                     sensor_id_name(e->id), lg_ctl_diag_name(e->code), (unsigned)e->level_to,
                     lg_health_comp_name(e->comp), (int)e->value);
    } else if (e->kind == LG_EVT_NOTIFY) {
        n = snprintf(buf, len, "seq=%u t_ms=%u kind=NOTIFY severity=%s to=maintenance sensor=none diag=%s "
                     "level=%u component=%s trend_s=%d\n",
                     (unsigned)e->seq, (unsigned)e->t_ms, severity_name(e->severity), lg_ctl_diag_name(e->code),
                     (unsigned)e->level_to, lg_health_comp_name(e->comp), (int)e->value);
    } else if (e->kind == LG_EVT_METRIC) {
        n = snprintf(buf, len, "seq=%u t_ms=%u kind=METRIC name=%s component=%s value=%d\n", (unsigned)e->seq,
                     (unsigned)e->t_ms, e->code == LG_CTL_METRIC_LEAD_TIME_MS ? "lead_time_ms" : "unknown",
                     lg_health_comp_name(e->comp), (int)e->value);
    } else {
        n = snprintf(buf, len, "seq=%u t_ms=%u kind=UNKNOWN code=%u\n", (unsigned)e->seq,
                     (unsigned)e->t_ms, (unsigned)e->code);
    }
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

size_t lg_ctl_text(const lg_ctl_t *c, char *buf, size_t len) {
    int n;
    unsigned proc_bad = 0;
    unsigned sensor_failed = 0;
    unsigned comp_bad = 0;
    int i;
    int d;

    if (buf == NULL || len == 0) {
        return 0;
    }
    for (i = 0; i < LG_PROC_COUNT; i++) {
        if (c->proc_state[i] != LG_PROC_RUNNING) {
            proc_bad |= 1u << i;
        }
    }
    for (i = 0; i < LG_COMP_COUNT; i++) {
        if (c->comp_verdict[i] != LG_VERDICT_OK) {
            comp_bad |= 1u << i;
        }
    }
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            if (c->last_verdict[d][i] != LG_VERDICT_OK) {
                sensor_failed |= 1u << (d * LG_SENSOR_ID_COUNT + i);
            }
        }
    }
    n = snprintf(buf, len,
                 "level=%u boot_lock=%u gate=%s gate_silent=%u target=%s lights=%u bell=%u active=%u,%u "
                 "activations=%u cycles=%u ticks=%u late_ticks=%u samples=%u rejected=%u "
                 "gate_replies=%u gate_timeouts=%u sup_armed=%u sup_silent=%u proc_status_rx=%u "
                 "proc_status_rejected=%u proc_bad=0x%x guard_acks=%u tickets=%u sensor_failed=0x%x "
                 "events=%u unmet=0x%x,0x%x stalls=%u health_armed=%u health_silent=%u health_rx=%u "
                 "health_rejected=%u comp_bad=0x%x warnings=%u lead_times=%u last_lead_time_ms=%u health_unassessed=%u\n",
                 (unsigned)c->mode.level, (unsigned)c->mode.boot_lock,
                 lg_gate_state_name(c->gate_state), (unsigned)c->gate_silent,
                 c->last_gate_target == LG_GATE_TARGET_UP ? "UP" : "DOWN", (unsigned)c->last_lights,
                 (unsigned)c->last_bell, (unsigned)c->active[0], (unsigned)c->active[1],
                 (unsigned)c->activations, (unsigned)c->cycles, (unsigned)c->ticks,
                 (unsigned)c->late_ticks, (unsigned)c->samples, (unsigned)c->samples_rejected,
                 (unsigned)c->gate_replies, (unsigned)c->gate_timeouts, (unsigned)c->sup_armed,
                 (unsigned)c->sup_silent, (unsigned)c->proc_status_rx, (unsigned)c->proc_status_rejected,
                 proc_bad, (unsigned)c->guard_acks, (unsigned)c->tickets, sensor_failed,
                 (unsigned)c->events, (unsigned)c->last_unmet[0], (unsigned)c->last_unmet[1],
                 (unsigned)c->stalls, (unsigned)c->health_armed, (unsigned)c->health_silent,
                 (unsigned)c->health_rx, (unsigned)c->health_rejected, comp_bad, (unsigned)c->warnings,
                 (unsigned)c->lead_times, (unsigned)c->last_lead_time_ms, (unsigned)c->health_unassessed);
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
