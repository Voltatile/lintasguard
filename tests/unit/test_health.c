/*
 * core/health.c (README 8.3, Step 11). The sensor cases drive the real sensor model (lg_sensor_model)
 * and its statistics accumulator at 10 ms and read them like lg-health does, every 200 ms (50 ms
 * while not OK). Seeds are fixed and printed.
 */
#include <stdio.h>
#include <string.h>

#include "lg_health.h"
#include "lg_sensor_model.h"
#include "lg_test.h"

#define TRAIN_EVERY_MS 60000u /* a train over the sensors for 8 s every minute */
#define TRAIN_LEN_MS 8000u

static uint32_t seed_of(int comp) {
    return (uint32_t)(comp + 1) * 2654435761u; /* the lg-sensor default */
}

typedef struct {
    lg_sensor_model_t m;
    lg_sensor_acc_t acc;
    uint32_t next_poll_ms;
} sim_sensor_t;

static void sim_init(sim_sensor_t *s, int comp, uint32_t now_ms) {
    memset(s, 0, sizeof *s);
    lg_sensor_model_init(&s->m, seed_of(comp), now_ms);
}

static int train_on(uint32_t t_ms) {
    return (t_ms % TRAIN_EVERY_MS) >= 20000u && (t_ms % TRAIN_EVERY_MS) < 20000u + TRAIN_LEN_MS;
}

static lg_health_sensor_in_t in_of(const lg_sensor_acc_t *a) {
    lg_health_sensor_in_t in;

    in.ticks = a->ticks;
    in.no_sample = a->no_sample;
    in.blind = a->blind;
    in.dev_count = a->dev_count;
    in.dev_sum = a->dev_sum;
    in.rise_count = a->rise_count;
    in.last_rise_ms = a->last_rise_ms;
    in.last_fall_ms = a->last_fall_ms;
    return in;
}

/* One 10 ms tick of a sensor, then a health read when it is due. */
static void sim_tick(lg_health_t *h, sim_sensor_t *s, int comp, uint32_t t_ms, int truth) {
    lg_sensor_out_t out;
    int produced = lg_sensor_model_step(&s->m, t_ms, (uint8_t)truth, 0, &out);

    lg_sensor_acc_step(&s->acc, t_ms, produced, &out);
    if ((int32_t)(t_ms - s->next_poll_ms) >= 0) {
        lg_health_sensor_in_t in = in_of(&s->acc);

        (void)lg_health_on_sensor(h, (uint8_t)comp, &in, t_ms);
        s->next_poll_ms = t_ms + lg_health_poll_period_ms(h, (uint8_t)comp);
    }
}

/* A healthy sensor for one simulated hour with a train every minute: never anything but OK. */
static void test_no_false_alarm(void) {
    static lg_health_t h;
    static sim_sensor_t s[LG_COMP_SENSOR_LAST + 1];
    lg_health_cfg_t cfg = lg_health_cfg_default();
    int comp;
    uint32_t t;
    uint32_t not_ok = 0;
    uint32_t alarms = 0;

    lg_health_init(&h, &cfg, 0);
    for (comp = 0; comp <= LG_COMP_SENSOR_LAST; comp++) {
        sim_init(&s[comp], comp, 0);
        printf("seed %s 0x%08x\n", lg_health_comp_name((uint8_t)comp), (unsigned)seed_of(comp));
    }
    for (t = 0; t < 3600000u; t += LG_SENSOR_PERIOD_MS) {
        for (comp = 0; comp <= LG_COMP_SENSOR_LAST; comp++) {
            sim_tick(&h, &s[comp], comp, t, train_on(t));
            if (lg_health_get(&h, (uint8_t)comp).verdict != LG_VERDICT_OK) {
                not_ok++;
            }
            alarms += h.c[comp].cusum_alarm;
        }
    }
    CHECK(not_ok == 0);
    CHECK(alarms == 0);
    /* The learned baseline sits at the base noise. */
    CHECK(h.c[0].noise_base_milli >= LG_SENSOR_BASE_NOISE * 1000 - 500);
    CHECK(h.c[0].noise_base_milli <= LG_SENSOR_BASE_NOISE * 1000 + 500);
    CHECK(h.c[0].learning == 0);
    CHECK(h.c[0].delay_samples > 0); /* the pairs were matched on every train */
    CHECK(h.c[0].disagreements == 0);
    printf("healthy hour: not_ok=%u alarms=%u base=%d noise_ewma=%d delay=%u\n", (unsigned)not_ok,
           (unsigned)alarms, (int)h.c[0].noise_base_milli, (int)h.c[0].noise_ewma_milli,
           (unsigned)h.c[0].delay_ewma_ms);
}

typedef struct {
    uint32_t t_warn;
    uint32_t t_fail;
    uint16_t diag_warn;
    int32_t trend_warn;
    uint32_t partner_not_ok;
} drift_result_t;

/* DRIFT(rate) on west/a at 30 s: when do WARNING and FAILED come? */
static drift_result_t run_drift(uint32_t rate) {
    static lg_health_t h;
    static sim_sensor_t a, b;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    drift_result_t r = {0, 0, 0, -1, 0};
    uint32_t t;

    lg_health_init(&h, &cfg, 0);
    sim_init(&a, 0, 0);
    sim_init(&b, 1, 0);
    for (t = 0; t < 200000u; t += LG_SENSOR_PERIOD_MS) {
        lg_health_out_t o;

        if (t == 30000u) {
            CHECK(lg_sensor_model_set_fault(&a.m, LG_SFAULT_DRIFT, rate, t) == 0);
        }
        sim_tick(&h, &a, 0, t, 0);
        sim_tick(&h, &b, 1, t, 0);
        o = lg_health_get(&h, 0);
        if (r.t_warn == 0 && o.verdict == LG_VERDICT_WARNING) {
            r.t_warn = t;
            r.diag_warn = o.diag;
            r.trend_warn = o.trend_s;
        }
        if (r.t_fail == 0 && o.verdict == LG_VERDICT_FAILED) {
            r.t_fail = t;
            CHECK(o.diag == LG_DIAG_NOISE_HIGH);
        }
        if (lg_health_get(&h, 1).verdict != LG_VERDICT_OK) {
            r.partner_not_ok++; /* the partner must not be blamed */
        }
    }
    CHECK(r.partner_not_ok == 0);
    return r;
}

static void test_drift_warns_before_failure(void) {
    uint32_t rates[] = {1, 2, 5};
    unsigned i;

    for (i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        drift_result_t r = run_drift(rates[i]);

        printf("drift %u/s: warning at %u ms, failed at %u ms, lead %u ms, diag %s, trend %d s\n",
               (unsigned)rates[i], (unsigned)r.t_warn, (unsigned)r.t_fail, (unsigned)(r.t_fail - r.t_warn),
               lg_health_diag_name(r.diag_warn), (int)r.trend_warn);
        CHECK(r.t_warn > 30000u);
        CHECK(r.t_fail > r.t_warn);
        CHECK(r.diag_warn == LG_DIAG_NOISE_RISING);
        CHECK(r.trend_warn > 0);
        /* FAILED by the noise threshold: (30 - 3) / rate seconds after the start, plus EWMA lag. */
        CHECK(r.t_fail <= 30000u + 27000u / rates[i] + 3000u);
        /* The warning comes early enough to matter. */
        CHECK(r.t_warn - 30000u <= 27000u / rates[i] / 2u);
    }
}

/* After the fault is cleared the sensor returns to OK, not before the hold time. */
static void test_drift_recovers(void) {
    static lg_health_t h;
    static sim_sensor_t a;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    uint32_t t;
    uint32_t t_ok = 0;

    lg_health_init(&h, &cfg, 0);
    sim_init(&a, 0, 0);
    for (t = 0; t < 120000u; t += LG_SENSOR_PERIOD_MS) {
        if (t == 20000u) {
            (void)lg_sensor_model_set_fault(&a.m, LG_SFAULT_DRIFT, 2, t);
        }
        if (t == 50000u) {
            CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_FAILED);
            (void)lg_sensor_model_set_fault(&a.m, LG_SFAULT_NONE, 0, t);
        }
        sim_tick(&h, &a, 0, t, 0);
        if (t > 50000u && t_ok == 0 && lg_health_get(&h, 0).verdict == LG_VERDICT_OK) {
            t_ok = t;
        }
    }
    printf("recovered to OK %u ms after the fault was cleared\n", (unsigned)(t_ok - 50000u));
    CHECK(t_ok >= 50000u + LG_HEALTH_OK_HOLD_MS);
    CHECK(t_ok <= 50000u + 20000u);
}

static void test_dropouts(void) {
    static lg_health_t h;
    static sim_sensor_t a, b;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    uint32_t t;

    lg_health_init(&h, &cfg, 0);
    sim_init(&a, 0, 0);
    sim_init(&b, 1, 0);
    (void)lg_sensor_model_set_fault(&a.m, LG_SFAULT_DROPOUT, 10, 0);
    (void)lg_sensor_model_set_fault(&b.m, LG_SFAULT_DROPOUT, 50, 0);
    for (t = 0; t < 30000u; t += LG_SENSOR_PERIOD_MS) {
        sim_tick(&h, &a, 0, t, 0);
        sim_tick(&h, &b, 1, t, 0);
    }
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_WARNING);
    CHECK(lg_health_get(&h, 0).diag == LG_DIAG_DROPOUTS);
    CHECK(lg_health_get(&h, 1).verdict == LG_VERDICT_FAILED);
    CHECK(lg_health_get(&h, 1).diag == LG_DIAG_DROPOUTS);
}

/* Blind samples (quality 0, the plant does not answer) count as dropouts: the Step 9 known limit of an
 * island blind 190 ms out of every 200 ms. */
static void test_blind_counts_as_dropout(void) {
    static lg_health_t h;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    lg_sensor_acc_t acc;
    uint32_t t;
    uint8_t island = 2;

    memset(&acc, 0, sizeof acc);
    lg_health_init(&h, &cfg, 0);
    for (t = 0; t < 20000u; t += LG_SENSOR_PERIOD_MS) {
        lg_sensor_out_t out = {0, 97, 0, LG_SENSOR_CLEAR_LEVEL};

        if ((t % 200u) < 190u) {
            out.quality = 0;
        }
        lg_sensor_acc_step(&acc, t, 1, &out);
        if (t % 200u == 0) {
            lg_health_sensor_in_t in = in_of(&acc);

            (void)lg_health_on_sensor(&h, island, &in, t);
        }
    }
    CHECK(lg_health_get(&h, island).verdict == LG_VERDICT_FAILED);
    CHECK(lg_health_get(&h, island).diag == LG_DIAG_DROPOUTS);
}

/* Pairs: a sensor that rises later than its partner (DELAY), and one that misses trains. */
static void run_pair(uint8_t fault_b, uint32_t param_b, uint32_t clear_at_ms, uint32_t end_ms,
                     lg_health_out_t *oa, lg_health_out_t *ob, uint32_t *b_not_ok) {
    static lg_health_t h;
    static sim_sensor_t a, b;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    uint32_t t;

    lg_health_init(&h, &cfg, 0);
    sim_init(&a, 0, 0);
    sim_init(&b, 1, 0);
    *b_not_ok = 0;
    (void)lg_sensor_model_set_fault(&b.m, fault_b, param_b, 0);
    for (t = 0; t < end_ms; t += LG_SENSOR_PERIOD_MS) {
        if (t == clear_at_ms) {
            (void)lg_sensor_model_set_fault(&b.m, LG_SFAULT_NONE, 0, t);
        }
        sim_tick(&h, &a, 0, t, train_on(t));
        sim_tick(&h, &b, 1, t, train_on(t));
        if (lg_health_get(&h, 1).verdict != LG_VERDICT_OK) {
            (*b_not_ok)++;
        }
    }
    *oa = lg_health_get(&h, 0);
    *ob = lg_health_get(&h, 1);
}

static void test_response_delay(void) {
    lg_health_out_t oa, ob;
    uint32_t nok;

    run_pair(LG_SFAULT_DELAY, 300, 0xFFFFFFFFu, 200000u, &oa, &ob, &nok);
    CHECK(oa.verdict == LG_VERDICT_OK);
    CHECK(ob.verdict == LG_VERDICT_WARNING);
    CHECK(ob.diag == LG_DIAG_RESPONSE_DELAY);
    run_pair(LG_SFAULT_DELAY, 600, 0xFFFFFFFFu, 200000u, &oa, &ob, &nok);
    CHECK(oa.verdict == LG_VERDICT_OK);
    CHECK(ob.verdict == LG_VERDICT_FAILED);
}

static void test_disagreement(void) {
    lg_health_out_t oa, ob;
    uint32_t nok;

    /* B stuck clear: it misses every train. */
    run_pair(LG_SFAULT_STUCK_CLEAR, 0, 0xFFFFFFFFu, 150000u, &oa, &ob, &nok);
    CHECK(oa.verdict == LG_VERDICT_OK);
    CHECK(ob.verdict == LG_VERDICT_WARNING);
    CHECK(ob.diag == LG_DIAG_DISAGREEMENT);
    /* Cleared at 150 s: three agreeing trains later (210, 270, 330 s) it is OK again. */
    run_pair(LG_SFAULT_STUCK_CLEAR, 0, 150000u, 400000u, &oa, &ob, &nok);
    CHECK(ob.verdict == LG_VERDICT_OK);
}

/* Gate: one movement per call. */
static uint32_t g_moves;
static uint32_t g_t;

static lg_health_out_t gate_move(lg_health_t *h, uint32_t travel_pct, uint32_t current_pct) {
    lg_health_gate_in_t in;

    g_moves++;
    g_t += 15000u;
    in.moves_done = g_moves;
    in.last_travel_ms = LG_GATE_TRAVEL_MS * travel_pct / 100u;
    in.last_peak_ma = LG_GATE_CURRENT_MOVING_MA * current_pct / 100u;
    in.battery_mv = LG_BATTERY_FULL_MV;
    lg_health_on_gate(h, &in, g_t);
    return lg_health_get(h, LG_COMP_GATE);
}

static void gate_reset(lg_health_t *h) {
    lg_health_cfg_t cfg = lg_health_cfg_default();
    lg_health_gate_in_t in = {0, 0, 0, LG_BATTERY_FULL_MV};

    g_moves = 0;
    g_t = 0;
    lg_health_init(h, &cfg, 0);
    lg_health_on_gate(h, &in, 0);
}

static void test_gate_slow(void) {
    static lg_health_t h;
    lg_health_out_t o;
    uint32_t pct;
    uint32_t warn_at = 0, fail_at = 0;
    int i;

    gate_reset(&h);
    for (i = 0; i < 10; i++) {
        o = gate_move(&h, 100, 100);
        CHECK(o.verdict == LG_VERDICT_OK);
    }
    /* 125 %: the CUSUM sees it within two movements (one train). */
    (void)gate_move(&h, 125, 100);
    o = gate_move(&h, 125, 100);
    CHECK(o.verdict == LG_VERDICT_WARNING);
    CHECK(o.diag == LG_DIAG_GATE_SLOW);
    CHECK(o.trend_s == -1 || o.trend_s > 0);

    /* One movement at the failure ratio is FAILED at once. */
    gate_reset(&h);
    o = gate_move(&h, 100, 100);
    o = gate_move(&h, LG_HEALTH_GATE_TRAVEL_FAIL_PCT, 100);
    CHECK(o.verdict == LG_VERDICT_FAILED);

    /* A ramp of mechanical wear: WARNING long before FAILED. */
    gate_reset(&h);
    for (pct = 100; pct <= 200 && fail_at == 0; pct += 5) {
        o = gate_move(&h, pct, 100);
        o = gate_move(&h, pct, 100);
        if (warn_at == 0 && o.verdict == LG_VERDICT_WARNING) {
            warn_at = pct;
        }
        if (o.verdict == LG_VERDICT_FAILED) {
            fail_at = pct;
        }
    }
    printf("gate ramp: warning at %u %%, failed at %u %%\n", (unsigned)warn_at, (unsigned)fail_at);
    CHECK(warn_at > 0 && warn_at <= 115);
    CHECK(fail_at > warn_at);
    CHECK(fail_at <= LG_HEALTH_GATE_TRAVEL_FAIL_PCT);
}

static void test_gate_current(void) {
    static lg_health_t h;
    lg_health_out_t o;
    int i;

    gate_reset(&h);
    for (i = 0; i < 4; i++) {
        o = gate_move(&h, 100, 150);
    }
    CHECK(o.verdict == LG_VERDICT_WARNING);
    CHECK(o.diag == LG_DIAG_GATE_HIGH_CURRENT);
    o = gate_move(&h, 100, LG_HEALTH_GATE_CURRENT_FAIL_PCT + 10);
    CHECK(o.verdict == LG_VERDICT_FAILED);
}

static void test_gate_without_movement_stays(void) {
    static lg_health_t h;
    lg_health_gate_in_t in = {0, 0, 0, LG_BATTERY_FULL_MV};
    uint32_t t;

    gate_reset(&h);
    (void)gate_move(&h, 100, 100);
    (void)gate_move(&h, LG_HEALTH_GATE_TRAVEL_FAIL_PCT + 5, 100);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_FAILED);
    /* No movement (level 4 holds the gate down): the verdict stays until a reset. */
    in.moves_done = g_moves;
    in.last_travel_ms = LG_GATE_TRAVEL_MS * (LG_HEALTH_GATE_TRAVEL_FAIL_PCT + 5) / 100u;
    in.last_peak_ma = LG_GATE_CURRENT_MOVING_MA;
    for (t = g_t; t < g_t + 60000u; t += 200u) {
        lg_health_on_gate(&h, &in, t);
    }
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_FAILED);
    lg_health_reset(&h, LG_COMP_GATE, t);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_OK);
    lg_health_on_gate(&h, &in, t + 200u); /* the old movement is not judged again */
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_OK);
}

/* Battery: draining at 5 mV/s from full gives a time to empty of about 540 s: WARNING; low: FAILED. */
static void test_battery(void) {
    static lg_health_t h;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    lg_health_gate_in_t in = {0, 0, 0, LG_BATTERY_FULL_MV};
    uint32_t t;
    uint32_t t_warn = 0, t_fail = 0;
    int32_t mv = LG_BATTERY_FULL_MV;

    lg_health_init(&h, &cfg, 0);
    for (t = 0; t < 60000u; t += 200u) {
        lg_health_on_gate(&h, &in, t);
    }
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).verdict == LG_VERDICT_OK);
    for (; t < 900000u; t += 200u) {
        lg_health_out_t o;

        mv -= 1; /* 5 mV/s */
        in.battery_mv = (uint32_t)(mv < 0 ? 0 : mv);
        lg_health_on_gate(&h, &in, t);
        o = lg_health_get(&h, LG_COMP_BATTERY);
        if (t_warn == 0 && o.verdict == LG_VERDICT_WARNING) {
            t_warn = t;
            CHECK(o.diag == LG_DIAG_BATTERY_DRAIN);
            CHECK(o.trend_s > 0);
        }
        if (t_fail == 0 && o.verdict == LG_VERDICT_FAILED) {
            t_fail = t;
        }
    }
    printf("battery 5 mV/s: warning at %u ms, failed at %u ms\n", (unsigned)t_warn, (unsigned)t_fail);
    CHECK(t_warn > 60000u && t_warn < 120000u);
    CHECK(t_fail > t_warn);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_OK); /* the gate itself is fine */
}

/* A restarted sensor process starts its counters again: no spike, no verdict change. */
static void test_counter_restart_and_wrap(void) {
    static lg_health_t h;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    lg_health_sensor_in_t in;
    uint32_t t;

    lg_health_init(&h, &cfg, 0);
    memset(&in, 0, sizeof in);
    in.ticks = 0xFFFFFF00u; /* near the wrap */
    in.dev_count = 0xFFFFFF00u;
    in.dev_sum = 0xFFFFFFFFFFFF0000ull;
    for (t = 0; t < 30000u; t += 200u) {
        in.ticks += 20;
        in.dev_count += 20;
        in.dev_sum += 20u * 12u; /* noise 3 units */
        (void)lg_health_on_sensor(&h, 0, &in, t);
        CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_OK);
    }
    CHECK(h.c[0].noise_ewma_milli > 2500 && h.c[0].noise_ewma_milli < 3500);
    memset(&in, 0, sizeof in); /* restart */
    for (; t < 40000u; t += 200u) {
        in.ticks += 20;
        in.dev_count += 20;
        in.dev_sum += 20u * 12u;
        (void)lg_health_on_sensor(&h, 0, &in, t);
        CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_OK);
    }
    CHECK(lg_health_on_sensor(&h, LG_COMP_GATE, &in, t) == -1);
}

/* A late read (lg-health held up for 1.5 s) covers a longer window, nothing else. */
static void test_late_read(void) {
    static lg_health_t h;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    lg_health_sensor_in_t in;
    uint32_t t;

    lg_health_init(&h, &cfg, 0);
    memset(&in, 0, sizeof in);
    for (t = 0; t < 60000u; t += 200u) {
        uint32_t step = (t % 10000u == 0 && t > 0) ? 150u : 20u; /* every 10 s a 1.5 s gap */

        in.ticks += step;
        in.dev_count += step;
        in.dev_sum += step * 12u;
        if (step > 20u) {
            t += 1300u;
        }
        (void)lg_health_on_sensor(&h, 0, &in, t);
        CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_OK);
    }
}

static void test_no_data(void) {
    static lg_health_t h;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    int i;

    lg_health_init(&h, &cfg, 0);
    for (i = 0; i < LG_HEALTH_NODATA_POLLS - 1; i++) {
        lg_health_on_no_data(&h, 3, (uint32_t)i * 200u);
    }
    CHECK(lg_health_get(&h, 3).verdict == LG_VERDICT_OK);
    lg_health_on_no_data(&h, 3, 1000u);
    CHECK(lg_health_get(&h, 3).verdict == LG_VERDICT_WARNING);
    CHECK(lg_health_get(&h, 3).diag == LG_DIAG_NO_DATA);
    lg_health_on_no_data(&h, LG_COMP_GATE, 0);
    lg_health_on_no_data(&h, LG_COMP_GATE, 200);
    lg_health_on_no_data(&h, LG_COMP_GATE, 400);
    CHECK(lg_health_get(&h, LG_COMP_GATE).diag == LG_DIAG_NO_DATA);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).diag == LG_DIAG_NO_DATA);
    lg_health_on_no_data(&h, 200, 0); /* ignored */
}

static void test_poll_period_and_names(void) {
    static lg_health_t h;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    uint16_t d;
    uint8_t c;
    static char buf[2048];
    size_t n;

    lg_health_init(&h, &cfg, 0);
    CHECK(lg_health_poll_period_ms(&h, 0) == LG_HEALTH_POLL_MS);
    h.c[0].verdict = LG_VERDICT_WARNING;
    CHECK(lg_health_poll_period_ms(&h, 0) == LG_HEALTH_POLL_FAST_MS);
    for (d = 0; d < LG_DIAG_COUNT; d++) {
        CHECK(lg_health_diag_name(d) != NULL && lg_health_diag_name(d)[0] != '?');
        CHECK(lg_health_diag_text(d) != NULL && strlen(lg_health_diag_text(d)) < 60);
    }
    CHECK(strcmp(lg_health_diag_name(999), "UNKNOWN") == 0);
    for (c = 0; c < LG_COMP_COUNT; c++) {
        CHECK(lg_health_comp_name(c)[0] != '?');
    }
    CHECK(strcmp(lg_health_comp_name(0), "west/a") == 0);
    CHECK(strcmp(lg_health_comp_name(5), "east/island") == 0);
    CHECK(strcmp(lg_health_comp_name(LG_COMP_COUNT), "?") == 0);
    n = lg_health_text(&h, buf, sizeof buf);
    CHECK(n > 0 && n < sizeof buf && strstr(buf, "comp=west/a") != NULL && strstr(buf, "comp=battery") != NULL);
    n = lg_health_text(&h, buf, 10);
    CHECK(n < 10 && buf[n] == '\0');
}

static void test_verdict_map(void) {
    CHECK(lg_health_verdict_from_score(100) == LG_VERDICT_OK);
    CHECK(lg_health_verdict_from_score(69) == LG_VERDICT_WARNING);
    CHECK(lg_health_verdict_from_score(39) == LG_VERDICT_FAILED);
}

/*
 * Review L1: the hold before a better verdict restarts when the better target changes. FAILED (all samples
 * missing), then clean readings 100 ms apart: the dropout EWMA falls through the WARNING band in about
 * 1.2 s and reaches the OK band; the OK target must itself be held for ok_hold_ms (the old code counted
 * the hold from the start of the WARNING target and turned OK 0.9 s after the OK target appeared).
 */
static void test_hold_restarts_for_a_new_target(void) {
    static lg_health_t h;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    lg_health_sensor_in_t in;
    uint32_t t = 0;
    uint32_t ok_target_at = 0;
    int i;

    lg_health_init(&h, &cfg, 0);
    memset(&in, 0, sizeof in);
    for (i = 0; i < 20; i++, t += 200) { /* every tick without a sample */
        in.ticks += 20;
        in.no_sample += 20;
        (void)lg_health_on_sensor(&h, 2, &in, t);
    }
    CHECK(lg_health_get(&h, 2).verdict == LG_VERDICT_FAILED);
    for (i = 0; i < 40 && ok_target_at == 0; i++, t += 100) { /* clean, fast readings */
        in.ticks += 20;
        in.dev_count += 20;
        in.dev_sum += 240;
        (void)lg_health_on_sensor(&h, 2, &in, t);
        if (h.c[2].better_tracking && h.c[2].better_target == LG_VERDICT_OK) {
            ok_target_at = t;
        }
    }
    CHECK(ok_target_at != 0);
    CHECK(lg_health_get(&h, 2).verdict == LG_VERDICT_FAILED); /* the WARNING band was crossed in < hold */
    /* 0.9 s later the OK target has not been held long enough. */
    in.ticks += 20;
    in.dev_count += 20;
    in.dev_sum += 240;
    (void)lg_health_on_sensor(&h, 2, &in, ok_target_at + 900);
    CHECK(lg_health_get(&h, 2).verdict != LG_VERDICT_OK);
    /* After the full hold it is OK. */
    in.ticks += 20;
    in.dev_count += 20;
    in.dev_sum += 240;
    (void)lg_health_on_sensor(&h, 2, &in, ok_target_at + LG_HEALTH_OK_HOLD_MS);
    CHECK(lg_health_get(&h, 2).verdict == LG_VERDICT_OK);
}

/* Review M1: "assessed" says whether a verdict rests on data. */
static void test_assessed_flag(void) {
    static lg_health_t h;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    lg_health_sensor_in_t in;
    lg_health_gate_in_t gin = {0, 0, 0, LG_BATTERY_FULL_MV};
    uint8_t k;

    lg_health_init(&h, &cfg, 0);
    for (k = 0; k < LG_COMP_COUNT; k++) {
        CHECK(lg_health_get(&h, k).verdict == LG_VERDICT_OK && lg_health_get(&h, k).assessed == 0);
    }
    memset(&in, 0, sizeof in);
    (void)lg_health_on_sensor(&h, 0, &in, 0); /* reference only */
    CHECK(lg_health_get(&h, 0).assessed == 0);
    in.ticks = 20;
    in.dev_count = 20;
    in.dev_sum = 240;
    (void)lg_health_on_sensor(&h, 0, &in, 200);
    CHECK(lg_health_get(&h, 0).assessed == 1);
    lg_health_on_gate(&h, &gin, 0); /* battery read, no movement yet */
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).assessed == 1);
    CHECK(lg_health_get(&h, LG_COMP_GATE).assessed == 0);
    gin.moves_done = 1;
    gin.last_travel_ms = LG_GATE_TRAVEL_MS;
    gin.last_peak_ma = LG_GATE_CURRENT_MOVING_MA;
    lg_health_on_gate(&h, &gin, 200);
    CHECK(lg_health_get(&h, LG_COMP_GATE).assessed == 1);
    lg_health_init(&h, &cfg, 0);
    lg_health_reset(&h, LG_COMP_GATE, 0); /* the technician's word */
    CHECK(lg_health_get(&h, LG_COMP_GATE).assessed == 1 && lg_health_get(&h, LG_COMP_BATTERY).assessed == 1);
    lg_health_on_no_data(&h, 3, 0);
    lg_health_on_no_data(&h, 3, 200);
    lg_health_on_no_data(&h, 3, 400);
    CHECK(lg_health_get(&h, 3).assessed == 1); /* NO_DATA is a judgement */
}

int main(void) {
    test_assessed_flag();
    test_hold_restarts_for_a_new_target();
    test_verdict_map();
    test_no_false_alarm();
    test_drift_warns_before_failure();
    test_drift_recovers();
    test_dropouts();
    test_blind_counts_as_dropout();
    test_response_delay();
    test_disagreement();
    test_gate_slow();
    test_gate_current();
    test_gate_without_movement_stays();
    test_battery();
    test_counter_restart_and_wrap();
    test_late_read();
    test_no_data();
    test_poll_period_and_names();
    LG_TEST_DONE("test_health");
}
