#include "lg_mode.h"
#include "lg_time.h"

lg_mode_cfg_t lg_mode_cfg_default(void) {
    lg_mode_cfg_t c;
    c.t_stable_ms = LG_T_STABLE_MS;
    c.t_boot_stable_ms = LG_T_BOOT_STABLE_MS;
    return c;
}

/* Test helper. schedule_ok is set to 1 here; the real controller must start with 0. */
void lg_mode_in_all_healthy(lg_mode_in_t *in) {
    int i;

    for (i = 0; i < LG_COMP_COUNT; i++) {
        in->verdict[i] = LG_VERDICT_OK;
    }
    for (i = 0; i < LG_PROC_COUNT; i++) {
        in->proc_state[i] = LG_PROC_RUNNING;
    }
    for (i = 0; i < LG_DIR_COUNT; i++) {
        in->schedule_ok[i] = 1;
    }
    in->self_check_ok = 1;
    in->guard_ack = 0;
}

/* Raise the required level and remember the reason of the highest requirement. */
static void raise_level(uint8_t *level, uint8_t *reason, uint8_t to, uint8_t why) {
    if (to > *level) {
        *level = to;
        *reason = why;
    }
}

/* Inputs come from IPC messages: an unknown value is treated as the worst case. */
static uint8_t verdict_of(const lg_mode_in_t *in, int comp) {
    uint8_t v = in->verdict[comp];

    return (v == LG_VERDICT_OK || v == LG_VERDICT_WARNING) ? v : (uint8_t)LG_VERDICT_FAILED;
}

static uint8_t proc_of(const lg_mode_in_t *in, int proc) {
    uint8_t p = in->proc_state[proc];

    return (p == LG_PROC_RUNNING || p == LG_PROC_RESTARTING) ? p : (uint8_t)LG_PROC_DEAD;
}

/* A sensor is lost when its verdict is FAILED or its driver process is restarting or dead. */
static int sensor_lost(const lg_mode_in_t *in, int dir, int id) {
    int comp = LG_COMP_SENSOR_BASE + dir * LG_SENSOR_ID_COUNT + id;
    int proc = LG_PROC_SENSOR_BASE + dir * LG_SENSOR_ID_COUNT + id;

    return verdict_of(in, comp) == LG_VERDICT_FAILED || proc_of(in, proc) != LG_PROC_RUNNING;
}

uint8_t lg_mode_required(const lg_mode_in_t *in, uint8_t *reason_out) {
    uint8_t level = LG_LEVEL_NORMAL;
    uint8_t reason = LG_REASON_NONE;
    int i;
    int dir;

    if (!in->self_check_ok) {
        raise_level(&level, &reason, LG_LEVEL_FAILSAFE, LG_REASON_SELF_CHECK);
    }

    /* Critical processes: no way to keep the crossing controlled without them. */
    if (proc_of(in, LG_PROC_CONTROLLER) == LG_PROC_DEAD ||
        proc_of(in, LG_PROC_SUPERVISOR) == LG_PROC_DEAD ||
        proc_of(in, LG_PROC_GATE) == LG_PROC_DEAD) {
        raise_level(&level, &reason, LG_LEVEL_FAILSAFE, LG_REASON_CRITICAL_PROC_DEAD);
    }
    if (verdict_of(in, LG_COMP_GATE) == LG_VERDICT_FAILED) {
        raise_level(&level, &reason, LG_LEVEL_FAILSAFE, LG_REASON_GATE_FAILED);
    }
    /* A gate that is being restarted is not under control: level 4 by rule, whatever the verdict says yet. */
    if (proc_of(in, LG_PROC_GATE) == LG_PROC_RESTARTING) {
        raise_level(&level, &reason, LG_LEVEL_FAILSAFE, LG_REASON_GATE_FAILED);
    }

    /* Non-sensor processes (sensor processes are handled with their sensors below). */
    for (i = 0; i < LG_PROC_COUNT; i++) {
        int is_sensor = i >= LG_PROC_SENSOR_BASE && i < LG_PROC_PLANT;

        if (is_sensor || i == LG_PROC_CONTROLLER || i == LG_PROC_SUPERVISOR) {
            continue;
        }
        if (proc_of(in, i) == LG_PROC_DEAD) {
            raise_level(&level, &reason, i == LG_PROC_BRIDGE ? LG_LEVEL_EARLY_WARNING : LG_LEVEL_DEGRADED,
                  LG_REASON_PROC_DEAD);
        } else if (proc_of(in, i) == LG_PROC_RESTARTING) {
            raise_level(&level, &reason, i == LG_PROC_BRIDGE ? LG_LEVEL_EARLY_WARNING : LG_LEVEL_DEGRADED,
                  LG_REASON_PROC_RESTART);
        }
    }
    if (proc_of(in, LG_PROC_CONTROLLER) == LG_PROC_RESTARTING ||
        proc_of(in, LG_PROC_SUPERVISOR) == LG_PROC_RESTARTING) {
        raise_level(&level, &reason, LG_LEVEL_DEGRADED, LG_REASON_PROC_RESTART);
    }

    /* Battery: FAILED needs urgent maintenance, WARNING is an early warning. */
    if (verdict_of(in, LG_COMP_BATTERY) == LG_VERDICT_FAILED) {
        raise_level(&level, &reason, LG_LEVEL_DEGRADED, LG_REASON_WARNING);
    }

    /* Per direction: approach sensors A and B, then the island sensor. */
    for (dir = 0; dir < LG_DIR_COUNT; dir++) {
        int a_lost = sensor_lost(in, dir, LG_SENSOR_A);
        int b_lost = sensor_lost(in, dir, LG_SENSOR_B);
        int island_lost = sensor_lost(in, dir, LG_SENSOR_ISLAND);
        uint8_t loss_level = in->schedule_ok[dir] ? LG_LEVEL_UNCERTAIN : LG_LEVEL_FAILSAFE;
        uint8_t loss_reason =
            in->schedule_ok[dir] ? LG_REASON_DETECT_LOST_SCHEDULE : LG_REASON_DETECT_LOST_NO_INFO;

        if (a_lost && b_lost) {
            raise_level(&level, &reason, loss_level, loss_reason);
        } else if (a_lost || b_lost) {
            raise_level(&level, &reason, LG_LEVEL_DEGRADED, LG_REASON_SENSOR_FAILED);
        }
        /* Island lost: the gate cannot be cleared by the island, so a guard or timetable is needed. */
        if (island_lost) {
            raise_level(&level, &reason, loss_level, loss_reason);
        }
    }

    /* Any WARNING verdict is an early warning. */
    for (i = 0; i < LG_COMP_COUNT; i++) {
        if (verdict_of(in, i) == LG_VERDICT_WARNING) {
            raise_level(&level, &reason, LG_LEVEL_EARLY_WARNING, LG_REASON_WARNING);
        }
    }

    if (reason_out != 0) {
        *reason_out = reason;
    }
    return level;
}

void lg_mode_init(lg_mode_t *m, uint32_t now_ms) {
    m->level = LG_LEVEL_FAILSAFE;
    m->boot_lock = 1;
    m->guard_latched = 0;
    m->rel_kind = 0;
    m->prev_required = LG_LEVEL_FAILSAFE;
    m->timing = 0;
    m->since_ms = now_ms;
}

void lg_mode_init_restarted(lg_mode_t *m, uint32_t now_ms) {
    lg_mode_init(m, now_ms);
    m->boot_lock = 0; /* level 4 is then left only through a guard acknowledgement */
}

static lg_mode_result_t result(const lg_mode_t *m, uint8_t from, uint8_t reason) {
    lg_mode_result_t r;

    r.changed = (uint8_t)(m->level != from);
    r.from = from;
    r.to = m->level;
    r.reason = reason;
    return r;
}

lg_mode_result_t lg_mode_step(lg_mode_t *m, const lg_mode_cfg_t *cfg, const lg_mode_in_t *in,
                              uint32_t now_ms) {
    uint8_t reason = LG_REASON_NONE;
    uint8_t required = lg_mode_required(in, &reason);
    uint8_t from = m->level;

    if (m->level == LG_LEVEL_FAILSAFE) {
        int hard = required == LG_LEVEL_FAILSAFE &&
                   (reason == LG_REASON_SELF_CHECK || reason == LG_REASON_CRITICAL_PROC_DEAD ||
                    reason == LG_REASON_GATE_FAILED);
        int boot_ok;
        int release;
        uint8_t kind;
        uint32_t need;

        /* A real level 4 event during boot is no longer "not ready yet": it needs a guard. */
        if (hard) {
            m->boot_lock = 0;
            m->guard_latched = 0;
            m->timing = 0;
        }
        /* An acknowledgement counts only while the inputs demand at most level 1,
         * and is voided the moment they demand more. */
        if (required > LG_LEVEL_EARLY_WARNING) {
            m->guard_latched = 0;
        } else if (in->guard_ack) {
            m->guard_latched = 1;
        }

        /* The boot release ignores lg-bridge (no safety role, reachable from the network): a bridge that
         * cannot start must not keep the gate down until a guard comes. The release lands on the full
         * requirement, so a bridge that is down still gives level 1 after the boot (Step 10 review M3). */
        if (m->boot_lock) {
            lg_mode_in_t no_bridge = *in;

            no_bridge.proc_state[LG_PROC_BRIDGE] = LG_PROC_RUNNING;
            boot_ok = lg_mode_required(&no_bridge, 0) == LG_LEVEL_NORMAL;
        } else {
            boot_ok = 0;
        }
        release = boot_ok || m->guard_latched;
        kind = boot_ok ? 1 : 2;
        need = boot_ok ? cfg->t_boot_stable_ms : cfg->t_stable_ms;

        if (!release) {
            m->timing = 0;
            return result(m, from, m->boot_lock ? LG_REASON_BOOT : reason);
        }
        if (!m->timing || m->rel_kind != kind) {
            m->timing = 1;
            m->rel_kind = kind;
            m->since_ms = now_ms;
        }
        if (lg_elapsed_ge(now_ms, m->since_ms, need)) {
            uint8_t why = boot_ok ? LG_REASON_BOOT_RELEASED : LG_REASON_GUARD_ACK;

            m->level = required;
            m->boot_lock = 0;
            m->guard_latched = 0;
            m->timing = 0;
            m->prev_required = required;
            return result(m, from, why);
        }
        return result(m, from, m->boot_lock ? LG_REASON_BOOT : reason);
    }

    if (required > m->level) {
        /* Escalation is immediate. */
        m->level = required;
        m->timing = 0;
        m->prev_required = required;
        if (required == LG_LEVEL_FAILSAFE) {
            m->boot_lock = 0;
            m->guard_latched = 0;
        }
        return result(m, from, reason);
    }

    if (required < m->level) {
        /* De-escalation: the requirement must stay unchanged for t_stable. */
        if (!m->timing || required != m->prev_required) {
            m->timing = 1;
            m->since_ms = now_ms;
        }
        m->prev_required = required;
        if (lg_elapsed_ge(now_ms, m->since_ms, cfg->t_stable_ms)) {
            m->level = required;
            m->timing = 0;
            return result(m, from, LG_REASON_STABLE);
        }
        return result(m, from, reason);
    }

    m->timing = 0;
    m->prev_required = required;
    return result(m, from, reason);
}
