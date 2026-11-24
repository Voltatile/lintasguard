#include "lg_health.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "lg_json_mini.h"
#include "lg_sensor_model.h" /* LG_SENSOR_RAW_MAX */
#include "lg_time.h"

/* A delta larger than this between two readings is not plausible: the counters were restarted. */
#define LG_HEALTH_MAX_TICKS_PER_READ 1000000u
/* Longest window that counts in the CUSUM (a long gap must not make one huge step). */
#define LG_HEALTH_MAX_WINDOW_MS 2000u
#define LG_HEALTH_WARN_CAP 69
#define LG_HEALTH_FAIL_CAP 39

lg_verdict_t lg_health_verdict_from_score(uint8_t score) {
    if (score >= LG_SCORE_OK_MIN) {
        return LG_VERDICT_OK;
    }
    if (score >= LG_SCORE_WARNING_MIN) {
        return LG_VERDICT_WARNING;
    }
    return LG_VERDICT_FAILED;
}

lg_health_cfg_t lg_health_cfg_default(void) {
    lg_health_cfg_t c;

    memset(&c, 0, sizeof c);
    c.poll_ms = LG_HEALTH_POLL_MS;
    c.poll_fast_ms = LG_HEALTH_POLL_FAST_MS;
    c.learn_ms = LG_HEALTH_LEARN_MS;
    c.sensor_period_ms = LG_SENSOR_PERIOD_MS;
    c.noise_base_milli = LG_SENSOR_BASE_NOISE * 1000;
    c.noise_learn_max_milli = LG_HEALTH_NOISE_LEARN_MAX * 1000;
    c.noise_fail_milli = LG_HEALTH_NOISE_FAIL * 1000;
    c.cusum_k_milli = LG_HEALTH_CUSUM_K_MILLI;
    c.cusum_h_milli = LG_HEALTH_CUSUM_H_MILLI;
    c.dropout_warn_pm = LG_HEALTH_DROPOUT_WARN_PM;
    c.dropout_fail_pm = LG_HEALTH_DROPOUT_FAIL_PM;
    c.delay_zero_ms = LG_HEALTH_DELAY_ZERO_MS;
    c.delay_fail_ms = LG_HEALTH_DELAY_FAIL_MS;
    c.pair_window_ms = LG_HEALTH_PAIR_WINDOW_MS;
    c.disagree_clear = LG_HEALTH_DISAGREE_CLEAR;
    c.gate_travel_nominal_ms = LG_GATE_TRAVEL_MS;
    c.gate_travel_fail_pct = LG_HEALTH_GATE_TRAVEL_FAIL_PCT;
    c.gate_current_nominal_ma = LG_GATE_CURRENT_MOVING_MA;
    c.gate_current_fail_pct = LG_HEALTH_GATE_CURRENT_FAIL_PCT;
    c.gate_cusum_k_pct = LG_HEALTH_GATE_CUSUM_K_PCT;
    c.gate_cusum_h_pct = LG_HEALTH_GATE_CUSUM_H_PCT;
    c.battery_full_mv = LG_BATTERY_FULL_MV;
    c.battery_empty_mv = LG_BATTERY_EMPTY_MV;
    c.battery_warn_mv = LG_BATTERY_WARN_MV;
    c.battery_fail_mv = LG_BATTERY_FAIL_MV;
    c.battery_tte_warn_s = LG_BATTERY_TTE_WARN_S;
    c.battery_tte_fail_s = LG_BATTERY_TTE_FAIL_S;
    c.ok_score = LG_HEALTH_OK_SCORE;
    c.ok_hold_ms = LG_HEALTH_OK_HOLD_MS;
    c.nodata_polls = LG_HEALTH_NODATA_POLLS;
    return c;
}

/*
 * Score of a statistic where larger is worse: 100 at or below x_ok, falling to 70 just below x_warn,
 * 69 at x_warn falling to 40 just below x_fail, 39 at x_fail falling to 0. Needs x_ok < x_warn < x_fail.
 */
static int32_t score_of(int64_t x, int64_t x_ok, int64_t x_warn, int64_t x_fail) {
    int64_t s;

    if (x_warn <= x_ok || x_fail <= x_warn) {
        return x >= x_fail ? LG_HEALTH_FAIL_CAP : 100; /* bad configuration: only the hard limit */
    }
    if (x <= x_ok) {
        return 100;
    }
    if (x < x_warn) {
        s = 100 - 30 * (x - x_ok) / (x_warn - x_ok);
        return (int32_t)(s < 70 ? 70 : s);
    }
    if (x < x_fail) {
        s = LG_HEALTH_WARN_CAP - 29 * (x - x_warn) / (x_fail - x_warn);
        return (int32_t)(s < 40 ? 40 : s);
    }
    s = LG_HEALTH_FAIL_CAP - 39 * (x - x_fail) / (x_fail - x_warn);
    return (int32_t)(s < 0 ? 0 : s);
}

static int64_t ewma(int64_t avg, int64_t x, int shift) {
    return avg + (x - avg) / (1 << shift);
}

/* Seconds until value reaches limit at slope (units per 1000 s); -1 when not moving towards it. */
static int32_t trend_of(int64_t value, int64_t limit, int64_t slope_per_ks) {
    int64_t t;

    if (value >= limit) {
        return 0;
    }
    if (slope_per_ks <= 0) {
        return -1;
    }
    t = (limit - value) * 1000 / slope_per_ks;
    return t > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)t;
}

static void comp_clear(lg_health_comp_t *c, const lg_health_cfg_t *cfg, uint32_t now_ms) {
    memset(c, 0, sizeof *c);
    c->score = 100;
    c->verdict = LG_VERDICT_OK;
    c->diag = LG_DIAG_NONE;
    c->trend_s = -1;
    c->learning = 1;
    c->start_ms = now_ms;
    c->noise_base_milli = cfg->noise_base_milli;
    c->batt_tte_s = -1;
}

void lg_health_init(lg_health_t *h, const lg_health_cfg_t *cfg, uint32_t now_ms) {
    int i;

    memset(h, 0, sizeof *h);
    h->cfg = *cfg;
    for (i = 0; i < LG_COMP_COUNT; i++) {
        comp_clear(&h->c[i], cfg, now_ms);
    }
}

void lg_health_reset(lg_health_t *h, uint8_t comp, uint32_t now_ms) {
    if (comp >= LG_COMP_COUNT) {
        return;
    }
    comp_clear(&h->c[comp], &h->cfg, now_ms);
    h->c[comp].assessed = 1;
    if (comp == LG_COMP_GATE) {
        comp_clear(&h->c[LG_COMP_BATTERY], &h->cfg, now_ms);
        h->c[LG_COMP_BATTERY].assessed = 1;
    }
}

/* Applies a new score: worse at once, better only after it held for ok_hold_ms (OK also needs ok_score). */
static void finish(lg_health_t *h, lg_health_comp_t *c, int32_t score, uint16_t diag, int32_t trend,
                   uint32_t now_ms) {
    uint8_t target;

    if (c->nodata >= h->cfg.nodata_polls) {
        c->assessed = 1; /* NO_DATA is a judgement too */
    }
    if (c->nodata >= h->cfg.nodata_polls && score > LG_HEALTH_WARN_CAP) {
        score = LG_HEALTH_WARN_CAP;
        diag = LG_DIAG_NO_DATA;
        trend = -1;
    }
    if (score < 0) {
        score = 0;
    }
    if (score > 100) {
        score = 100;
    }
    c->score = (uint8_t)score;
    target = (uint8_t)lg_health_verdict_from_score(c->score);
    if (target == LG_VERDICT_OK && c->score < h->cfg.ok_score && c->verdict != LG_VERDICT_OK) {
        target = LG_VERDICT_WARNING; /* hysteresis band */
    }
    if (target > c->verdict) {
        c->verdict = target;
        c->better_tracking = 0;
        c->changes++;
    } else if (target < c->verdict) {
        if (!c->better_tracking || target != c->better_target) {
            c->better_tracking = 1; /* (re)start the hold: a different better verdict starts from zero */
            c->better_target = target;
            c->better_since_ms = now_ms;
        } else if (lg_elapsed_ge(now_ms, c->better_since_ms, h->cfg.ok_hold_ms)) {
            c->verdict = target;
            c->better_tracking = 0;
            c->changes++;
        }
    } else {
        c->better_tracking = 0;
    }
    if (c->verdict == LG_VERDICT_OK) {
        c->diag = LG_DIAG_NONE;
        c->trend_s = -1;
    } else if (diag != LG_DIAG_NONE) {
        c->diag = diag; /* while getting better the last reason is kept */
        c->trend_s = trend;
    }
}

/* ---- Sensors ----------------------------------------------------------- */

static int partner_of(uint8_t comp) {
    uint8_t id = (uint8_t)(comp % LG_SENSOR_ID_COUNT);

    if (id == LG_SENSOR_A) {
        return comp + 1;
    }
    if (id == LG_SENSOR_B) {
        return comp - 1;
    }
    return -1;
}

static void delay_sample(lg_health_comp_t *c, uint32_t delay_ms) {
    if (c->delay_samples == 0) {
        c->delay_ewma_ms = delay_ms;
    } else {
        c->delay_ewma_ms = (uint32_t)ewma(c->delay_ewma_ms, delay_ms, 1);
    }
    c->delay_samples++;
}

static void agreed(const lg_health_cfg_t *cfg, lg_health_comp_t *c) {
    c->agree_run++;
    if (c->disagree && c->agree_run >= cfg->disagree_clear) {
        c->disagree = 0;
    }
}

/*
 * `who` did not rise for a train `seen_by` rose for. Only a sensor that is itself OK can accuse its
 * partner: a noisy or failed sensor reads phantom trains and must not make the healthy one look bad.
 */
static void missed(lg_health_comp_t *who, const lg_health_comp_t *seen_by) {
    if (seen_by->verdict != LG_VERDICT_OK) {
        return;
    }
    who->disagree = 1;
    who->agree_run = 0;
    who->disagreements++;
}

/*
 * A pending rise of a that its partner b did not match. Judged only on a completed, plausible run of a
 * (as the controller's MISSED_TRAIN rule): a blip shorter than LG_T_DISAGREE_MS or a run that does not
 * end within LG_SENSOR_MAX_OCCUPIED_MS (a sensor stuck occupied) accuses nobody.
 */
static void lone_judge(lg_health_t *h, lg_health_comp_t *a, lg_health_comp_t *b, uint32_t now_ms) {
    int32_t run = (int32_t)(uint32_t)(a->fall_ms - a->rise_ms);

    if (!a->rise_pending) {
        return;
    }
    if (b->rise_pending) {
        int32_t d = (int32_t)(uint32_t)(a->rise_ms - b->rise_ms);

        if ((uint32_t)(d < 0 ? -d : d) <= h->cfg.pair_window_ms) {
            return; /* matched by pair_step */
        }
    }
    if (a->fell) { /* a fell after its rise: the run is complete */
        if ((uint32_t)run < LG_T_DISAGREE_MS || (uint32_t)run > LG_SENSOR_MAX_OCCUPIED_MS) {
            a->rise_pending = 0; /* a blip or no train: nobody is accused */
            return;
        }
        if (lg_elapsed_ge(now_ms, a->fall_ms, 2u * h->cfg.poll_ms) && b->have_prev) {
            a->rise_pending = 0;
            missed(b, a);
        }
        return;
    }
    if (lg_elapsed_ge(now_ms, a->rise_ms, LG_SENSOR_MAX_OCCUPIED_MS)) {
        a->rise_pending = 0; /* occupied for longer than any train: a is the odd one, not b */
    }
}

/* Matches the pending rises of an approach pair (called after either sensor was read). */
static void pair_step(lg_health_t *h, uint8_t comp, uint32_t now_ms) {
    const lg_health_cfg_t *cfg = &h->cfg;
    int p = partner_of(comp);
    lg_health_comp_t *x;
    lg_health_comp_t *y;

    if (p < 0) {
        return;
    }
    x = &h->c[comp];
    y = &h->c[p];
    if (x->rise_pending && y->rise_pending) {
        int32_t d = (int32_t)(uint32_t)(x->rise_ms - y->rise_ms);
        uint32_t ad = (uint32_t)(d < 0 ? -d : d);

        if (ad <= cfg->pair_window_ms) {
            delay_sample(d > 0 ? x : y, ad); /* the later one carries the delay */
            delay_sample(d > 0 ? y : x, 0);
            agreed(cfg, x);
            agreed(cfg, y);
            x->rise_pending = 0;
            y->rise_pending = 0;
            return;
        }
        /* Far apart: not the same train. Each pending rise is judged alone below. */
    }
    lone_judge(h, x, y, now_ms);
    lone_judge(h, y, x, now_ms);
}

static void sensor_score(lg_health_t *h, uint8_t comp, uint32_t now_ms) {
    const lg_health_cfg_t *cfg = &h->cfg;
    lg_health_comp_t *c = &h->c[comp];
    int32_t score = 100;
    uint16_t diag = LG_DIAG_NONE;
    int32_t trend = -1;
    int32_t s;

    if (c->have_noise) {
        int64_t base = c->noise_base_milli;
        int64_t fail = cfg->noise_fail_milli;

        s = score_of(c->noise_ewma_milli, base, base + (fail - base) / 2, fail);
        if (s < score) {
            score = s;
            /* At or past the failure threshold the noise is high, whatever the CUSUM says. */
            diag = (c->cusum_alarm && s >= LG_SCORE_WARNING_MIN) ? LG_DIAG_NOISE_RISING : LG_DIAG_NOISE_HIGH;
            trend = trend_of(c->noise_ewma_milli, fail, (int64_t)c->noise_slope_milli * 1000);
        }
    }
    s = score_of(c->drop_ewma_pm, 0, cfg->dropout_warn_pm, cfg->dropout_fail_pm);
    if (s < score) {
        score = s;
        diag = LG_DIAG_DROPOUTS;
        trend = -1;
    }
    if (c->delay_samples > 0) {
        int64_t z = cfg->delay_zero_ms;
        int64_t f = cfg->delay_fail_ms;

        s = score_of(c->delay_ewma_ms, z, z + (f - z) / 2, f);
        if (s < score) {
            score = s;
            diag = LG_DIAG_RESPONSE_DELAY;
            trend = -1;
        }
    }
    if (c->disagree && score > LG_HEALTH_WARN_CAP) {
        score = LG_HEALTH_WARN_CAP;
        diag = LG_DIAG_DISAGREEMENT;
        trend = -1;
    }
    if (c->cusum_alarm && score > LG_HEALTH_WARN_CAP) {
        int64_t fail = cfg->noise_fail_milli;

        score = LG_HEALTH_WARN_CAP;
        diag = LG_DIAG_NOISE_RISING;
        trend = trend_of(c->noise_ewma_milli, fail, (int64_t)c->noise_slope_milli * 1000);
    }
    if (score >= LG_SCORE_OK_MIN) {
        diag = LG_DIAG_NONE;
        trend = -1;
    }
    finish(h, c, score, diag, trend, now_ms);
}

static void noise_window(const lg_health_cfg_t *cfg, lg_health_comp_t *c, uint64_t dsum, uint32_t dcount,
                         uint32_t window_ms, uint32_t now_ms) {
    int64_t n = (int64_t)(dsum * 250u / dcount); /* mean deviation / 4, milli noise units */
    int64_t old = c->noise_ewma_milli;

    if (n > 0x7FFFFFFF) {
        n = 0x7FFFFFFF;
    }
    c->noise_milli = (int32_t)n;
    if (!c->have_noise) {
        c->noise_ewma_milli = (int32_t)n;
        c->have_noise = 1;
    } else {
        int64_t inst;

        c->noise_ewma_milli = (int32_t)ewma(old, n, 3);
        inst = ((int64_t)c->noise_ewma_milli - old) * 1000 / (window_ms > 0 ? window_ms : 1);
        c->noise_slope_milli = (int32_t)ewma(c->noise_slope_milli, inst, 3);
    }
    if (c->learning) {
        c->learn_sum += n * dcount;
        c->learn_n += dcount;
        if (lg_elapsed_ge(now_ms, c->start_ms, cfg->learn_ms) && c->learn_n > 0) {
            int64_t base = c->learn_sum / c->learn_n;

            if (base < cfg->noise_base_milli) {
                base = cfg->noise_base_milli;
            }
            if (base > (int64_t)cfg->noise_base_milli + cfg->noise_learn_max_milli) {
                base = (int64_t)cfg->noise_base_milli + cfg->noise_learn_max_milli;
            }
            c->noise_base_milli = (int32_t)base;
            c->learning = 0;
        }
        return; /* no CUSUM while the baseline is not known */
    }
    {
        int64_t w = window_ms > LG_HEALTH_MAX_WINDOW_MS ? LG_HEALTH_MAX_WINDOW_MS : window_ms;
        int64_t s = c->cusum_milli + (n - c->noise_base_milli - cfg->cusum_k_milli) * w / 1000;
        int64_t cap = 2 * (int64_t)cfg->cusum_h_milli;

        s = s < 0 ? 0 : (s > cap ? cap : s);
        c->cusum_milli = (int32_t)s;
        if (s >= cfg->cusum_h_milli) {
            c->cusum_alarm = 1;
        } else if (s < cfg->cusum_h_milli / 2) {
            c->cusum_alarm = 0;
        }
    }
}

int lg_health_on_sensor(lg_health_t *h, uint8_t comp, const lg_health_sensor_in_t *in, uint32_t now_ms) {
    const lg_health_cfg_t *cfg = &h->cfg;
    lg_health_comp_t *c;
    uint32_t dt, dns, dbl, dcnt, drise;
    uint64_t dsum;

    if (comp > LG_COMP_SENSOR_LAST || in == NULL) {
        return -1;
    }
    c = &h->c[comp];
    c->nodata = 0;
    dt = in->ticks - c->p_ticks;
    dns = in->no_sample - c->p_no_sample;
    dbl = in->blind - c->p_blind;
    dcnt = in->dev_count - c->p_dev_count;
    dsum = in->dev_sum - c->p_dev_sum;
    drise = in->rise_count - c->p_rise_count;
    if (c->have_prev && (dt > LG_HEALTH_MAX_TICKS_PER_READ || dns > dt || dbl > dt || dcnt > dt ||
                         dsum > (uint64_t)dcnt * (LG_SENSOR_RAW_MAX + 1u) || drise > dt)) {
        c->have_prev = 0; /* the counters started again (a new sensor process) */
    }
    if (!c->have_prev) {
        c->p_ticks = in->ticks;
        c->p_no_sample = in->no_sample;
        c->p_blind = in->blind;
        c->p_dev_count = in->dev_count;
        c->p_dev_sum = in->dev_sum;
        c->p_rise_count = in->rise_count;
        c->have_prev = 1;
        sensor_score(h, comp, now_ms);
        return 0;
    }
    if (dt == 0) {
        sensor_score(h, comp, now_ms); /* nothing new (the sensor did not tick) */
        return 0;
    }
    c->p_ticks = in->ticks;
    c->p_no_sample = in->no_sample;
    c->p_blind = in->blind;
    c->p_dev_count = in->dev_count;
    c->p_dev_sum = in->dev_sum;
    c->p_rise_count = in->rise_count;

    c->drop_ewma_pm = (uint32_t)ewma(c->drop_ewma_pm, (int64_t)(dns + dbl) * 1000 / dt, 2);
    c->assessed = 1; /* a window of the sensor's own counters was judged */
    if (dcnt > 0) {
        noise_window(cfg, c, dsum, dcnt, dt * cfg->sensor_period_ms, now_ms);
    }
    if (drise > 0 && partner_of(comp) >= 0) {
        c->rise_pending = 1;
        c->rise_ms = in->last_rise_ms;
        /* A fall in the same window counts only when it is later than the new rise. */
        c->fell = (uint8_t)(in->last_fall_ms != c->fall_ms &&
                            (int32_t)(uint32_t)(in->last_fall_ms - in->last_rise_ms) > 0);
    } else if (in->last_fall_ms != c->fall_ms && c->rise_pending) {
        c->fell = 1; /* the run of the pending rise ended */
    }
    c->fall_ms = in->last_fall_ms;
    pair_step(h, comp, now_ms);
    sensor_score(h, comp, now_ms);
    if (partner_of(comp) >= 0) {
        lg_health_comp_t *p = &h->c[partner_of(comp)];

        if (p->have_prev) {
            sensor_score(h, (uint8_t)partner_of(comp), now_ms); /* the pair step may have judged it */
        }
    }
    return 0;
}

/* ---- Gate and battery --------------------------------------------------- */

static void gate_score(lg_health_t *h, uint32_t now_ms) {
    const lg_health_cfg_t *cfg = &h->cfg;
    lg_health_comp_t *c = &h->c[LG_COMP_GATE];
    int32_t score = 100;
    uint16_t diag = LG_DIAG_NONE;
    int64_t tfail = (int64_t)cfg->gate_travel_fail_pct * 10;
    int64_t cfail = (int64_t)cfg->gate_current_fail_pct * 10;
    int32_t st, sc;
    int32_t trend = -1;

    if (c->have_move) {
        st = score_of(c->travel_ewma_pm, 1000, 1000 + (tfail - 1000) / 2, tfail);
        sc = score_of(c->current_ewma_pm, 1000, 1000 + (cfail - 1000) / 2, cfail);
        if (c->travel_alarm && st > LG_HEALTH_WARN_CAP) {
            st = LG_HEALTH_WARN_CAP;
        }
        if (c->current_alarm && sc > LG_HEALTH_WARN_CAP) {
            sc = LG_HEALTH_WARN_CAP;
        }
        if (c->latest_fail == 1 && st > LG_HEALTH_FAIL_CAP) {
            st = LG_HEALTH_FAIL_CAP;
        }
        if (c->latest_fail == 2 && sc > LG_HEALTH_FAIL_CAP) {
            sc = LG_HEALTH_FAIL_CAP;
        }
        if (st <= sc && st < 100) {
            score = st;
            diag = LG_DIAG_GATE_SLOW;
            trend = trend_of(c->travel_ewma_pm, tfail, c->gate_slope_pm);
        } else if (sc < 100) {
            score = sc;
            diag = LG_DIAG_GATE_HIGH_CURRENT;
            trend = trend_of(c->current_ewma_pm, cfail, c->gate_slope_pm);
        }
    }
    if (score >= LG_SCORE_OK_MIN) {
        diag = LG_DIAG_NONE;
        trend = -1;
    }
    finish(h, c, score, diag, trend, now_ms);
}

static int32_t cusum_step(int32_t s, int64_t x_pm, int64_t k_pm, int64_t h_pm, uint8_t *alarm) {
    int64_t v = (int64_t)s + x_pm - (1000 + k_pm);

    v = v < 0 ? 0 : (v > 2 * h_pm ? 2 * h_pm : v);
    if (v >= h_pm) {
        *alarm = 1;
    } else if (v < h_pm / 2) {
        *alarm = 0;
    }
    return (int32_t)v;
}

static void gate_move(lg_health_t *h, const lg_health_gate_in_t *in, uint32_t now_ms) {
    const lg_health_cfg_t *cfg = &h->cfg;
    lg_health_comp_t *c = &h->c[LG_COMP_GATE];
    uint32_t tn = cfg->gate_travel_nominal_ms ? cfg->gate_travel_nominal_ms : 1u;
    uint32_t cn = cfg->gate_current_nominal_ma ? cfg->gate_current_nominal_ma : 1u;
    int64_t t_pm = (int64_t)in->last_travel_ms * 1000 / tn;
    int64_t c_pm = (int64_t)in->last_peak_ma * 1000 / cn;
    int64_t old_t = c->travel_ewma_pm;
    int64_t old_c = c->current_ewma_pm;
    int64_t k = (int64_t)cfg->gate_cusum_k_pct * 10;
    int64_t hh = (int64_t)cfg->gate_cusum_h_pct * 10;

    if (t_pm > 0x7FFFFFFF) {
        t_pm = 0x7FFFFFFF;
    }
    if (c_pm > 0x7FFFFFFF) {
        c_pm = 0x7FFFFFFF;
    }
    if (!c->have_move) {
        c->travel_ewma_pm = (int32_t)t_pm;
        c->current_ewma_pm = (int32_t)c_pm;
        c->have_move = 1;
    } else {
        uint32_t dt = now_ms - c->last_move_ms;
        int64_t dt_s = dt / 1000u > 0 ? dt / 1000u : 1;
        int64_t sl;

        c->travel_ewma_pm = (int32_t)ewma(old_t, t_pm, 1);
        c->current_ewma_pm = (int32_t)ewma(old_c, c_pm, 1);
        /* slope of the travel ratio (or the current if that is the worse one), per mille per 1000 s */
        sl = (c->travel_ewma_pm - old_t >= c->current_ewma_pm - old_c ? c->travel_ewma_pm - old_t
                                                                    : c->current_ewma_pm - old_c) *
             1000 / dt_s;
        sl = sl > 0x3FFFFFFF ? 0x3FFFFFFF : (sl < -0x3FFFFFFF ? -0x3FFFFFFF : sl);
        c->gate_slope_pm = (int32_t)ewma(c->gate_slope_pm, sl, 1);
    }
    c->travel_cusum_pm = cusum_step(c->travel_cusum_pm, t_pm, k, hh, &c->travel_alarm);
    c->current_cusum_pm = cusum_step(c->current_cusum_pm, c_pm, k, hh, &c->current_alarm);
    if (t_pm >= (int64_t)cfg->gate_travel_fail_pct * 10) {
        c->latest_fail = 1;
    } else if (c_pm >= (int64_t)cfg->gate_current_fail_pct * 10) {
        c->latest_fail = 2;
    } else {
        c->latest_fail = 0;
    }
    c->last_move_ms = now_ms;
    c->moves++;
    c->assessed = 1; /* a completed movement was judged */
}

static void battery_step(lg_health_t *h, uint32_t mv, uint32_t now_ms) {
    const lg_health_cfg_t *cfg = &h->cfg;
    lg_health_comp_t *c = &h->c[LG_COMP_BATTERY];
    int64_t uv = (int64_t)mv * 1000;
    int64_t v_mv;
    int32_t score;
    uint16_t diag = LG_DIAG_BATTERY_LOW;
    int32_t trend = -1;

    c->nodata = 0;
    if (!c->have_batt) {
        c->batt_ewma_uv = uv;
        c->have_batt = 1;
        c->assessed = 1;
    } else {
        uint32_t dt = now_ms - c->batt_ms;
        int64_t old = c->batt_ewma_uv;
        int64_t inst;

        if (dt == 0) {
            return;
        }
        c->batt_ewma_uv = ewma(old, uv, 3);
        inst = (c->batt_ewma_uv - old) * 1000 / dt; /* uV per s */
        c->batt_slope_umv = (int32_t)ewma(c->batt_slope_umv, inst, 3);
    }
    c->batt_ms = now_ms;
    v_mv = c->batt_ewma_uv / 1000;
    /* Draining faster than 0.1 mV/s gives a time to empty. */
    if (c->batt_slope_umv < -100) {
        int64_t tte = (v_mv - cfg->battery_empty_mv) * 1000 / -(int64_t)c->batt_slope_umv;

        c->batt_tte_s = tte < 0 ? 0 : (tte > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)tte);
        trend = trend_of(-v_mv, -(int64_t)cfg->battery_fail_mv, -(int64_t)c->batt_slope_umv);
    } else {
        c->batt_tte_s = -1;
    }
    score = score_of(-v_mv, -(int64_t)cfg->battery_full_mv, -(int64_t)cfg->battery_warn_mv,
                     -(int64_t)cfg->battery_fail_mv);
    if (c->batt_tte_s >= 0 && (uint32_t)c->batt_tte_s < cfg->battery_tte_fail_s && score > LG_HEALTH_FAIL_CAP) {
        score = LG_HEALTH_FAIL_CAP;
        diag = LG_DIAG_BATTERY_DRAIN;
    } else if (c->batt_tte_s >= 0 && (uint32_t)c->batt_tte_s < cfg->battery_tte_warn_s &&
               score > LG_HEALTH_WARN_CAP) {
        score = LG_HEALTH_WARN_CAP;
        diag = LG_DIAG_BATTERY_DRAIN;
    }
    if (score >= LG_SCORE_OK_MIN) {
        diag = LG_DIAG_NONE;
        trend = -1;
    }
    finish(h, c, score, diag, trend, now_ms);
}

void lg_health_on_gate(lg_health_t *h, const lg_health_gate_in_t *in, uint32_t now_ms) {
    lg_health_comp_t *c = &h->c[LG_COMP_GATE];

    if (in == NULL) {
        return;
    }
    c->nodata = 0;
    if (!c->have_prev) {
        c->p_moves = in->moves_done; /* reference only: a movement before a reset is not judged */
        c->have_prev = 1;
    } else {
        uint32_t d = in->moves_done - c->p_moves;

        if (d > LG_HEALTH_MAX_TICKS_PER_READ) {
            c->p_moves = in->moves_done; /* the gate process was restarted */
        } else if (d > 0) {
            c->p_moves = in->moves_done;
            gate_move(h, in, now_ms);
        }
    }
    gate_score(h, now_ms);
    battery_step(h, in->battery_mv, now_ms);
}

void lg_health_on_no_data(lg_health_t *h, uint8_t comp, uint32_t now_ms) {
    lg_health_comp_t *c;

    if (comp >= LG_COMP_COUNT || comp == LG_COMP_BATTERY) {
        return;
    }
    c = &h->c[comp];
    if (c->nodata < 0xFF) {
        c->nodata++;
    }
    if (comp == LG_COMP_GATE) {
        lg_health_comp_t *b = &h->c[LG_COMP_BATTERY];

        if (b->nodata < 0xFF) {
            b->nodata++;
        }
        finish(h, c, c->score, c->diag, c->trend_s, now_ms);
        finish(h, b, b->score, b->diag, b->trend_s, now_ms);
        return;
    }
    finish(h, c, c->score, c->diag, c->trend_s, now_ms);
}

lg_health_out_t lg_health_get(const lg_health_t *h, uint8_t comp) {
    lg_health_out_t o;

    memset(&o, 0, sizeof o);
    if (comp >= LG_COMP_COUNT) {
        o.score = 0;
        o.verdict = LG_VERDICT_FAILED;
        o.diag = LG_DIAG_NO_DATA;
        o.trend_s = -1;
        o.assessed = 1;
        return o;
    }
    o.assessed = h->c[comp].assessed;
    o.score = h->c[comp].score;
    o.verdict = h->c[comp].verdict;
    o.diag = h->c[comp].diag;
    o.trend_s = h->c[comp].trend_s;
    return o;
}

uint32_t lg_health_poll_period_ms(const lg_health_t *h, uint8_t comp) {
    if (comp < LG_COMP_COUNT && h->c[comp].verdict != LG_VERDICT_OK) {
        return h->cfg.poll_fast_ms;
    }
    return h->cfg.poll_ms;
}

const char *lg_health_diag_name(uint16_t diag) {
    switch (diag) {
    case LG_DIAG_NONE: return "NONE";
    case LG_DIAG_NOISE_RISING: return "NOISE_RISING";
    case LG_DIAG_NOISE_HIGH: return "NOISE_HIGH";
    case LG_DIAG_DROPOUTS: return "DROPOUTS";
    case LG_DIAG_RESPONSE_DELAY: return "RESPONSE_DELAY";
    case LG_DIAG_DISAGREEMENT: return "DISAGREEMENT";
    case LG_DIAG_GATE_SLOW: return "GATE_SLOW";
    case LG_DIAG_GATE_HIGH_CURRENT: return "GATE_HIGH_CURRENT";
    case LG_DIAG_BATTERY_DRAIN: return "BATTERY_DRAIN";
    case LG_DIAG_BATTERY_LOW: return "BATTERY_LOW";
    case LG_DIAG_NO_DATA: return "NO_DATA";
    default: return "UNKNOWN";
    }
}

const char *lg_health_diag_text(uint16_t diag) {
    switch (diag) {
    case LG_DIAG_NONE: return "healthy";
    case LG_DIAG_NOISE_RISING: return "noise rising, likely connector corrosion or loose cable";
    case LG_DIAG_NOISE_HIGH: return "noise high, check cable and shielding";
    case LG_DIAG_DROPOUTS: return "samples missing, check link and power";
    case LG_DIAG_RESPONSE_DELAY: return "responds late, check sensor head and mounting";
    case LG_DIAG_DISAGREEMENT: return "missed a train its partner saw, inspect sensor";
    case LG_DIAG_GATE_SLOW: return "travel time rising, likely mechanical wear";
    case LG_DIAG_GATE_HIGH_CURRENT: return "motor current rising, check bearings and arm";
    case LG_DIAG_BATTERY_DRAIN: return "battery draining, check charger";
    case LG_DIAG_BATTERY_LOW: return "battery voltage low, replace battery";
    case LG_DIAG_NO_DATA: return "health monitor cannot read the device";
    default: return "unknown diagnosis";
    }
}

const char *lg_health_comp_name(uint8_t comp) {
    static const char *const names[LG_COMP_COUNT] = {"west/a", "west/b", "west/island", "east/a",
                                                     "east/b", "east/island", "gate", "battery"};

    return comp < LG_COMP_COUNT ? names[comp] : "?";
}

static const char *verdict_name(uint8_t v) {
    switch (v) {
    case LG_VERDICT_OK: return "OK";
    case LG_VERDICT_WARNING: return "WARNING";
    case LG_VERDICT_FAILED: return "FAILED";
    default: return "UNKNOWN";
    }
}

size_t lg_health_text(const lg_health_t *h, char *buf, size_t len) {
    size_t used = 0;
    uint8_t i;

    if (buf == NULL || len == 0) {
        return 0;
    }
    buf[0] = '\0';
    for (i = 0; i < LG_COMP_COUNT; i++) {
        const lg_health_comp_t *c = &h->c[i];
        int n;

        if (i == LG_COMP_GATE) {
            n = snprintf(buf + used, len - used,
                         "comp=gate score=%u verdict=%s diag=%s trend_s=%d moves=%u travel_pm=%d "
                         "current_pm=%d cusum_travel=%d cusum_current=%d\n",
                         (unsigned)c->score, verdict_name(c->verdict), lg_health_diag_name(c->diag),
                         (int)c->trend_s, (unsigned)c->moves, (int)c->travel_ewma_pm, (int)c->current_ewma_pm,
                         (int)c->travel_cusum_pm, (int)c->current_cusum_pm);
        } else if (i == LG_COMP_BATTERY) {
            n = snprintf(buf + used, len - used,
                         "comp=battery score=%u verdict=%s diag=%s trend_s=%d mv=%d slope_uv_s=%d tte_s=%d\n",
                         (unsigned)c->score, verdict_name(c->verdict), lg_health_diag_name(c->diag),
                         (int)c->trend_s, (int)(c->batt_ewma_uv / 1000), (int)c->batt_slope_umv,
                         (int)c->batt_tte_s);
        } else {
            n = snprintf(buf + used, len - used,
                         "comp=%s score=%u verdict=%s diag=%s trend_s=%d noise_milli=%d base_milli=%d "
                         "cusum=%d drop_pm=%u delay_ms=%u disagree=%u learning=%u\n",
                         lg_health_comp_name(i), (unsigned)c->score, verdict_name(c->verdict),
                         lg_health_diag_name(c->diag), (int)c->trend_s, (int)c->noise_ewma_milli,
                         (int)c->noise_base_milli, (int)c->cusum_milli, (unsigned)c->drop_ewma_pm,
                         (unsigned)c->delay_ewma_ms, (unsigned)c->disagree, (unsigned)c->learning);
        }
        if (n < 0) {
            break;
        }
        if ((size_t)n >= len - used) {
            used = len - 1; /* truncated: snprintf wrote a terminated prefix */
            break;
        }
        used += (size_t)n;
    }
    buf[used] = '\0';
    return used;
}

/* ---- config/thresholds.json ---------------------------------------------- */

typedef struct {
    const char *key;
    size_t off;
    uint8_t kind; /* 0 = uint32 whole, 1 = int32 whole, 2 = int32 milli (noise units), 3 = uint8 whole */
    int64_t min;  /* in the JSON's own unit */
    int64_t max;
} cfg_key_t;

#define CK(k, f, kind, lo, hi) {k, offsetof(lg_health_cfg_t, f), kind, lo, hi}

static const cfg_key_t g_cfg_keys[] = {
    CK("poll_ms", poll_ms, 0, 10, 10000),
    CK("poll_fast_ms", poll_fast_ms, 0, 10, 10000),
    CK("learn_ms", learn_ms, 0, 0, 600000),
    CK("noise_base", noise_base_milli, 2, 0, 100),
    CK("noise_learn_max", noise_learn_max_milli, 2, 0, 100),
    CK("noise_fail", noise_fail_milli, 2, 1, 100),
    CK("cusum_k", cusum_k_milli, 2, 0, 100),
    CK("cusum_h", cusum_h_milli, 2, 1, 1000),
    CK("dropout_warn_pm", dropout_warn_pm, 0, 1, 1000),
    CK("dropout_fail_pm", dropout_fail_pm, 0, 1, 1000),
    CK("delay_zero_ms", delay_zero_ms, 0, 0, 10000),
    CK("delay_fail_ms", delay_fail_ms, 0, 1, 10000),
    CK("pair_window_ms", pair_window_ms, 0, 10, 60000),
    CK("disagree_clear", disagree_clear, 0, 1, 100),
    CK("gate_travel_nominal_ms", gate_travel_nominal_ms, 0, 1, 1000000),
    CK("gate_travel_fail_pct", gate_travel_fail_pct, 0, 101, 1000),
    CK("gate_current_nominal_ma", gate_current_nominal_ma, 0, 1, 100000),
    CK("gate_current_fail_pct", gate_current_fail_pct, 0, 101, 1000),
    CK("gate_cusum_k_pct", gate_cusum_k_pct, 0, 0, 100),
    CK("gate_cusum_h_pct", gate_cusum_h_pct, 0, 1, 1000),
    CK("battery_full_mv", battery_full_mv, 1, 1000, 100000),
    CK("battery_empty_mv", battery_empty_mv, 1, 0, 100000),
    CK("battery_warn_mv", battery_warn_mv, 1, 1000, 100000),
    CK("battery_fail_mv", battery_fail_mv, 1, 1000, 100000),
    CK("battery_tte_warn_s", battery_tte_warn_s, 0, 1, 1000000),
    CK("battery_tte_fail_s", battery_tte_fail_s, 0, 1, 1000000),
    CK("ok_score", ok_score, 3, 70, 100),
    CK("ok_hold_ms", ok_hold_ms, 0, 0, 600000),
    CK("nodata_polls", nodata_polls, 0, 1, 100),
};

static int cfg_fail(char *err, size_t errlen, const char *what, const char *key) {
    if (err != NULL && errlen > 0) {
        (void)snprintf(err, errlen, "thresholds: %s%s%s", what, key != NULL ? " " : "", key != NULL ? key : "");
    }
    return -1;
}

int lg_health_cfg_from_json(const lg_json_doc_t *doc, lg_health_cfg_t *out, char *err, size_t errlen) {
    const lg_json_node_t *root = lg_json_root(doc);
    lg_health_cfg_t c = *out;
    int32_t idx;

    if (root == NULL || root->type != LG_JSON_OBJECT) {
        return cfg_fail(err, errlen, "the top level must be an object", NULL);
    }
    for (idx = root->first_child; idx >= 0; idx = doc->nodes[idx].next) {
        const lg_json_node_t *n = &doc->nodes[idx];
        const char *key = n->key >= 0 ? &doc->strs[n->key] : "";
        const cfg_key_t *k = NULL;
        size_t i;
        int64_t v;

        for (i = 0; i < sizeof g_cfg_keys / sizeof g_cfg_keys[0]; i++) {
            if (strcmp(g_cfg_keys[i].key, key) == 0) {
                k = &g_cfg_keys[i];
                break;
            }
        }
        if (k == NULL) {
            return cfg_fail(err, errlen, "unknown key", key);
        }
        if (n->type != LG_JSON_NUMBER) {
            return cfg_fail(err, errlen, "not a number:", key);
        }
        if (n->milli < k->min * 1000 || n->milli > k->max * 1000) {
            return cfg_fail(err, errlen, "out of range:", key);
        }
        if (k->kind != 2 && n->milli % 1000 != 0) {
            return cfg_fail(err, errlen, "must be a whole number:", key);
        }
        v = k->kind == 2 ? n->milli : n->milli / 1000;
        if (k->kind == 0) {
            *(uint32_t *)(void *)((char *)&c + k->off) = (uint32_t)v;
        } else if (k->kind == 3) {
            *(uint8_t *)(void *)((char *)&c + k->off) = (uint8_t)v;
        } else {
            *(int32_t *)(void *)((char *)&c + k->off) = (int32_t)v;
        }
    }
    if (c.poll_fast_ms > c.poll_ms || c.noise_fail_milli <= c.noise_base_milli + c.noise_learn_max_milli ||
        c.dropout_warn_pm >= c.dropout_fail_pm || c.delay_zero_ms >= c.delay_fail_ms ||
        !(c.battery_empty_mv < c.battery_fail_mv && c.battery_fail_mv < c.battery_warn_mv &&
          c.battery_warn_mv < c.battery_full_mv) ||
        c.battery_tte_fail_s >= c.battery_tte_warn_s) {
        return cfg_fail(err, errlen, "inconsistent values (order of warn and fail thresholds)", NULL);
    }
    *out = c;
    return 0;
}
