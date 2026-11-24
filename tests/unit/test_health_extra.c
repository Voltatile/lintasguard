/*
 * core/health.c, extra cases (Step 11): counter restarts per field, combined causes, the pair logic
 * (delay EWMA, far-apart rises, clock wrap, accusation rule), gate restarts, hysteresis steps, NO_DATA
 * through the gate, resets, threshold file edges and the status text. Counters are synthetic here, so
 * every expected number follows from the arithmetic in health.c and none depends on a random seed.
 */
#include <stdio.h>
#include <string.h>

#include "lg_health.h"
#include "lg_json_mini.h"
#include "lg_sensor_model.h"
#include "lg_test.h"

/* Synthetic sensor: one read adds `ticks` ticks, `dns` of them without a sample. */
typedef struct {
    lg_health_sensor_in_t in;
} syn_t;

static void syn_read(lg_health_t *h, uint8_t comp, syn_t *s, uint32_t ticks, uint32_t dns, uint32_t dev_per_sample,
                     uint32_t now_ms) {
    uint32_t samples = ticks - dns;

    s->in.ticks += ticks;
    s->in.no_sample += dns;
    s->in.dev_count += samples;
    s->in.dev_sum += (uint64_t)samples * dev_per_sample;
    (void)lg_health_on_sensor(h, comp, &s->in, now_ms);
}

static void syn_rise(lg_health_t *h, uint8_t comp, syn_t *s, uint32_t rise_ms, uint32_t now_ms) {
    s->in.rise_count++;
    s->in.last_rise_ms = rise_ms;
    syn_read(h, comp, s, 20, 0, 12, now_ms);
}

/* The end of a run: the sensor reads clear again at fall_ms. */
static void syn_fall(lg_health_t *h, uint8_t comp, syn_t *s, uint32_t fall_ms, uint32_t now_ms) {
    s->in.last_fall_ms = fall_ms;
    syn_read(h, comp, s, 20, 0, 12, now_ms);
}

static void fresh(lg_health_t *h, uint32_t now_ms) {
    lg_health_cfg_t cfg = lg_health_cfg_default();

    lg_health_init(h, &cfg, now_ms);
}

/* ---- Counter restart, one field at a time ----------------------------------- */

static void test_restart_each_field(void) {
    static lg_health_t h;
    int field;

    for (field = 0; field < 6; field++) {
        syn_t s;
        uint32_t t;
        int32_t noise_before;

        fresh(&h, 0);
        memset(&s, 0, sizeof s);
        s.in.ticks = 5000000u;
        s.in.no_sample = 1000;
        s.in.blind = 1000;
        s.in.dev_count = 5000000u;
        s.in.dev_sum = 12ull * 5000000u;
        s.in.rise_count = 1000;
        for (t = 0; t < 15000u; t += 200u) {
            syn_read(&h, 0, &s, 20, 0, 12, t);
        }
        noise_before = h.c[0].noise_ewma_milli;
        CHECK(noise_before == 3000);
        CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_OK);

        /* One reading in which exactly one counter went backwards by one. */
        switch (field) {
        case 0: s.in.ticks -= 1; break;
        case 1: s.in.no_sample -= 1; break;
        case 2: s.in.blind -= 1; break;
        case 3: s.in.dev_count -= 1; break;
        case 4: s.in.dev_sum -= 1; break;
        default: s.in.rise_count -= 1; break;
        }
        s.in.ticks += (field == 0) ? 0 : 20;
        (void)lg_health_on_sensor(&h, 0, &s.in, t);
        t += 200;
        /* That reading only sets the reference: no dropout, no noise step, no verdict change. */
        CHECK(h.c[0].noise_ewma_milli == noise_before);
        CHECK(h.c[0].drop_ewma_pm == 0);
        CHECK(h.c[0].rise_pending == 0);
        CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_OK);
        /* And the next normal reading is used again (deltas are taken from the new reference). */
        syn_read(&h, 0, &s, 20, 0, 12, t);
        CHECK(h.c[0].p_ticks == s.in.ticks);
        CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_OK);
        if (field != 0) {
            CHECK(h.c[0].have_prev == 1);
        }
    }
}

/* A restart is not hidden when the new counters are larger than the old ones: only a jump over the
 * plausibility limits is seen as a restart. A huge forward jump of ticks is a restart too. */
static void test_restart_forward_jump(void) {
    static lg_health_t h;
    syn_t s;
    uint32_t t;

    fresh(&h, 0);
    memset(&s, 0, sizeof s);
    for (t = 0; t < 12000u; t += 200u) {
        syn_read(&h, 0, &s, 20, 0, 12, t);
    }
    s.in.ticks += 2000000u; /* more than LG_HEALTH_MAX_TICKS_PER_READ */
    s.in.dev_count += 2000000u;
    s.in.dev_sum += 24000000ull;
    (void)lg_health_on_sensor(&h, 0, &s.in, t);
    CHECK(h.c[0].noise_ewma_milli == 3000);
    CHECK(h.c[0].drop_ewma_pm == 0);
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_OK);
}

/* The sensor did not tick between two reads: nothing is added, the score is just recomputed. */
static void test_no_new_ticks(void) {
    static lg_health_t h;
    syn_t s;
    uint32_t t;
    uint32_t drop;

    fresh(&h, 0);
    memset(&s, 0, sizeof s);
    for (t = 0; t < 3000u; t += 200u) {
        syn_read(&h, 0, &s, 20, 5, 12, t);
    }
    drop = h.c[0].drop_ewma_pm;
    CHECK(drop > 0);
    (void)lg_health_on_sensor(&h, 0, &s.in, t);
    (void)lg_health_on_sensor(&h, 0, &s.in, t + 200u);
    CHECK(h.c[0].drop_ewma_pm == drop);
    CHECK(lg_health_on_sensor(&h, 0, NULL, t) == -1);
    CHECK(lg_health_on_sensor(&h, LG_COMP_BATTERY, &s.in, t) == -1);
    CHECK(lg_health_on_sensor(&h, 200, &s.in, t) == -1);
}

/* ---- Which cause wins ------------------------------------------------------- */

/* 12 s learning at noise 3, then the phase under test for 10 s. dev_per_sample 12 = 3 units, 120 = 30. */
static void phase(lg_health_t *h, syn_t *s, uint32_t *t, uint32_t dns, uint32_t dev, uint32_t ms) {
    uint32_t end = *t + ms;

    for (; *t < end; *t += 200u) {
        syn_read(h, 0, s, 100, dns, dev, *t);
    }
}

static void test_combined_causes(void) {
    static lg_health_t h;
    syn_t s;
    uint32_t t = 0;

    /* Dropouts alone (10 %): DROPOUTS. */
    fresh(&h, 0);
    memset(&s, 0, sizeof s);
    phase(&h, &s, &t, 0, 12, 12000);
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_OK);
    phase(&h, &s, &t, 10, 12, 6000);
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_WARNING);
    CHECK(lg_health_get(&h, 0).diag == LG_DIAG_DROPOUTS);

    /* Noise at the failure level and 6 % dropouts: the noise is the lower score, NOISE_HIGH. */
    fresh(&h, 0);
    memset(&s, 0, sizeof s);
    t = 0;
    phase(&h, &s, &t, 0, 12, 12000);
    phase(&h, &s, &t, 6, 128, 20000); /* the EWMA creeps up to the target, aim above the limit */
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_FAILED);
    CHECK(lg_health_get(&h, 0).diag == LG_DIAG_NOISE_HIGH);

    /* Mild noise rise (warning range) and heavy dropouts (FAILED): DROPOUTS decides. */
    fresh(&h, 0);
    memset(&s, 0, sizeof s);
    t = 0;
    phase(&h, &s, &t, 0, 12, 12000);
    phase(&h, &s, &t, 50, 60, 20000); /* 5 units, 500 per mille */
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_FAILED);
    CHECK(lg_health_get(&h, 0).diag == LG_DIAG_DROPOUTS);
    /* A FAILED verdict has no trend for dropouts. */
    CHECK(lg_health_get(&h, 0).trend_s == -1);
}

/* A sensor that sends nothing at all: dropouts head for 1000 per mille, FAILED, and no noise (no
 * samples) is ever computed. */
static void test_silent_sensor_is_dropouts(void) {
    static lg_health_t h;
    syn_t s;
    uint32_t t;

    fresh(&h, 0);
    memset(&s, 0, sizeof s);
    for (t = 0; t < 5000u; t += 200u) {
        syn_read(&h, 0, &s, 20, 20, 0, t);
    }
    CHECK(h.c[0].have_noise == 0);
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_FAILED);
    CHECK(lg_health_get(&h, 0).diag == LG_DIAG_DROPOUTS);
    CHECK(h.c[0].drop_ewma_pm > 900);
}

/* ---- Pairs ------------------------------------------------------------------- */

static void pair_start(lg_health_t *h, syn_t *a, syn_t *b) {
    fresh(h, 0);
    memset(a, 0, sizeof *a);
    memset(b, 0, sizeof *b);
    syn_read(h, 0, a, 20, 0, 12, 0);
    syn_read(h, 1, b, 20, 0, 12, 0);
}

/* Both read in the same train: the later one carries the delay, EWMA 1/2 over trains. */
static void test_delay_ewma(void) {
    static lg_health_t h;
    syn_t a, b;
    uint32_t t = 1000;

    pair_start(&h, &a, &b);
    syn_rise(&h, 0, &a, 1000, t);
    CHECK(h.c[0].rise_pending == 1);
    t += 200;
    syn_rise(&h, 1, &b, 1300, t); /* 300 ms later */
    CHECK(h.c[0].rise_pending == 0 && h.c[1].rise_pending == 0);
    CHECK(h.c[1].delay_ewma_ms == 300 && h.c[1].delay_samples == 1);
    CHECK(h.c[0].delay_ewma_ms == 0 && h.c[0].delay_samples == 1);
    /* Score of 300 ms: zero 50, fail 400, warn 225: between warn and fail: WARNING, RESPONSE_DELAY. */
    CHECK(lg_health_get(&h, 1).verdict == LG_VERDICT_WARNING);
    CHECK(lg_health_get(&h, 1).diag == LG_DIAG_RESPONSE_DELAY);
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_OK);

    /* Second train, the roles swap: A is 100 ms late. B: ewma(300, 0) = 150, A: ewma(0, 100) = 50. */
    t += 20000;
    syn_rise(&h, 1, &b, t, t);
    t += 200;
    syn_rise(&h, 0, &a, t - 100, t);
    CHECK(h.c[1].delay_ewma_ms == 150);
    CHECK(h.c[0].delay_ewma_ms == 50);
    CHECK(h.c[0].delay_samples == 2 && h.c[1].delay_samples == 2);
    /* The partner's score was recomputed in the same call (150 ms: 100 - 30*100/175 = 82, still OK band
     * but the verdict only gets better after the hold time). */
    CHECK(lg_health_get(&h, 1).verdict == LG_VERDICT_WARNING);
}

/* An island sensor has no partner: a rise is not pending, nothing is judged. */
static void test_island_has_no_pair(void) {
    static lg_health_t h;
    syn_t s;

    fresh(&h, 0);
    memset(&s, 0, sizeof s);
    syn_read(&h, 2, &s, 20, 0, 12, 0);
    syn_rise(&h, 2, &s, 500, 200);
    CHECK(h.c[2].rise_pending == 0);
    CHECK(h.c[2].delay_samples == 0);
    CHECK(lg_health_get(&h, 2).verdict == LG_VERDICT_OK);
}

/*
 * Rises further apart than the pair window are two trains, each judged alone on its own completed run
 * (the rule of the controller's MISSED_TRAIN: a run of LG_T_DISAGREE_MS to LG_SENSOR_MAX_OCCUPIED_MS).
 */
static void test_pair_far_apart(void) {
    static lg_health_t h;
    syn_t a, b;

    pair_start(&h, &a, &b);
    syn_rise(&h, 0, &a, 1000, 1200);
    CHECK(h.c[0].rise_pending == 1);
    syn_fall(&h, 0, &a, 3000, 3200); /* a 2 s run */
    CHECK(h.c[0].rise_pending == 1);  /* judged 2 polls after the fall */
    syn_rise(&h, 1, &b, 5000, 5200);  /* B rises 4 s after A: another train */
    CHECK(h.c[0].rise_pending == 0);
    CHECK(h.c[1].disagree == 1 && h.c[1].disagreements == 1); /* A saw a train B did not */
    CHECK(lg_health_get(&h, 1).verdict == LG_VERDICT_WARNING);
    CHECK(lg_health_get(&h, 1).diag == LG_DIAG_DISAGREEMENT);
    CHECK(h.c[1].rise_pending == 1);
    syn_fall(&h, 1, &b, 7000, 7200);
    syn_read(&h, 1, &b, 20, 0, 12, 7600);
    CHECK(h.c[1].rise_pending == 0);
    /* B is itself WARNING (it was accused before), so its run proves nothing against A. */
    CHECK(h.c[0].disagree == 0 && h.c[0].disagreements == 0);
}

/* A sensor stuck occupied (a run that never ends) and a blip accuse nobody (found by the campaign). */
static void test_stuck_or_blip_accuses_nobody(void) {
    static lg_health_t h;
    syn_t a, b;
    uint32_t t;

    pair_start(&h, &a, &b);
    syn_rise(&h, 0, &a, 1000, 1200); /* A stuck occupied: no fall */
    for (t = 1400; t < 1000 + LG_SENSOR_MAX_OCCUPIED_MS + 1000; t += 200) {
        syn_read(&h, 0, &a, 20, 0, 12, t);
        syn_read(&h, 1, &b, 20, 0, 12, t);
    }
    CHECK(h.c[0].rise_pending == 0);
    CHECK(h.c[1].disagree == 0 && h.c[1].disagreements == 0);
    CHECK(lg_health_get(&h, 1).verdict == LG_VERDICT_OK);
    syn_rise(&h, 0, &a, t, t);
    syn_fall(&h, 0, &a, t + 200, t + 200); /* a 200 ms blip */
    syn_read(&h, 0, &a, 20, 0, 12, t + 1000);
    CHECK(h.c[0].rise_pending == 0);
    CHECK(h.c[1].disagreements == 0);
}

/* Rise times near the wrap of the 32-bit clock still pair. */
static void test_pair_clock_wrap(void) {
    static lg_health_t h;
    syn_t a, b;

    pair_start(&h, &a, &b);
    syn_rise(&h, 0, &a, 0xFFFFFF00u, 0xFFFFFF40u);
    syn_rise(&h, 1, &b, 0x00000020u, 0x00000040u); /* 288 ms after A */
    CHECK(h.c[0].rise_pending == 0 && h.c[1].rise_pending == 0);
    CHECK(h.c[1].delay_ewma_ms == 288);
    CHECK(h.c[0].delay_ewma_ms == 0);
    CHECK(h.c[0].disagreements == 0 && h.c[1].disagreements == 0);
}

/* Only an OK sensor accuses its partner: a failed sensor that sees phantom trains is no evidence. */
static void test_only_ok_sensor_accuses(void) {
    static lg_health_t h;
    syn_t a, b;
    uint32_t t;

    pair_start(&h, &a, &b);
    /* A sends nothing for a while: FAILED (dropouts). */
    for (t = 200; t < 4000u; t += 200u) {
        syn_read(&h, 0, &a, 20, 20, 0, t);
        syn_read(&h, 1, &b, 20, 0, 12, t);
    }
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_FAILED);
    CHECK(lg_health_get(&h, 1).verdict == LG_VERDICT_OK);
    /* A reports a whole run, B did not: B must not be accused. */
    syn_rise(&h, 0, &a, t, t);
    syn_fall(&h, 0, &a, t + 2000, t + 2000);
    t += 3000;
    syn_read(&h, 1, &b, 20, 0, 12, t);
    syn_read(&h, 1, &b, 20, 0, 12, t + 200);
    CHECK(h.c[0].rise_pending == 0);
    CHECK(h.c[1].disagree == 0 && h.c[1].disagreements == 0);
    CHECK(lg_health_get(&h, 1).verdict == LG_VERDICT_OK);
}

/* The latched disagreement is forgotten after disagree_clear agreeing trains, not before. */
static void test_disagree_clears_after_agreeing_trains(void) {
    static lg_health_t h;
    syn_t a, b;
    uint32_t t = 1000;
    int k;

    pair_start(&h, &a, &b);
    syn_rise(&h, 0, &a, t, t);
    syn_fall(&h, 0, &a, t + 2000, t + 2000); /* a whole train on A */
    for (t += 2200; t < 6000u; t += 200u) {
        syn_read(&h, 1, &b, 20, 0, 12, t); /* B never rose: judged 2 polls after A's fall */
    }
    CHECK(h.c[1].disagree == 1);
    for (k = 0; k < (int)h.cfg.disagree_clear; k++) {
        CHECK(h.c[1].disagree == 1);
        t += 20000;
        syn_rise(&h, 0, &a, t, t);
        t += 100;
        syn_rise(&h, 1, &b, t, t);
    }
    CHECK(h.c[1].disagree == 0);
}

/* ---- Gate and battery -------------------------------------------------------- */

static lg_health_gate_in_t gin(uint32_t moves, uint32_t travel, uint32_t peak, uint32_t mv) {
    lg_health_gate_in_t g;

    g.moves_done = moves;
    g.last_travel_ms = travel;
    g.last_peak_ma = peak;
    g.battery_mv = mv;
    return g;
}

static void test_gate_restart_and_skipped_moves(void) {
    static lg_health_t h;
    lg_health_gate_in_t g;

    fresh(&h, 0);
    g = gin(5, 4000, 800, 13000);
    lg_health_on_gate(&h, &g, 0); /* reference only */
    CHECK(h.c[LG_COMP_GATE].moves == 0 && h.c[LG_COMP_GATE].have_move == 0);
    g = gin(6, 4000, 800, 13000);
    lg_health_on_gate(&h, &g, 5000);
    CHECK(h.c[LG_COMP_GATE].moves == 1);
    CHECK(h.c[LG_COMP_GATE].travel_ewma_pm == 1000 && h.c[LG_COMP_GATE].current_ewma_pm == 1000);
    /* Two movements between two reads: one judgement, with the latest values. */
    g = gin(8, 4400, 800, 13000);
    lg_health_on_gate(&h, &g, 10000);
    CHECK(h.c[LG_COMP_GATE].moves == 2);
    CHECK(h.c[LG_COMP_GATE].travel_ewma_pm == 1050);
    /* Gate process restarted: moves_done went back to 0 with a huge, bad last_travel: not judged. */
    g = gin(0, 40000, 800, 13000);
    lg_health_on_gate(&h, &g, 15000);
    CHECK(h.c[LG_COMP_GATE].moves == 2);
    CHECK(h.c[LG_COMP_GATE].p_moves == 0);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_OK);
    /* The first movement after the restart counts again, normally. */
    g = gin(1, 4000, 800, 13000);
    lg_health_on_gate(&h, &g, 20000);
    CHECK(h.c[LG_COMP_GATE].moves == 3);
    /* The same count read twice judges nothing twice. */
    lg_health_on_gate(&h, &g, 20200);
    CHECK(h.c[LG_COMP_GATE].moves == 3);
}

/* A movement with zero nominal in a (refused-by-loader) config must not divide by zero. */
static void test_gate_zero_nominal_guard(void) {
    static lg_health_t h;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    lg_health_gate_in_t g;

    cfg.gate_travel_nominal_ms = 0;
    cfg.gate_current_nominal_ma = 0;
    lg_health_init(&h, &cfg, 0);
    g = gin(0, 0, 0, 13000);
    lg_health_on_gate(&h, &g, 0);
    g = gin(1, 4000, 800, 13000);
    lg_health_on_gate(&h, &g, 5000);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_FAILED); /* absurd ratio: fails closed */
}

/* FAILED -> WARNING -> OK: each step needs its own hold time, there is no jump from FAILED to OK. */
static void test_gate_recovery_steps(void) {
    static lg_health_t h;
    lg_health_gate_in_t g;
    uint32_t t = 0;
    uint32_t moves = 0;
    int seen_failed = 0, seen_warning_after_failed = 0, seen_ok_after_warning = 0;
    uint8_t last;
    int k;

    fresh(&h, 0);
    g = gin(moves, 4000, 800, 13000);
    lg_health_on_gate(&h, &g, t);
    moves++;
    t += 5000;
    g = gin(moves, 8000, 800, 13000); /* one movement at 200 % of nominal: FAILED at once */
    lg_health_on_gate(&h, &g, t);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_FAILED);
    CHECK(lg_health_get(&h, LG_COMP_GATE).diag == LG_DIAG_GATE_SLOW);
    CHECK(h.c[LG_COMP_GATE].latest_fail == 1);
    last = LG_VERDICT_FAILED;
    seen_failed = 1;
    for (k = 0; k < 40; k++) {
        uint8_t v;

        moves++;
        t += 5000;
        g = gin(moves, 4000, 800, 13000);
        lg_health_on_gate(&h, &g, t);
        v = lg_health_get(&h, LG_COMP_GATE).verdict;
        if (last == LG_VERDICT_FAILED && v == LG_VERDICT_OK) {
            CHECK(0 && "jumped from FAILED straight to OK");
        }
        if (seen_failed && v == LG_VERDICT_WARNING && last == LG_VERDICT_FAILED) {
            seen_warning_after_failed = 1;
        }
        if (seen_warning_after_failed && v == LG_VERDICT_OK && last == LG_VERDICT_WARNING) {
            seen_ok_after_warning = 1;
            CHECK(lg_health_get(&h, LG_COMP_GATE).diag == LG_DIAG_NONE);
            CHECK(lg_health_get(&h, LG_COMP_GATE).trend_s == -1);
            break;
        }
        if (v == LG_VERDICT_WARNING) {
            CHECK(lg_health_get(&h, LG_COMP_GATE).diag == LG_DIAG_GATE_SLOW); /* the reason is kept */
        }
        last = v;
    }
    CHECK(seen_warning_after_failed);
    CHECK(seen_ok_after_warning);
}

/* A high current alone (travel nominal) gives GATE_HIGH_CURRENT; the worse of the two wins. */
static void test_gate_current_vs_travel(void) {
    static lg_health_t h;
    lg_health_gate_in_t g;

    fresh(&h, 0);
    g = gin(0, 4000, 800, 13000);
    lg_health_on_gate(&h, &g, 0);
    g = gin(1, 4000, 1700, 13000); /* 212 % of the nominal current */
    lg_health_on_gate(&h, &g, 5000);
    CHECK(h.c[LG_COMP_GATE].latest_fail == 2);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_FAILED);
    CHECK(lg_health_get(&h, LG_COMP_GATE).diag == LG_DIAG_GATE_HIGH_CURRENT);
    lg_health_reset(&h, LG_COMP_GATE, 6000);
    g = gin(0, 4000, 800, 13000);
    lg_health_on_gate(&h, &g, 6000);
    g = gin(1, 7200, 1700, 13000); /* travel 180 % (fail), current 212 % (fail): travel is looked at first */
    lg_health_on_gate(&h, &g, 11000);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_FAILED);
    CHECK(h.c[LG_COMP_GATE].latest_fail == 1);
}

/* NO_DATA of the gate covers the battery as well; a read of the battery alone is not a thing. */
static void test_no_data_through_gate(void) {
    static lg_health_t h;
    lg_health_gate_in_t g;
    uint32_t t = 0;
    int k;

    fresh(&h, 0);
    g = gin(0, 4000, 800, 13000);
    lg_health_on_gate(&h, &g, t);
    lg_health_on_no_data(&h, LG_COMP_BATTERY, t); /* ignored */
    CHECK(h.c[LG_COMP_BATTERY].nodata == 0);
    for (k = 0; k < 2; k++) {
        lg_health_on_no_data(&h, LG_COMP_GATE, t += 50);
    }
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_OK);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).verdict == LG_VERDICT_OK);
    lg_health_on_no_data(&h, LG_COMP_GATE, t += 50);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_WARNING);
    CHECK(lg_health_get(&h, LG_COMP_GATE).diag == LG_DIAG_NO_DATA);
    CHECK(lg_health_get(&h, LG_COMP_GATE).score == 69);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).verdict == LG_VERDICT_WARNING);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).diag == LG_DIAG_NO_DATA);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).trend_s == -1);
    CHECK(lg_health_poll_period_ms(&h, LG_COMP_GATE) == h.cfg.poll_fast_ms);
    CHECK(lg_health_poll_period_ms(&h, LG_COMP_BATTERY) == h.cfg.poll_fast_ms);
    /* The device answers again: back to OK only after the hold time. */
    t += 50;
    g = gin(0, 4000, 800, 13000);
    lg_health_on_gate(&h, &g, t);
    CHECK(h.c[LG_COMP_GATE].nodata == 0 && h.c[LG_COMP_BATTERY].nodata == 0);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_WARNING);
    t += 1000;
    lg_health_on_gate(&h, &g, t);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_WARNING);
    t += 1500;
    lg_health_on_gate(&h, &g, t);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_OK);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).verdict == LG_VERDICT_OK);
    CHECK(lg_health_get(&h, LG_COMP_GATE).diag == LG_DIAG_NONE);
}

/* NO_DATA never hides a worse verdict: a FAILED gate stays FAILED with its own diagnosis. */
static void test_no_data_keeps_worse_verdict(void) {
    static lg_health_t h;
    lg_health_gate_in_t g;
    int k;

    fresh(&h, 0);
    g = gin(0, 4000, 800, 13000);
    lg_health_on_gate(&h, &g, 0);
    g = gin(1, 8000, 800, 13000);
    lg_health_on_gate(&h, &g, 5000);
    for (k = 0; k < 5; k++) {
        lg_health_on_no_data(&h, LG_COMP_GATE, 5100u + (uint32_t)k * 50u);
    }
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_FAILED);
    CHECK(lg_health_get(&h, LG_COMP_GATE).diag == LG_DIAG_GATE_SLOW);
    /* The counter saturates instead of wrapping. */
    for (k = 0; k < 300; k++) {
        lg_health_on_no_data(&h, 0, 6000u + (uint32_t)k);
    }
    CHECK(h.c[0].nodata == 255);
    lg_health_on_no_data(&h, LG_COMP_COUNT, 9000); /* ignored, no crash */
}

/* Battery: voltage far below the failure level on the very first reading is FAILED, BATTERY_LOW. */
static void test_battery_levels(void) {
    static lg_health_t h;
    lg_health_gate_in_t g;

    fresh(&h, 0);
    g = gin(0, 4000, 800, 10900);
    lg_health_on_gate(&h, &g, 0);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).verdict == LG_VERDICT_FAILED);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).diag == LG_DIAG_BATTERY_LOW);

    fresh(&h, 0);
    g = gin(0, 4000, 800, 11500);
    lg_health_on_gate(&h, &g, 0);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).verdict == LG_VERDICT_WARNING);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).diag == LG_DIAG_BATTERY_LOW);

    fresh(&h, 0);
    g = gin(0, 4000, 800, 13200);
    lg_health_on_gate(&h, &g, 0);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).verdict == LG_VERDICT_OK);
    CHECK(h.c[LG_COMP_BATTERY].batt_tte_s == -1);
    /* The same time twice: no division by zero, nothing changes. */
    lg_health_on_gate(&h, &g, 0);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).verdict == LG_VERDICT_OK);
}

/* Reset: the gate also resets the battery, the battery alone only the battery, a bad id nothing. */
static void test_reset_scope(void) {
    static lg_health_t h;
    lg_health_gate_in_t g;

    fresh(&h, 0);
    g = gin(0, 4000, 800, 10900);
    lg_health_on_gate(&h, &g, 0);
    g = gin(1, 8000, 800, 10900);
    lg_health_on_gate(&h, &g, 5000);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_FAILED);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).verdict == LG_VERDICT_FAILED);

    lg_health_reset(&h, LG_COMP_COUNT, 6000);
    lg_health_reset(&h, 255, 6000);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_FAILED);

    lg_health_reset(&h, LG_COMP_BATTERY, 6000);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).verdict == LG_VERDICT_OK);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).score == 100 && h.c[LG_COMP_BATTERY].have_batt == 0);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_FAILED);

    /* Battery gets low again, then the gate reset takes the battery with it. */
    g = gin(2, 4000, 800, 10900);
    lg_health_on_gate(&h, &g, 7000);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).verdict == LG_VERDICT_FAILED);
    lg_health_reset(&h, LG_COMP_GATE, 8000);
    CHECK(lg_health_get(&h, LG_COMP_GATE).verdict == LG_VERDICT_OK);
    CHECK(lg_health_get(&h, LG_COMP_BATTERY).verdict == LG_VERDICT_OK);
    CHECK(h.c[LG_COMP_GATE].have_prev == 0 && h.c[LG_COMP_GATE].moves == 0);
    CHECK(h.c[LG_COMP_BATTERY].learning == 1 && h.c[LG_COMP_BATTERY].start_ms == 8000);
}

/* A sensor reset: learning again, the baseline goes back to the default and a latched alarm is gone. */
static void test_sensor_reset(void) {
    static lg_health_t h;
    syn_t s;
    uint32_t t = 0;

    fresh(&h, 0);
    memset(&s, 0, sizeof s);
    phase(&h, &s, &t, 0, 12, 12000);
    phase(&h, &s, &t, 50, 40, 20000);
    CHECK(lg_health_get(&h, 0).verdict != LG_VERDICT_OK);
    lg_health_reset(&h, 0, t);
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_OK && lg_health_get(&h, 0).score == 100);
    CHECK(h.c[0].learning == 1 && h.c[0].have_prev == 0 && h.c[0].disagree == 0);
    CHECK(h.c[0].noise_base_milli == h.cfg.noise_base_milli);
    CHECK(h.c[0].drop_ewma_pm == 0 && h.c[0].cusum_milli == 0);
}

/* ---- Hysteresis -------------------------------------------------------------- */

/*
 * Dropouts settle near 48 per mille (score 71) with a 4.5 % input: below the ok_score of 75, so a
 * sensor that was WARNING stays WARNING there, while one that never left OK stays OK. At 3 % it
 * settles near 33 per mille (score 80): back to OK after the hold time.
 */
static void test_hysteresis_band(void) {
    static lg_health_t h;
    syn_t s;
    uint32_t t = 0;
    uint32_t k;
    uint32_t read_ticks = 1000;

    fresh(&h, 0);
    memset(&s, 0, sizeof s);
    phase(&h, &s, &t, 0, 12, 12000); /* learn, 100 ticks per read */
    /* Raise: 10 % dropouts, WARNING. */
    for (k = 0; k < 30; k++, t += 200u) {
        syn_read(&h, 0, &s, read_ticks, 100, 12, t);
    }
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_WARNING);
    /* Band: score 70..74 is not enough to return to OK. */
    for (k = 0; k < 60; k++, t += 200u) {
        syn_read(&h, 0, &s, read_ticks, 45, 12, t);
    }
    /* Integer EWMA coming down from above stops 3 short of the input: 48 (score 71). */
    CHECK(h.c[0].drop_ewma_pm >= 45 && h.c[0].drop_ewma_pm <= 48);
    CHECK(lg_health_get(&h, 0).score >= 70 && lg_health_get(&h, 0).score < h.cfg.ok_score);
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_WARNING);
    CHECK(lg_health_get(&h, 0).diag == LG_DIAG_DROPOUTS);
    /* Better than ok_score: OK only after the hold time. */
    for (k = 0; k < 40; k++, t += 200u) {
        syn_read(&h, 0, &s, read_ticks, 30, 12, t);
    }
    CHECK(lg_health_get(&h, 0).score >= h.cfg.ok_score);
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_OK);
    CHECK(lg_health_get(&h, 0).diag == LG_DIAG_NONE);
    CHECK(h.c[0].changes == 2); /* OK -> WARNING -> OK */
}

/* A score that is better for a while and then worse again restarts the hold time. */
static void test_hold_restarts(void) {
    static lg_health_t h;
    syn_t s;
    uint32_t t = 0;
    uint32_t k;
    uint32_t ok_since;

    fresh(&h, 0);
    memset(&s, 0, sizeof s);
    phase(&h, &s, &t, 0, 12, 12000);
    for (k = 0; k < 30; k++, t += 200u) {
        syn_read(&h, 0, &s, 1000, 100, 12, t);
    }
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_WARNING);
    /* Good readings until just short of the hold time after the score first reaches ok_score. */
    for (k = 0; lg_health_get(&h, 0).score < h.cfg.ok_score; k++, t += 200u) {
        syn_read(&h, 0, &s, 1000, 0, 12, t);
        CHECK(k < 100);
    }
    ok_since = t;
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_WARNING);
    CHECK(h.c[0].better_tracking == 1);
    /* Relapse: heavy dropouts for 2 reads; score falls under the OK band, tracking restarts later. */
    syn_read(&h, 0, &s, 1000, 400, 12, t += 200u);
    syn_read(&h, 0, &s, 1000, 400, 12, t += 200u);
    CHECK(lg_health_get(&h, 0).verdict == LG_VERDICT_WARNING);
    (void)ok_since;
}

/* ---- Names, text, bad arguments -------------------------------------------------- */

static void test_names_and_bad_args(void) {
    static lg_health_t h;
    lg_health_out_t o;

    fresh(&h, 0);
    CHECK(strcmp(lg_health_diag_name(LG_DIAG_COUNT), "UNKNOWN") == 0);
    CHECK(strcmp(lg_health_diag_name(0xFFFF), "UNKNOWN") == 0);
    CHECK(strcmp(lg_health_diag_text(LG_DIAG_COUNT), "unknown diagnosis") == 0);
    CHECK(strcmp(lg_health_diag_text(LG_DIAG_NONE), "healthy") == 0);
    CHECK(strcmp(lg_health_comp_name(LG_COMP_COUNT), "?") == 0);
    CHECK(strcmp(lg_health_comp_name(255), "?") == 0);
    CHECK(strcmp(lg_health_comp_name(LG_COMP_BATTERY), "battery") == 0);
    o = lg_health_get(&h, LG_COMP_COUNT);
    CHECK(o.verdict == LG_VERDICT_FAILED && o.score == 0 && o.diag == LG_DIAG_NO_DATA && o.trend_s == -1);
    CHECK(lg_health_poll_period_ms(&h, 99) == h.cfg.poll_ms);
    lg_health_on_gate(&h, NULL, 0); /* no crash */
    lg_health_on_no_data(&h, 99, 0);
    CHECK(h.c[0].verdict == LG_VERDICT_OK);
    CHECK(lg_health_verdict_from_score(70) == LG_VERDICT_OK);
    CHECK(lg_health_verdict_from_score(69) == LG_VERDICT_WARNING);
    CHECK(lg_health_verdict_from_score(40) == LG_VERDICT_WARNING);
    CHECK(lg_health_verdict_from_score(39) == LG_VERDICT_FAILED);
    CHECK(lg_health_verdict_from_score(0) == LG_VERDICT_FAILED);
    CHECK(lg_health_verdict_from_score(255) == LG_VERDICT_OK);
}

static void test_text(void) {
    static lg_health_t h;
    static char buf[4096];
    size_t full;
    size_t len;
    size_t n;
    int lines = 0;
    const char *p;
    lg_health_gate_in_t g;

    fresh(&h, 0);
    g = gin(0, 4000, 800, 13000);
    lg_health_on_gate(&h, &g, 0);
    full = lg_health_text(&h, buf, sizeof buf);
    CHECK(full == strlen(buf) && full > 0);
    for (p = buf; *p; p++) {
        if (*p == '\n') {
            lines++;
        }
    }
    CHECK(lines == LG_COMP_COUNT);
    CHECK(strstr(buf, "comp=west/a ") == buf);
    CHECK(strstr(buf, "comp=gate score=100 verdict=OK diag=NONE") != NULL);
    CHECK(strstr(buf, "comp=battery score=95 verdict=OK") != NULL);
    CHECK(strstr(buf, "mv=13000") != NULL);

    /* Every possible buffer size: always terminated, the return value is the string length. */
    for (len = 1; len <= full + 8; len++) {
        static char small[4096];

        memset(small, 'x', sizeof small);
        n = lg_health_text(&h, small, len);
        CHECK(n < len);
        CHECK(small[n] == '\0');
        CHECK(strlen(small) == n);
        CHECK(strncmp(small, buf, n) == 0);
        if (len > full) {
            CHECK(n == full);
        }
    }
    CHECK(lg_health_text(&h, buf, 0) == 0);
    CHECK(lg_health_text(&h, NULL, 100) == 0);
}

/* ---- thresholds.json edges ------------------------------------------------------- */

static lg_json_doc_t g_doc;

static int load(const char *text, lg_health_cfg_t *cfg, char *err, size_t errlen) {
    *cfg = lg_health_cfg_default();
    if (lg_json_parse(&g_doc, text, strlen(text)) != LG_JSON_OK) {
        return -2;
    }
    return lg_health_cfg_from_json(&g_doc, cfg, err, errlen);
}

static void test_cfg_edges(void) {
    lg_health_cfg_t cfg;
    lg_health_cfg_t def = lg_health_cfg_default();
    char err[96];
    const char *bad = "{\"poll_ms\": 300, \"learn_ms\": 5000, \"bogus\": 1}";

    /* Range bounds are inclusive. */
    CHECK(load("{\"poll_ms\": 10, \"poll_fast_ms\": 10}", &cfg, err, sizeof err) == 0);
    CHECK(cfg.poll_ms == 10 && cfg.poll_fast_ms == 10);
    CHECK(load("{\"poll_ms\": 10000}", &cfg, err, sizeof err) == 0);
    CHECK(load("{\"poll_ms\": 10001}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"poll_ms\": 9}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"poll_ms\": -200}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"ok_score\": 70}", &cfg, err, sizeof err) == 0 && cfg.ok_score == 70);
    CHECK(load("{\"ok_score\": 100}", &cfg, err, sizeof err) == 0 && cfg.ok_score == 100);
    CHECK(load("{\"ok_score\": 69}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"ok_score\": 101}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"gate_travel_fail_pct\": 101}", &cfg, err, sizeof err) == 0);
    CHECK(load("{\"gate_travel_fail_pct\": 100}", &cfg, err, sizeof err) == -1); /* fail must exceed nominal */
    CHECK(load("{\"gate_current_fail_pct\": 100}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"nodata_polls\": 0}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"nodata_polls\": 100}", &cfg, err, sizeof err) == 0);
    CHECK(load("{\"disagree_clear\": 0}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"learn_ms\": 0}", &cfg, err, sizeof err) == 0 && cfg.learn_ms == 0);
    CHECK(load("{\"gate_travel_nominal_ms\": 0}", &cfg, err, sizeof err) == -1); /* no division by zero later */
    CHECK(load("{\"gate_current_nominal_ma\": 0}", &cfg, err, sizeof err) == -1);

    /* Noise values may have three decimals, the rest only whole numbers. */
    CHECK(load("{\"noise_base\": 2.5, \"noise_learn_max\": 1.25, \"noise_fail\": 12.125}", &cfg, err, sizeof err) == 0);
    CHECK(cfg.noise_base_milli == 2500 && cfg.noise_learn_max_milli == 1250 && cfg.noise_fail_milli == 12125);
    CHECK(load("{\"cusum_k\": 0.5, \"cusum_h\": 1.5}", &cfg, err, sizeof err) == 0);
    CHECK(cfg.cusum_k_milli == 500 && cfg.cusum_h_milli == 1500);
    CHECK(load("{\"noise_fail\": 100}", &cfg, err, sizeof err) == 0);
    CHECK(load("{\"noise_fail\": 100.001}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"noise_fail\": 0}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"gate_cusum_k_pct\": 5.5}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"battery_full_mv\": 13200.5}", &cfg, err, sizeof err) == -1);

    /* Order rules: the equal case is refused, one step apart is fine. */
    CHECK(load("{\"noise_fail\": 5}", &cfg, err, sizeof err) == -1);     /* = base 3 + learn max 2 */
    CHECK(load("{\"noise_fail\": 5.001}", &cfg, err, sizeof err) == 0);
    CHECK(load("{\"dropout_warn_pm\": 300}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"dropout_warn_pm\": 299}", &cfg, err, sizeof err) == 0);
    CHECK(load("{\"delay_zero_ms\": 400}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"delay_zero_ms\": 399}", &cfg, err, sizeof err) == 0);
    CHECK(load("{\"battery_tte_fail_s\": 600}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"battery_tte_fail_s\": 599}", &cfg, err, sizeof err) == 0);
    CHECK(load("{\"battery_empty_mv\": 11000}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"battery_empty_mv\": 10999}", &cfg, err, sizeof err) == 0);
    CHECK(load("{\"battery_full_mv\": 12000}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"poll_ms\": 1000, \"poll_fast_ms\": 1000}", &cfg, err, sizeof err) == 0);
    CHECK(load("{\"poll_fast_ms\": 201}", &cfg, err, sizeof err) == -1);

    /* Wrong value types. */
    CHECK(load("{\"poll_ms\": null}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"poll_ms\": true}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"poll_ms\": [200]}", &cfg, err, sizeof err) == -1);
    CHECK(load("{\"poll_ms\": {}}", &cfg, err, sizeof err) == -1);
    CHECK(load("\"poll_ms\"", &cfg, err, sizeof err) == -1);
    CHECK(load("42", &cfg, err, sizeof err) == -1);

    /* Error text names the key; NULL or tiny error buffers are fine. */
    CHECK(load("{\"bogus_key\": 1}", &cfg, err, sizeof err) == -1);
    CHECK(strstr(err, "bogus_key") != NULL && strstr(err, "unknown key") != NULL);
    CHECK(load("{\"poll_ms\": 5}", &cfg, err, sizeof err) == -1);
    CHECK(strstr(err, "poll_ms") != NULL && strstr(err, "out of range") != NULL);
    CHECK(load("{\"bogus_key\": 1}", &cfg, NULL, 0) == -1);
    CHECK(load("{\"bogus_key\": 1}", &cfg, err, 1) == -1 && err[0] == '\0');
    CHECK(load("{\"bogus_key\": 1}", &cfg, err, 8) == -1 && strlen(err) == 7);

    /* A refused file changes nothing, even when an earlier key was fine. */
    cfg = def;
    CHECK(lg_json_parse(&g_doc, bad, strlen(bad)) == LG_JSON_OK);
    CHECK(lg_health_cfg_from_json(&g_doc, &cfg, err, sizeof err) == -1);
    CHECK(memcmp(&cfg, &def, sizeof cfg) == 0);
    /* Same for an order violation found only at the end. */
    CHECK(load("{\"poll_ms\": 300, \"dropout_warn_pm\": 400}", &cfg, err, sizeof err) == -1);
    CHECK(memcmp(&cfg, &def, sizeof cfg) == 0);
}

int main(void) {
    test_restart_each_field();
    test_restart_forward_jump();
    test_no_new_ticks();
    test_combined_causes();
    test_silent_sensor_is_dropouts();
    test_delay_ewma();
    test_island_has_no_pair();
    test_pair_far_apart();
    test_stuck_or_blip_accuses_nobody();
    test_pair_clock_wrap();
    test_only_ok_sensor_accuses();
    test_disagree_clears_after_agreeing_trains();
    test_gate_restart_and_skipped_moves();
    test_gate_zero_nominal_guard();
    test_gate_recovery_steps();
    test_gate_current_vs_travel();
    test_no_data_through_gate();
    test_no_data_keeps_worse_verdict();
    test_battery_levels();
    test_reset_scope();
    test_sensor_reset();
    test_hysteresis_band();
    test_hold_restarts();
    test_names_and_bad_args();
    test_text();
    test_cfg_edges();
    LG_TEST_DONE("test_health_extra");
}
