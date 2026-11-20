#include "lg_gate_model.h"

#include <stdio.h>
#include <string.h>

#include "lg_time.h"

lg_gate_cfg_t lg_gate_cfg_default(void) {
    lg_gate_cfg_t c;

    c.travel_ms = LG_GATE_TRAVEL_MS;
    c.current_moving_ma = LG_GATE_CURRENT_MOVING_MA;
    c.current_hold_ma = LG_GATE_CURRENT_HOLD_MA;
    c.watchdog_ms = LG_GATE_WATCHDOG_MS;
    c.fault_timeout_factor = LG_GATE_FAULT_TIMEOUT_FACTOR;
    return c;
}

const char *lg_gate_state_name(uint8_t state) {
    switch (state) {
    case LG_GATE_UP: return "UP";
    case LG_GATE_LOWERING: return "LOWERING";
    case LG_GATE_DOWN: return "DOWN";
    case LG_GATE_RAISING: return "RAISING";
    case LG_GATE_FAULT: return "FAULT";
    default: return "UNKNOWN";
    }
}

const char *lg_gate_fault_name(uint8_t fault) {
    switch (fault) {
    case LG_GFAULT_NONE: return "NONE";
    case LG_GFAULT_SLOW: return "SLOW";
    case LG_GFAULT_HIGH_CURRENT: return "HIGH_CURRENT";
    case LG_GFAULT_STUCK_UP: return "STUCK_UP";
    case LG_GFAULT_STUCK_DOWN: return "STUCK_DOWN";
    case LG_GFAULT_CRASH: return "CRASH";
    default: return "UNKNOWN";
    }
}

/* True when strictly more than limit_ms have passed. Wrap-safe; a timestamp in the future is no. */
static int elapsed_gt(uint32_t now_ms, uint32_t since_ms, uint32_t limit_ms) {
    int32_t d = (int32_t)(uint32_t)(now_ms - since_ms);

    return d >= 0 && (uint32_t)d > limit_ms;
}

static uint32_t fault_timeout_ms(const lg_gate_model_t *m) {
    uint64_t t = (uint64_t)m->cfg.travel_ms * m->cfg.fault_timeout_factor;

    return t > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)t;
}

static int at_target(const lg_gate_model_t *m) {
    return m->target == LG_GATE_TARGET_DOWN ? m->position_mdeg >= LG_GATE_POS_DOWN_MDEG
                                            : m->position_mdeg <= LG_GATE_POS_UP_MDEG;
}

static int blocked(const lg_gate_model_t *m) {
    return (m->target == LG_GATE_TARGET_DOWN && m->fault == LG_GFAULT_STUCK_UP) ||
           (m->target == LG_GATE_TARGET_UP && m->fault == LG_GFAULT_STUCK_DOWN);
}

static uint64_t effective_travel_ms(const lg_gate_model_t *m) {
    uint64_t t = m->cfg.travel_ms;

    if (m->fault == LG_GFAULT_SLOW) {
        t = t * m->fault_pct / 100u;
    }
    return t == 0 ? 1 : t;
}

/* Start timing a new movement, or cancel it when the gate is already where it should be. */
static void retarget(lg_gate_model_t *m, uint8_t target, uint32_t now_ms) {
    m->target = target;
    m->move_start_ms = now_ms;
    m->stall_ref_ms = now_ms;
    m->move_acc = 0;
    m->pending = (uint8_t)!at_target(m);
}

void lg_gate_model_init(lg_gate_model_t *m, const lg_gate_cfg_t *cfg, uint32_t now_ms) {
    memset(m, 0, sizeof *m);
    m->cfg = *cfg;
    if (m->cfg.travel_ms == 0) {
        m->cfg.travel_ms = 1; /* a zero travel time would divide by zero and time out at once */
    }
    if (m->cfg.travel_ms > LG_GATE_TRAVEL_MS_MAX) {
        m->cfg.travel_ms = LG_GATE_TRAVEL_MS_MAX; /* keeps the fixed-point products inside 64 bits */
    }
    if (m->cfg.fault_timeout_factor == 0) {
        m->cfg.fault_timeout_factor = 1;
    }
    m->state = LG_GATE_DOWN;
    m->target = LG_GATE_TARGET_DOWN;
    m->lights = 1;
    m->bell = 1;
    m->fault = LG_GFAULT_NONE;
    m->fault_pct = 100;
    m->position_mdeg = LG_GATE_POS_DOWN_MDEG;
    m->current_ma = cfg->current_hold_ma;
    m->last_step_ms = now_ms;
    m->last_feed_ms = now_ms;
    m->move_start_ms = now_ms;
    m->stall_ref_ms = now_ms;
}

void lg_gate_model_feed(lg_gate_model_t *m, uint32_t now_ms) {
    m->last_feed_ms = now_ms;
}

void lg_gate_model_step(lg_gate_model_t *m, uint32_t now_ms) {
    int32_t d = (int32_t)(uint32_t)(now_ms - m->last_step_ms);
    uint32_t dt;

    if (d < 0) {
        return; /* the clock stepped back: ignore until it catches up */
    }
    dt = (uint32_t)d;

    /* Watchdog: the controller wants the gate up but has been silent for too long. */
    if (m->target == LG_GATE_TARGET_UP &&
        elapsed_gt(now_ms, m->last_feed_ms, m->cfg.watchdog_ms)) {
        retarget(m, LG_GATE_TARGET_DOWN, now_ms);
        m->lights = 1;
        m->bell = 1;
        m->watchdog_trips++;
        dt = 0; /* the movement starts now: time before the trip must not count as travel */
    }

    /* Motor. */
    m->moving = 0;
    if (!at_target(m) && !blocked(m)) {
        uint64_t eff = effective_travel_ms(m);
        uint64_t steps;

        m->move_acc += (uint64_t)dt * (uint64_t)LG_GATE_POS_DOWN_MDEG;
        steps = m->move_acc / eff;
        m->move_acc %= eff;
        if (m->target == LG_GATE_TARGET_DOWN) {
            if (steps > (uint64_t)(LG_GATE_POS_DOWN_MDEG - m->position_mdeg)) {
                steps = (uint64_t)(LG_GATE_POS_DOWN_MDEG - m->position_mdeg);
            }
            m->position_mdeg += (int32_t)steps;
        } else {
            if (steps > (uint64_t)m->position_mdeg) {
                steps = (uint64_t)m->position_mdeg;
            }
            m->position_mdeg -= (int32_t)steps;
        }
        m->moving = 1;
    }

    /* State. A movement that takes too long is reported as FAULT but still completes. */
    if (at_target(m)) {
        if (m->pending) {
            m->last_travel_ms = (uint32_t)(now_ms - m->move_start_ms);
            m->pending = 0;
        }
        m->state = m->target == LG_GATE_TARGET_DOWN ? LG_GATE_DOWN : LG_GATE_UP;
    } else if (lg_elapsed_ge(now_ms, m->stall_ref_ms, fault_timeout_ms(m))) {
        m->state = LG_GATE_FAULT;
    } else {
        m->state = m->target == LG_GATE_TARGET_DOWN ? LG_GATE_LOWERING : LG_GATE_RAISING;
    }

    /* Arrived: the motor stops this step, only the holding current remains. */
    if (at_target(m)) {
        m->moving = 0;
    }

    /* Current. A blocked motor stays energised against the obstruction. */
    if (m->moving || (!at_target(m) && blocked(m))) {
        uint64_t ma = m->cfg.current_moving_ma;

        if (m->fault == LG_GFAULT_HIGH_CURRENT) {
            ma = ma * m->fault_pct / 100u;
        }
        m->current_ma = ma > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)ma;
    } else {
        m->current_ma = m->cfg.current_hold_ma;
    }

    m->last_step_ms = now_ms;
}

int lg_gate_model_command(lg_gate_model_t *m, uint8_t cmd, uint32_t now_ms) {
    if (cmd < LG_GATE_CMD_LOWER || cmd > LG_GATE_CMD_BELL_OFF) {
        m->rejected++;
        return -1;
    }

    /* Bring the model up to now first, so a new target starts from the right position. */
    lg_gate_model_step(m, now_ms);
    lg_gate_model_feed(m, now_ms);

    switch (cmd) {
    case LG_GATE_CMD_LOWER:
        if (m->target != LG_GATE_TARGET_DOWN) {
            retarget(m, LG_GATE_TARGET_DOWN, now_ms);
        }
        break;
    case LG_GATE_CMD_RAISE:
        if (m->target != LG_GATE_TARGET_UP) {
            retarget(m, LG_GATE_TARGET_UP, now_ms);
        }
        break;
    case LG_GATE_CMD_LIGHTS_ON: m->lights = 1; break;
    case LG_GATE_CMD_LIGHTS_OFF: m->lights = 0; break;
    case LG_GATE_CMD_BELL_ON: m->bell = 1; break;
    default: m->bell = 0; break;
    }

    lg_gate_model_step(m, now_ms);
    return 0;
}

int lg_gate_model_set_fault(lg_gate_model_t *m, uint8_t mode, uint32_t factor_pct, uint32_t now_ms) {
    uint32_t pct = 100;
    uint64_t eff_old;

    switch (mode) {
    case LG_GFAULT_NONE:
    case LG_GFAULT_STUCK_UP:
    case LG_GFAULT_STUCK_DOWN:
        break;
    case LG_GFAULT_SLOW:
    case LG_GFAULT_HIGH_CURRENT:
        if (factor_pct < 100u || factor_pct > LG_GATE_FACTOR_MAX_PCT) {
            return -1;
        }
        pct = factor_pct;
        break;
    default:
        return -1; /* CRASH is a process-level fault, unknown modes are invalid */
    }

    lg_gate_model_step(m, now_ms);
    eff_old = effective_travel_ms(m);
    if (mode != m->fault || pct != m->fault_pct) {
        /* A changed condition gets a fresh FAULT timeout. travel_ms and the fixed-point
         * remainder are not touched, and repeating the same fault changes nothing. */
        m->stall_ref_ms = now_ms;
    }
    m->fault = mode;
    m->fault_pct = pct;
    /* The remainder is measured against the effective travel time: rescale it to the new one,
     * so the position neither jumps nor loses its fraction when the speed changes. */
    m->move_acc = m->move_acc * effective_travel_ms(m) / eff_old;
    lg_gate_model_step(m, now_ms);
    return 0;
}

void lg_gate_model_fill_state(const lg_gate_model_t *m, lg_gate_state_msg_t *out) {
    out->state = m->state;
    out->reserved = 0;
    out->position_deg = (int16_t)(m->position_mdeg / 1000);
    out->current_ma = (uint16_t)(m->current_ma > 65535u ? 65535u : m->current_ma);
    out->reserved2 = 0;
    out->travel_ms = m->last_travel_ms;
    out->reserved3 = 0;
}

size_t lg_gate_model_text(const lg_gate_model_t *m, char *buf, size_t len) {
    if (len == 0) {
        return 0;
    }
    (void)snprintf(buf, len,
                   "state=%s position=%d current_ma=%u travel_ms=%u fault=%s factor_pct=%u "
                   "lights=%u bell=%u target=%s watchdog_trips=%u rejected=%u\n",
                   lg_gate_state_name(m->state), (int)(m->position_mdeg / 1000),
                   (unsigned)m->current_ma, (unsigned)m->last_travel_ms,
                   lg_gate_fault_name(m->fault), (unsigned)m->fault_pct, (unsigned)m->lights,
                   (unsigned)m->bell, m->target == LG_GATE_TARGET_UP ? "UP" : "DOWN",
                   (unsigned)m->watchdog_trips, (unsigned)m->rejected);
    return strlen(buf);
}
