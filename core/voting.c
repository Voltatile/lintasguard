#include "lg_time.h"
#include "lg_voting.h"

int lg_vsensor_usable(const lg_vsensor_t *s) {
    /* Fail closed: only OK and WARNING are usable, any other value is treated as FAILED. */
    return (s->verdict == LG_VERDICT_OK || s->verdict == LG_VERDICT_WARNING) && s->fresh &&
           s->quality >= LG_SENSOR_QUALITY_USABLE_MIN;
}

lg_vote_t lg_vote_direction(const lg_vdir_t *d) {
    lg_vote_t v;
    int a = lg_vsensor_usable(&d->a);
    int b = lg_vsensor_usable(&d->b);

    v.detection = (a || b) ? LG_DET_TRUSTED : LG_DET_NONE;
    v.approaching = (uint8_t)((a && d->a.occupied) || (b && d->b.occupied));
    return v;
}

lg_vote_cfg_t lg_vote_cfg_default(void) {
    lg_vote_cfg_t c;
    c.prewarning_ms = LG_PREWARNING_MS;
    c.extra_margin_ms = LG_EXTRA_MARGIN_MS;
    return c;
}

uint32_t lg_activation_margin_ms(const lg_vote_cfg_t *cfg, uint8_t level) {
    uint32_t ms = cfg->prewarning_ms;

    if (level >= LG_LEVEL_DEGRADED) {
        ms += cfg->extra_margin_ms;
    }
    return ms;
}

lg_clear_cfg_t lg_clear_cfg_default(void) {
    lg_clear_cfg_t c;
    c.t_clear_ms = LG_T_CLEAR_MS;
    c.min_gate_down_ms = LG_MIN_GATE_DOWN_MS;
    return c;
}

void lg_clear_init(lg_clear_t *c) {
    c->island_clear_since_ms = 0;
    c->island_clear_tracking = 0;
    c->island_seen_occupied = 0;
}

void lg_clear_reset(lg_clear_t *c) {
    lg_clear_init(c);
}

uint32_t lg_clear_step(lg_clear_t *c, const lg_clear_cfg_t *cfg, const lg_clear_in_t *in) {
    uint32_t unmet = 0;
    uint8_t level = in->level > LG_LEVEL_FAILSAFE ? LG_LEVEL_FAILSAFE : in->level;
    int guard_override = (level == LG_LEVEL_UNCERTAIN) && in->guard_confirm;
    int need_sighting = (level == LG_LEVEL_UNCERTAIN) || !in->axle_counting_healthy;
    int usable = lg_vsensor_usable(&in->island);
    int island_ok;

    /* The island must have seen the train (where required) and be clear for t_clear_ms. */
    if (usable && in->island.occupied) {
        c->island_seen_occupied = 1;
    }
    if (usable && !in->island.occupied) {
        if (!c->island_clear_tracking) {
            c->island_clear_tracking = 1;
            c->island_clear_since_ms = in->now_ms;
        }
    } else {
        c->island_clear_tracking = 0;
    }
    island_ok = c->island_clear_tracking &&
                lg_elapsed_ge(in->now_ms, c->island_clear_since_ms, cfg->t_clear_ms) &&
                (!need_sighting || c->island_seen_occupied);

    if (!island_ok && !guard_override) {
        unmet |= LG_CLEAR_ISLAND_NOT_CLEAR;
    }
    if (in->axle_counting_healthy && in->axles_in != in->axles_out) {
        unmet |= LG_CLEAR_AXLE_MISMATCH;
    }
    if (in->other_dir_approaching) {
        unmet |= LG_CLEAR_OTHER_APPROACHING;
    }
    if (!in->gate_down) {
        unmet |= LG_CLEAR_GATE_NOT_DOWN;
    } else if (!lg_elapsed_ge(in->now_ms, in->gate_down_since_ms, cfg->min_gate_down_ms)) {
        unmet |= LG_CLEAR_MIN_DOWN_TIME;
    }
    if (level == LG_LEVEL_FAILSAFE) {
        unmet |= LG_CLEAR_LEVEL4;
    }
    return unmet;
}
