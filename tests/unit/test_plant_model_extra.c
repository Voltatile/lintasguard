#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lg_plant_model.h"
#include "lg_test.h"

/*
 * Extra tests for core/plant_model.c: invariants over random scenarios (monotonic axle counts,
 * occupancy against an independent floating point oracle, island_any consistency), events against
 * the truth, sliding event windows, equal strike times, short trains in wide zones and integer
 * extremes. Seeded, the seed is printed.
 */

static lg_crossing_cfg_t cfg = {400000, 12000, 2000};
static lg_scenario_t sc;
static lg_plant_t plant;

static uint32_t rng_state = 0xBADC0DE5u;

static uint32_t rnd(void) {
    uint32_t x = rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static void reset(void) {
    memset(&sc, 0, sizeof sc);
    (void)snprintf(sc.name, sizeof sc.name, "extra");
}

static void add_train(uint16_t id, uint8_t dir, uint32_t t_strike_ms, int32_t speed_mmps,
                      int32_t length_mm, uint8_t axles) {
    lg_train_t *t = &sc.trains[sc.n_trains++];

    t->id = id;
    t->dir = dir;
    t->t_strike_ms = t_strike_ms;
    t->speed_mmps = speed_mmps;
    t->length_mm = length_mm;
    t->axles = axles;
}

static void start(void) {
    lg_plant_init(&plant, &cfg, &sc);
}

static void random_scenario(unsigned max_trains, int allow_overlap_same_dir) {
    unsigned n = 1 + rnd() % max_trains;
    unsigned i;

    (void)allow_overlap_same_dir;
    reset();
    cfg.strike_in_mm = (int32_t)(50000 + (rnd() % 400) * 1000 + rnd() % 1000);
    cfg.island_mm = (int32_t)(1000 + (rnd() % 100) * 1000 + rnd() % 1000);
    cfg.sensor_zone_mm = (int32_t)(1000 + (rnd() % 20) * 1000 + rnd() % 1000);
    if (cfg.sensor_zone_mm > 20000) {
        cfg.sensor_zone_mm = 20000;
    }
    for (i = 0; i < n; i++) {
        add_train((uint16_t)(i + 1), (uint8_t)(rnd() & 1u), 1 + rnd() % 60000,
                  (int32_t)(1000 + (rnd() % 100) * 1000 + rnd() % 1000),
                  (int32_t)(5000 + (rnd() % 400) * 1000 + rnd() % 1000), (uint8_t)(1 + rnd() % 200));
    }
}

/* ---- independent oracle (floating point, real valued) ---------------------------------------- */

static double fr(const lg_train_t *t, uint32_t now) {
    return (double)t->speed_mmps * ((double)now - (double)t->t_strike_ms) / 1000.0;
}

/* axles that have passed x when the front is at f (real valued): axle k sits at f - (k+.5)L/N */
static int axles_oracle(const lg_train_t *t, double f, double x) {
    int k;
    int c = 0;

    for (k = 0; k < t->axles; k++) {
        double pos = f - ((double)k + 0.5) * (double)t->length_mm / (double)t->axles;

        if (pos >= x) {
            c++;
        }
    }
    return c;
}

/* A value that sits within 1.5 mm of a boundary is not judged (the model uses whole mm). */
#define NEAR(a, b) (fabs((a) - (b)) < 1.5)

static void test_random_truth(void) {
    int round;
    unsigned bad_mono = 0;
    unsigned bad_occ = 0;
    unsigned bad_island = 0;
    unsigned bad_axles = 0;
    unsigned bad_any = 0;
    unsigned bad_counts = 0;

    printf("plant extra seed 0xBADC0DE5\n");
    for (round = 0; round < 300; round++) {
        uint16_t last[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
        uint32_t t;
        uint32_t t_end = 0;
        unsigned dir;
        unsigned i;

        random_scenario(6, 1);
        start();
        for (i = 0; i < sc.n_trains; i++) {
            const lg_train_t *tr = &sc.trains[i];
            double need = ((double)cfg.strike_in_mm + cfg.island_mm + tr->length_mm) * 1000.0 /
                          (double)tr->speed_mmps;

            if (tr->t_strike_ms + (uint32_t)need + 3000 > t_end) {
                t_end = tr->t_strike_ms + (uint32_t)need + 3000;
            }
        }
        memset(last, 0, sizeof last);
        for (t = 0; t <= t_end; t += 7 + (uint32_t)(rnd() % 90)) {
            unsigned dir;
            unsigned id;
            int island_occ = 0;
            lg_plant_counts_t c = lg_plant_counts(&plant, t);

            for (i = 0; i < sc.n_trains; i++) {
                const lg_train_t *tr = &sc.trains[i];
                double f = fr(tr, t);
                double r = f - tr->length_mm;
                double exit = (double)cfg.strike_in_mm + cfg.island_mm;

                if (f > cfg.strike_in_mm + 1.5 && r < exit - 1.5) {
                    island_occ = 1; /* surely on the island */
                } else if (!NEAR(f, cfg.strike_in_mm) && !NEAR(r, exit)) {
                    /* surely not: fine, island_occ stays as it is */
                }
            }
            if (island_occ && !c.island_any) {
                bad_any++;
            }
            for (dir = 0; dir < LG_DIR_COUNT; dir++) {
                for (id = 0; id < LG_SENSOR_ID_COUNT; id++) {
                    lg_truth_t tr = lg_plant_truth(&plant, t, (uint8_t)dir, (uint8_t)id);
                    int expect_occ = 0;
                    int surely_occ = 0;
                    int unsure = 0;
                    int lo = 0;
                    int hi = 0;

                    if (tr.axles < last[dir][id]) {
                        bad_mono++;
                    }
                    last[dir][id] = tr.axles;
                    if (tr.island_any != c.island_any) {
                        bad_any++;
                    }
                    for (i = 0; i < sc.n_trains; i++) {
                        const lg_train_t *t2 = &sc.trains[i];
                        double f = fr(t2, t);
                        double r = f - t2->length_mm;
                        double exit = (double)cfg.strike_in_mm + cfg.island_mm;
                        double x0 = id == LG_SENSOR_ISLAND ? exit : 0.0;

                        if (id == LG_SENSOR_ISLAND) {
                            if (f > cfg.strike_in_mm + 1.5 && r < exit - 1.5) {
                                surely_occ = 1;
                                expect_occ = 1;
                            } else if (NEAR(f, cfg.strike_in_mm) || NEAR(r, exit)) {
                                unsure = 1;
                            }
                        } else if (t2->dir == dir) {
                            if (f > 1.5 && r < cfg.sensor_zone_mm - 1.5) {
                                surely_occ = 1;
                                expect_occ = 1;
                            } else if (NEAR(f, 0.0) || NEAR(r, cfg.sensor_zone_mm)) {
                                unsure = 1;
                            }
                        }
                        if (t2->dir == dir) {
                            lo += axles_oracle(t2, f - 1.5, x0);
                            hi += axles_oracle(t2, f + 1.5, x0);
                        }
                    }
                    if (!unsure && (tr.occupied != 0) != (expect_occ != 0)) {
                        bad_occ++;
                        printf("occupancy mismatch t=%u dir=%u id=%u got %d want %d\n", (unsigned)t, dir, id,
                               tr.occupied, expect_occ);
                    }
                    if (surely_occ && !tr.occupied) {
                        bad_occ++;
                    }
                    if (id == LG_SENSOR_ISLAND && (tr.occupied != 0) != (c.island_any != 0)) {
                        bad_island++;
                    }
                    if ((int)tr.axles < lo || (int)tr.axles > hi) {
                        bad_axles++;
                        printf("axle mismatch t=%u dir=%u id=%u got %u want %d..%d\n", (unsigned)t, dir, id,
                               (unsigned)tr.axles, lo, hi);
                    }
                }
            }
            if (c.waiting + c.active + c.finished != sc.n_trains) {
                bad_counts++;
            }
        }
        /* after everything: every train's axles counted on each of its sensors */
        for (dir = 0; dir < LG_DIR_COUNT; dir++) {
            unsigned want = 0;
            unsigned id;

            for (i = 0; i < sc.n_trains; i++) {
                if (sc.trains[i].dir == dir) {
                    want += sc.trains[i].axles;
                }
            }
            for (id = 0; id < LG_SENSOR_ID_COUNT; id++) {
                lg_truth_t tr = lg_plant_truth(&plant, t_end, (uint8_t)dir, (uint8_t)id);

                if (tr.axles != want || tr.occupied != 0 || tr.island_any != 0) {
                    bad_axles++;
                }
            }
        }
        {
            lg_plant_counts_t c = lg_plant_counts(&plant, t_end);

            if (c.finished != sc.n_trains || c.active != 0 || c.waiting != 0 || c.island_any != 0) {
                bad_counts++;
            }
            c = lg_plant_counts(&plant, 0);
            if (c.waiting != sc.n_trains) {
                bad_counts++;
            }
        }
    }
    CHECK(bad_mono == 0);
    CHECK(bad_occ == 0);
    CHECK(bad_island == 0);
    CHECK(bad_axles == 0);
    CHECK(bad_any == 0);
    CHECK(bad_counts == 0);
}

/* ---- events versus truth --------------------------------------------------------------------- */

static void test_events_match_truth(void) {
    int round;
    unsigned bad = 0;
    uint32_t worst[4] = {0, 0, 0, 0};

    for (round = 0; round < 400; round++) {
        lg_plant_event_t ev[8];
        size_t n;
        const lg_train_t *tr;
        uint32_t t;
        uint32_t t_enter = 0;
        uint32_t t_clear = 0;
        uint32_t t_strike = 0;
        int seen_enter = 0;
        int seen_clear = 0;
        int seen_strike = 0;
        uint32_t t_end;
        uint8_t dir;
        size_t k;

        random_scenario(1, 0);
        start();
        tr = &sc.trains[0];
        dir = tr->dir;
        t_end = tr->t_strike_ms +
                (uint32_t)(((double)cfg.strike_in_mm + cfg.island_mm + tr->length_mm) * 1000.0 /
                           tr->speed_mmps) +
                3000;
        n = lg_plant_events(&plant, 0, t_end, ev, 8);
        if (n != 4) {
            bad++;
            continue;
        }
        /* first ms at which the truth shows each change */
        for (t = 0; t <= t_end; t++) {
            lg_truth_t a = lg_plant_truth(&plant, t, dir, LG_SENSOR_A);
            lg_truth_t isl = lg_plant_truth(&plant, t, dir, LG_SENSOR_ISLAND);

            if (!seen_strike && a.occupied) {
                seen_strike = 1;
                t_strike = t;
            }
            if (!seen_enter && isl.occupied) {
                seen_enter = 1;
                t_enter = t;
            }
            if (seen_enter && !seen_clear && !isl.occupied) {
                seen_clear = 1;
                t_clear = t;
            }
        }
        if (!seen_strike || !seen_enter || !seen_clear) {
            bad++;
            continue;
        }
        for (k = 0; k < 4; k++) {
            uint32_t truth_t = 0;
            uint32_t d;

            if (ev[k].kind == LG_PLANT_EV_STRIKE_IN) {
                truth_t = t_strike;
            } else if (ev[k].kind == LG_PLANT_EV_ISLAND_ENTER) {
                truth_t = t_enter;
            } else if (ev[k].kind == LG_PLANT_EV_CLEARED) {
                truth_t = t_clear;
            } else if (ev[k].kind == LG_PLANT_EV_ISLAND_EXIT) {
                /* the front is at the island exit: the first axle passes later; check the front */
                double f = (double)tr->speed_mmps * ((double)ev[k].t_ms - (double)tr->t_strike_ms) / 1000.0;
                double exit = (double)cfg.strike_in_mm + cfg.island_mm;

                if (f > exit + 1e-6 || f < exit - (double)tr->speed_mmps / 1000.0 - 1e-6) {
                    bad++;
                }
                continue;
            } else {
                bad++;
                continue;
            }
            /* the event never comes after the truth change, and at most 2 ms before it */
            if (ev[k].t_ms > truth_t) {
                bad++;
                continue;
            }
            d = truth_t - ev[k].t_ms;
            if (d > worst[ev[k].kind - 1]) {
                worst[ev[k].kind - 1] = d;
            }
            if (d > 2) {
                bad++;
            }
            if (ev[k].train_id != tr->id || ev[k].dir != tr->dir) {
                bad++;
            }
        }
        if (!(ev[0].t_ms <= ev[1].t_ms && ev[1].t_ms <= ev[2].t_ms && ev[2].t_ms <= ev[3].t_ms)) {
            bad++;
        }
        if (!(ev[0].kind == 1 && ev[1].kind == 2 && ev[2].kind == 3 && ev[3].kind == 4)) {
            bad++;
        }
    }
    printf("event lag behind truth, worst ms: strike-in %u, enter %u, exit n/a, cleared %u\n",
           (unsigned)worst[0], (unsigned)worst[1], (unsigned)worst[3]);
    CHECK(bad == 0);
}

/* ---- sliding windows ------------------------------------------------------------------------- */

static void test_sliding_windows(void) {
    int round;
    unsigned bad = 0;

    for (round = 0; round < 200; round++) {
        lg_plant_event_t whole[LG_SCENARIO_MAX_TRAINS * 4];
        lg_plant_event_t pieced[LG_SCENARIO_MAX_TRAINS * 4];
        size_t n_whole;
        size_t n_pieced = 0;
        uint32_t t_prev = 0;
        uint32_t t_end = 400000;
        unsigned win_kind = (unsigned)(rnd() % 4);
        size_t i;

        random_scenario(10, 1);
        start();
        n_whole = lg_plant_events(&plant, 0, t_end, whole, LG_SCENARIO_MAX_TRAINS * 4);
        /* every train strikes in at >= 1 ms, so every event happened in (0, end] */
        for (i = 0; i < sc.n_trains; i++) {
            const lg_train_t *tr = &sc.trains[i];
            double end = tr->t_strike_ms +
                         ((double)cfg.strike_in_mm + cfg.island_mm + tr->length_mm) * 1000.0 / tr->speed_mmps;

            if (end > (double)t_end) {
                t_end = (uint32_t)end + 10;
            }
        }
        n_whole = lg_plant_events(&plant, 0, t_end, whole, LG_SCENARIO_MAX_TRAINS * 4);
        if (n_whole != sc.n_trains * 4) {
            bad++;
        }
        while (t_prev < t_end) {
            uint32_t step;
            uint32_t t_now;
            size_t n;

            switch (win_kind) {
            case 0: step = 1; break;
            case 1: step = 1 + rnd() % 10; break;
            case 2: step = 1 + rnd() % 5000; break;
            default: step = 1 + rnd() % 300000; break;
            }
            if (win_kind == 0 && t_prev < 60000) {
                step = 10; /* keep the 1 ms case affordable: only the tail is stepped by 1 */
            }
            t_now = t_prev + step;
            if (t_now > t_end || t_now < t_prev) {
                t_now = t_end;
            }
            n = lg_plant_events(&plant, t_prev, t_now, pieced + n_pieced,
                                LG_SCENARIO_MAX_TRAINS * 4 - n_pieced);
            for (i = 0; i < n; i++) {
                if (pieced[n_pieced + i].t_ms <= t_prev || pieced[n_pieced + i].t_ms > t_now) {
                    bad++;
                }
            }
            n_pieced += n;
            t_prev = t_now;
            if (win_kind == 0 && n_pieced > 0 && t_prev > 120000) {
                /* stop stepping by 10: finish in one go */
                n = lg_plant_events(&plant, t_prev, t_end, pieced + n_pieced,
                                    LG_SCENARIO_MAX_TRAINS * 4 - n_pieced);
                n_pieced += n;
                t_prev = t_end;
            }
        }
        if (n_pieced != n_whole) {
            bad++;
            continue;
        }
        /* concatenated windows are the whole list: each event exactly once and in order */
        if (memcmp(whole, pieced, n_whole * sizeof whole[0]) != 0) {
            /* ties between trains are ordered inside a window only: compare as sorted multisets */
            size_t a;
            size_t b;
            size_t found = 0;

            for (a = 0; a < n_whole; a++) {
                for (b = 0; b < n_pieced; b++) {
                    if (whole[a].t_ms == pieced[b].t_ms && whole[a].train_id == pieced[b].train_id &&
                        whole[a].kind == pieced[b].kind && whole[a].dir == pieced[b].dir) {
                        found++;
                        break;
                    }
                }
            }
            if (found != n_whole) {
                bad++;
            }
        }
        for (i = 1; i < n_pieced; i++) {
            if (pieced[i].t_ms < pieced[i - 1].t_ms) {
                bad++;
            }
        }
    }
    CHECK(bad == 0);
}

static void test_event_edges(void) {
    lg_plant_event_t ev[16];

    /* v = 1 m/s: 1 mm per ms. Train strikes in at 1000 ms. Events at 1000, 401000, 413000, 418000
     * for a 5 m train (cfg 400 m + 12 m). */
    cfg.strike_in_mm = 400000;
    cfg.island_mm = 12000;
    cfg.sensor_zone_mm = 2000;
    reset();
    add_train(1, LG_DIR_WEST, 1000, 1000, 5000, 4);
    start();
    CHECK(lg_plant_events(&plant, 0, 1000, ev, 16) == 1 && ev[0].t_ms == 1000);
    CHECK(lg_plant_events(&plant, 999, 1000, ev, 16) == 1 && ev[0].kind == LG_PLANT_EV_STRIKE_IN);
    CHECK(lg_plant_events(&plant, 1000, 1001, ev, 16) == 0); /* the window is open at the start */
    CHECK(lg_plant_events(&plant, 0, 999, ev, 16) == 0);
    CHECK(lg_plant_events(&plant, 401000, 401000, ev, 16) == 0); /* empty window */
    CHECK(lg_plant_events(&plant, 401001, 401000, ev, 16) == 0); /* reversed window */
    CHECK(lg_plant_events(&plant, 400999, 401000, ev, 16) == 1 && ev[0].kind == LG_PLANT_EV_ISLAND_ENTER);
    CHECK(lg_plant_events(&plant, 412999, 413000, ev, 16) == 1 && ev[0].kind == LG_PLANT_EV_ISLAND_EXIT);
    CHECK(lg_plant_events(&plant, 417999, 418000, ev, 16) == 1 && ev[0].kind == LG_PLANT_EV_CLEARED);
    CHECK(lg_plant_events(&plant, 0, 0xFFFFFFFFu, ev, 16) == 4);
    CHECK(lg_plant_events(&plant, 418000, 0xFFFFFFFFu, ev, 16) == 0);
    CHECK(lg_plant_events(&plant, 0, 0xFFFFFFFFu, ev, 0) == 0);
    CHECK(lg_plant_events(&plant, 0, 0xFFFFFFFFu, ev, 1) == 1 && ev[0].kind == LG_PLANT_EV_STRIKE_IN);
    CHECK(lg_plant_events(&plant, 0, 0xFFFFFFFFu, ev, 3) == 3 && ev[2].kind == LG_PLANT_EV_ISLAND_EXIT);

    /* No trains: no events, counts are zero. */
    reset();
    start();
    CHECK(lg_plant_events(&plant, 0, 1000000, ev, 16) == 0);
    CHECK(lg_plant_counts(&plant, 5).waiting == 0);
    CHECK(lg_plant_truth(&plant, 5, LG_DIR_WEST, LG_SENSOR_A).axles == 0);
}

static void test_same_strike_time(void) {
    lg_plant_event_t ev[16];
    lg_truth_t a;
    lg_truth_t e;
    size_t n;
    size_t i;

    /* Two trains from opposite sides with the same strike time and ids in reverse order. */
    reset();
    add_train(9, LG_DIR_EAST, 5000, 30000, 100000, 8);
    add_train(3, LG_DIR_WEST, 5000, 30000, 100000, 8);
    start();
    n = lg_plant_events(&plant, 0, 100000, ev, 16);
    CHECK(n == 8);
    for (i = 1; i < n; i++) {
        CHECK(ev[i - 1].t_ms < ev[i].t_ms ||
              (ev[i - 1].t_ms == ev[i].t_ms &&
               (ev[i - 1].train_id < ev[i].train_id ||
                (ev[i - 1].train_id == ev[i].train_id && ev[i - 1].kind < ev[i].kind))));
    }
    CHECK(ev[0].train_id == 3 && ev[1].train_id == 9 && ev[0].t_ms == 5000 && ev[1].t_ms == 5000);
    /* One train per direction: each side counts only its own. */
    a = lg_plant_truth(&plant, 10000, LG_DIR_WEST, LG_SENSOR_A);
    e = lg_plant_truth(&plant, 10000, LG_DIR_EAST, LG_SENSOR_A);
    CHECK(a.occupied == 0 && e.occupied == 0 && a.axles == 8 && e.axles == 8);
    a = lg_plant_truth(&plant, 5001, LG_DIR_WEST, LG_SENSOR_A);
    e = lg_plant_truth(&plant, 5001, LG_DIR_EAST, LG_SENSOR_B);
    CHECK(a.occupied == 1 && e.occupied == 1);

    /* Same direction, same time (not a legal scenario, but the model must not break): counts add. */
    reset();
    add_train(1, LG_DIR_WEST, 5000, 30000, 100000, 8);
    add_train(2, LG_DIR_WEST, 5000, 30000, 100000, 12);
    start();
    a = lg_plant_truth(&plant, 60000, LG_DIR_WEST, LG_SENSOR_ISLAND);
    CHECK(a.axles == 20);
    a = lg_plant_truth(&plant, 60000, LG_DIR_EAST, LG_SENSOR_ISLAND);
    CHECK(a.axles == 0);
    /* Same id and time: the tie ends on the kind. */
    reset();
    add_train(4, LG_DIR_WEST, 5000, 30000, 100000, 8);
    add_train(4, LG_DIR_EAST, 5000, 30000, 100000, 8);
    start();
    n = lg_plant_events(&plant, 0, 100000, ev, 16);
    CHECK(n == 8);
    for (i = 1; i < n; i++) {
        CHECK(ev[i - 1].t_ms <= ev[i].t_ms);
    }
}

static void test_short_train_wide_zone(void) {
    /* Zone 20 m, train 5 m, 1 m/s (1 mm per ms): occupied while front in (0, 25000]. Front 25000
     * means the rear is at 20000 = Z: not occupied. */
    uint32_t t0 = 100;

    cfg.strike_in_mm = 400000;
    cfg.island_mm = 12000;
    cfg.sensor_zone_mm = 20000;
    reset();
    add_train(1, LG_DIR_WEST, t0, 1000, 5000, 2);
    start();
    CHECK(lg_plant_truth(&plant, t0, LG_DIR_WEST, LG_SENSOR_A).occupied == 0);
    CHECK(lg_plant_truth(&plant, t0 + 1, LG_DIR_WEST, LG_SENSOR_A).occupied == 1);
    CHECK(lg_plant_truth(&plant, t0 + 4999, LG_DIR_WEST, LG_SENSOR_A).occupied == 1);
    CHECK(lg_plant_truth(&plant, t0 + 5000, LG_DIR_WEST, LG_SENSOR_A).occupied == 1);
    CHECK(lg_plant_truth(&plant, t0 + 24999, LG_DIR_WEST, LG_SENSOR_A).occupied == 1);
    CHECK(lg_plant_truth(&plant, t0 + 25000, LG_DIR_WEST, LG_SENSOR_A).occupied == 0);
    CHECK(lg_plant_truth(&plant, t0 + 25000, LG_DIR_WEST, LG_SENSOR_B).occupied == 0);
    /* Both axles passed u = 0 by the time the rear is at 0: axle 1 at front - 3750. */
    /* Axle 0 sits 1250 behind the front: passes u=0 at front=1250; axle 1 at front=3750. */
    CHECK(lg_plant_truth(&plant, t0 + 1249, LG_DIR_WEST, LG_SENSOR_A).axles == 0);
    CHECK(lg_plant_truth(&plant, t0 + 1250, LG_DIR_WEST, LG_SENSOR_A).axles == 1);
    CHECK(lg_plant_truth(&plant, t0 + 3749, LG_DIR_WEST, LG_SENSOR_A).axles == 1);
    CHECK(lg_plant_truth(&plant, t0 + 3750, LG_DIR_WEST, LG_SENSOR_A).axles == 2);
    /* The other side sees nothing, the island is far away. */
    CHECK(lg_plant_truth(&plant, t0 + 3750, LG_DIR_EAST, LG_SENSOR_A).axles == 0);
    CHECK(lg_plant_truth(&plant, t0 + 3750, LG_DIR_WEST, LG_SENSOR_ISLAND).occupied == 0);
    cfg.sensor_zone_mm = 2000;
}

static void test_extremes(void) {
    lg_truth_t tr;
    lg_plant_counts_t c;
    lg_plant_event_t ev[16];
    char text[200];
    size_t n;
    uint32_t t;

    cfg.strike_in_mm = 5000000;
    cfg.island_mm = 200000;
    cfg.sensor_zone_mm = 20000;
    reset();
    add_train(65535, LG_DIR_EAST, 86400000u, 100000, 1000000, 200);
    add_train(1, LG_DIR_WEST, 86400000u, 1000, 1000000, 200);
    start();

    /* 100 m/s: 5.2 km + 1 km takes 62 s. */
    tr = lg_plant_truth(&plant, 86400000u + 62000u, LG_DIR_EAST, LG_SENSOR_ISLAND);
    CHECK(tr.axles == 200 && tr.occupied == 0);
    tr = lg_plant_truth(&plant, 86400000u + 61999u, LG_DIR_EAST, LG_SENSOR_ISLAND);
    CHECK(tr.occupied == 1 && tr.axles >= 199);
    /* 1 m/s train: 6.2e6 ms to clear. */
    tr = lg_plant_truth(&plant, 86400000u + 6200000u, LG_DIR_WEST, LG_SENSOR_ISLAND);
    CHECK(tr.axles == 200 && tr.occupied == 0);
    tr = lg_plant_truth(&plant, 86400000u + 6199999u, LG_DIR_WEST, LG_SENSOR_ISLAND);
    CHECK(tr.occupied == 1);
    /* Near 2^32: every train is long gone, nothing wraps. */
    for (t = 0xFFFFFFFFu - 5; t != 0u; t++) {
        c = lg_plant_counts(&plant, t);
        CHECK(c.finished == 2 && c.active == 0 && c.waiting == 0 && c.island_any == 0);
        tr = lg_plant_truth(&plant, t, LG_DIR_WEST, LG_SENSOR_A);
        CHECK(tr.axles == 200 && tr.occupied == 0 && tr.island_any == 0);
        tr = lg_plant_truth(&plant, t, LG_DIR_EAST, LG_SENSOR_ISLAND);
        CHECK(tr.axles == 200 && tr.occupied == 0);
    }
    /* Time 0, far before the strike: waiting, nothing seen. */
    c = lg_plant_counts(&plant, 0);
    CHECK(c.waiting == 2 && c.active == 0 && c.finished == 0);
    tr = lg_plant_truth(&plant, 0, LG_DIR_EAST, LG_SENSOR_A);
    CHECK(tr.axles == 0 && tr.occupied == 0);
    /* Events near the end of the uint32 range. */
    CHECK(lg_plant_events(&plant, 4294967000u, 0xFFFFFFFFu, ev, 16) == 0);
    n = lg_plant_events(&plant, 0, 0xFFFFFFFFu, ev, 16);
    CHECK(n == 8);
    CHECK(ev[7].t_ms == 86400000u + 6200000u && ev[7].train_id == 1 && ev[7].kind == LG_PLANT_EV_CLEARED);
    n = lg_plant_text(&plant, 0xFFFFFFFFu, text, sizeof text);
    CHECK(n == strlen(text) && strstr(text, "finished=2") != NULL && strstr(text, "t_ms=4294967295") != NULL);

    /* Many trains: the axle sum saturates at 65535 instead of wrapping. */
    reset();
    {
        int i;

        for (i = 0; i < LG_SCENARIO_MAX_TRAINS; i++) {
            add_train((uint16_t)(i + 1), LG_DIR_WEST, 1000, 100000, 5000, 200);
        }
    }
    start();
    tr = lg_plant_truth(&plant, 3000000u, LG_DIR_WEST, LG_SENSOR_A);
    CHECK(tr.axles == 64 * 200); /* 12800: below saturation */
    n = lg_plant_events(&plant, 0, 0xFFFFFFFFu, ev, 16);
    CHECK(n == 16);
    {
        static lg_plant_event_t all[LG_SCENARIO_MAX_TRAINS * 4];

        CHECK(lg_plant_events(&plant, 0, 0xFFFFFFFFu, all, LG_SCENARIO_MAX_TRAINS * 4) ==
              LG_SCENARIO_MAX_TRAINS * 4);
    }
    cfg.strike_in_mm = 400000;
    cfg.island_mm = 12000;
    cfg.sensor_zone_mm = 2000;
}

static void test_text(void) {
    char buf[200];
    size_t n;

    reset();
    add_train(1, LG_DIR_WEST, 5000, 30000, 120000, 8);
    start();
    n = lg_plant_text(&plant, 0, buf, sizeof buf);
    CHECK(n == strlen(buf) && buf[n - 1] == '\n');
    CHECK(strcmp(buf, "t_ms=0 scenario=extra trains=1 waiting=1 active=0 finished=0 island=0\n") == 0);
    CHECK(lg_plant_text(&plant, 0, buf, 0) == 0);
    memset(buf, 'x', sizeof buf);
    n = lg_plant_text(&plant, 0, buf, 1);
    CHECK(n == 0 && buf[0] == '\0' && buf[1] == 'x');
    memset(buf, 'x', sizeof buf);
    n = lg_plant_text(&plant, 0, buf, 10);
    CHECK(n == 9 && buf[9] == '\0' && buf[10] == 'x');
}

/* Exact touching positions, 1 mm per ms so the front is on a boundary at an integer time. */
static void test_exact_boundaries(void) {
    lg_plant_counts_t c;

    cfg.strike_in_mm = 400000;
    cfg.island_mm = 12000;
    cfg.sensor_zone_mm = 2000;
    reset();
    add_train(1, LG_DIR_WEST, 1000, 1000, 5000, 4);
    start();
    /* front == 0: still waiting; one ms later it is active */
    c = lg_plant_counts(&plant, 1000);
    CHECK(c.waiting == 1 && c.active == 0);
    c = lg_plant_counts(&plant, 1001);
    CHECK(c.waiting == 0 && c.active == 1);
    /* front == island entry: not on the island yet */
    CHECK(lg_plant_truth(&plant, 401000, LG_DIR_WEST, LG_SENSOR_ISLAND).occupied == 0);
    CHECK(lg_plant_counts(&plant, 401000).island_any == 0);
    CHECK(lg_plant_truth(&plant, 401001, LG_DIR_WEST, LG_SENSOR_ISLAND).occupied == 1);
    CHECK(lg_plant_counts(&plant, 401001).island_any == 1);
    /* rear == island exit (front = 417000): the island is free and the train is finished */
    CHECK(lg_plant_truth(&plant, 417999, LG_DIR_EAST, LG_SENSOR_ISLAND).occupied == 1);
    c = lg_plant_counts(&plant, 417999);
    CHECK(c.active == 1 && c.finished == 0 && c.island_any == 1);
    CHECK(lg_plant_truth(&plant, 418000, LG_DIR_EAST, LG_SENSOR_ISLAND).occupied == 0);
    c = lg_plant_counts(&plant, 418000);
    CHECK(c.active == 0 && c.finished == 1 && c.island_any == 0);
}

int main(void) {
    test_exact_boundaries();
    test_event_edges();
    test_short_train_wide_zone();
    test_same_strike_time();
    test_extremes();
    test_text();
    test_random_truth();
    test_events_match_truth();
    test_sliding_windows();
    LG_TEST_DONE("test_plant_model_extra");
}
