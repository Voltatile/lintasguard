#include <stdint.h>
#include <string.h>

#include "lg_gate_model.h"
#include "lg_test.h"

/*
 * Extra adversarial tests for core/gate_model.c: watchdog boundaries, time handling, command and
 * fault spam, integer limits, tiny buffers and a seeded random walk with invariants.
 * The last block holds regression tests for defects this suite found (the first version of
 * gate_model.c failed them); they are normal tests now.
 */

static lg_gate_cfg_t cfg;

#define POS_UP LG_GATE_POS_UP_MDEG
#define POS_DOWN LG_GATE_POS_DOWN_MDEG

/* Step every step_ms from t (exclusive) to t_end (inclusive, last step lands exactly on t_end). */
static uint32_t run_steps(lg_gate_model_t *m, uint32_t t, uint32_t t_end, uint32_t step_ms) {
    while ((uint32_t)(t_end - t) != 0) {
        uint32_t left = (uint32_t)(t_end - t);
        t += left < step_ms ? left : step_ms;
        lg_gate_model_step(m, t);
    }
    return t;
}

/* Like run_steps with 1 ms steps, and a controller heartbeat every 20 ms. */
static uint32_t run_fed(lg_gate_model_t *m, uint32_t t, uint32_t t_end) {
    while (t != t_end) {
        t++;
        if (t % 20 == 0) {
            lg_gate_model_feed(m, t);
        }
        lg_gate_model_step(m, t);
    }
    return t;
}

/* Raise fully with 1 ms steps and feeds; returns the time when UP. */
static uint32_t raise_up(lg_gate_model_t *m, uint32_t t0) {
    uint32_t t = t0;

    CHECK(lg_gate_model_command(m, LG_GATE_CMD_RAISE, t) == 0);
    while (m->state != LG_GATE_UP && (uint32_t)(t - t0) < 4 * m->cfg.travel_ms) {
        t++;
        if ((t - t0) % 20 == 0) {
            lg_gate_model_feed(m, t);
        }
        lg_gate_model_step(m, t);
    }
    CHECK(m->state == LG_GATE_UP);
    return t;
}

static void test_watchdog_boundary_matrix(void) {
    static const uint32_t wds[] = {1, 2, 10, 100, 1000};
    static const uint32_t starts[] = {0, 12345, 0xFFFFFFFFu - 50u, 0x7FFFFFF0u, 0x80000000u};
    size_t i;
    size_t j;

    for (i = 0; i < sizeof wds / sizeof wds[0]; i++) {
        for (j = 0; j < sizeof starts / sizeof starts[0]; j++) {
            lg_gate_model_t m;
            lg_gate_cfg_t c = cfg;
            uint32_t t;
            uint32_t T;

            c.watchdog_ms = wds[i];
            c.travel_ms = 200;
            lg_gate_model_init(&m, &c, starts[j]);
            t = starts[j];
            CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, t) == 0);
            for (; m.state != LG_GATE_UP && (uint32_t)(t - starts[j]) < 1000; t++) {
                lg_gate_model_feed(&m, t);
                lg_gate_model_step(&m, t + 1);
            }
            CHECK(m.state == LG_GATE_UP);
            T = t + 1; /* last feed */
            lg_gate_model_feed(&m, T);
            /* A single coarse step exactly at T + wd: no trip. One more ms: trip. */
            lg_gate_model_step(&m, T + wds[i]);
            CHECK(m.watchdog_trips == 0 && m.state == LG_GATE_UP && m.target == LG_GATE_TARGET_UP);
            lg_gate_model_step(&m, T + wds[i] + 1);
            CHECK(m.watchdog_trips == 1 && m.target == LG_GATE_TARGET_DOWN);
            CHECK(m.position_mdeg == POS_UP); /* no travel credited for the silent time */
            run_steps(&m, T + wds[i] + 1, T + wds[i] + 1 + 200, 1);
            CHECK(m.state == LG_GATE_DOWN && m.watchdog_trips == 1);
            CHECK(m.last_travel_ms == 200);
        }
    }
}

static void test_watchdog_huge_silent_gap(void) {
    uint32_t gaps[] = {101, 1000, 60000u, 3600000u, 86400000u, 0x7FFFFFFFu};
    size_t i;

    for (i = 0; i < sizeof gaps / sizeof gaps[0]; i++) {
        lg_gate_model_t m;
        uint32_t t_up;

        lg_gate_model_init(&m, &cfg, 0);
        t_up = raise_up(&m, 0);
        lg_gate_model_feed(&m, t_up);
        lg_gate_model_step(&m, t_up + gaps[i]); /* one single huge step */
        CHECK(m.watchdog_trips == 1);
        CHECK(m.target == LG_GATE_TARGET_DOWN);
        CHECK(m.position_mdeg == POS_UP); /* not an instant jump to DOWN, not a reported DOWN */
        CHECK(m.state == LG_GATE_LOWERING);
        CHECK(m.lights == 1 && m.bell == 1);
        run_steps(&m, t_up + gaps[i], t_up + gaps[i] + cfg.travel_ms, 1);
        CHECK(m.state == LG_GATE_DOWN && m.last_travel_ms == cfg.travel_ms);
    }
}

static void test_huge_step_while_moving(void) {
    lg_gate_model_t m;
    lg_gate_model_t m2;

    /* Command, then keep feeding, then one step of 2^31-1 ms: gate arrives, clamped, no overflow. */
    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    lg_gate_model_feed(&m, 0x7FFFFFFFu);
    lg_gate_model_step(&m, 0x7FFFFFFFu);
    CHECK(m.position_mdeg == POS_UP && m.state == LG_GATE_UP);
    CHECK(m.watchdog_trips == 0);
    CHECK(m.last_travel_ms == 0x7FFFFFFFu); /* measured from the command */

    /* Same with lowering from UP and a step far beyond the travel time. */
    lg_gate_model_init(&m2, &cfg, 0);
    (void)raise_up(&m2, 0);
    CHECK(lg_gate_model_command(&m2, LG_GATE_CMD_LOWER, 5000) == 0);
    lg_gate_model_step(&m2, 5000 + 3600000u);
    CHECK(m2.position_mdeg == POS_DOWN && m2.state == LG_GATE_DOWN);
}

static void test_same_timestamp_repeated(void) {
    lg_gate_model_t m;
    int i;
    int32_t pos;

    lg_gate_model_init(&m, &cfg, 500);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 500) == 0);
    lg_gate_model_feed(&m, 1500);
    lg_gate_model_step(&m, 1500);
    pos = m.position_mdeg;
    for (i = 0; i < 1000; i++) {
        lg_gate_model_step(&m, 1500);
        CHECK(m.position_mdeg == pos);
        CHECK(m.state == LG_GATE_RAISING);
    }
    /* Many commands at the same instant: no movement, no trip. */
    for (i = 0; i < 100; i++) {
        CHECK(lg_gate_model_command(&m, i % 2 ? LG_GATE_CMD_LIGHTS_ON : LG_GATE_CMD_RAISE, 1500) == 0);
    }
    CHECK(m.position_mdeg == pos && m.watchdog_trips == 0);
}

static void test_command_spam_reversal(void) {
    uint32_t period;

    for (period = 1; period <= 7; period += 2) {
        lg_gate_model_t m;
        uint32_t t;
        int32_t prev;

        lg_gate_model_init(&m, &cfg, 0);
        prev = m.position_mdeg;
        for (t = 1; t <= 6000; t++) {
            if (t % period == 0) {
                uint8_t cmd = ((t / period) % 2) ? LG_GATE_CMD_RAISE : LG_GATE_CMD_LOWER;
                CHECK(lg_gate_model_command(&m, cmd, t) == 0);
                CHECK(m.target == (cmd == LG_GATE_CMD_RAISE ? LG_GATE_TARGET_UP : LG_GATE_TARGET_DOWN));
                prev = m.position_mdeg; /* the command's own step still moved in the old direction */
            }
            lg_gate_model_step(&m, t);
            CHECK(m.position_mdeg >= POS_UP && m.position_mdeg <= POS_DOWN);
            if (m.target == LG_GATE_TARGET_DOWN) {
                CHECK(m.position_mdeg >= prev);
            } else {
                CHECK(m.position_mdeg <= prev);
            }
            prev = m.position_mdeg;
            CHECK(m.state != LG_GATE_UP || m.position_mdeg == POS_UP);
            CHECK(m.state != LG_GATE_DOWN || m.position_mdeg == POS_DOWN);
        }
        CHECK(m.watchdog_trips == 0); /* commands feed the watchdog */
        /* Controller falls silent: the gate ends DOWN within watchdog + travel (+ slack). */
        run_steps(&m, 6000, 6000 + cfg.watchdog_ms + 1 + cfg.travel_ms + 10, 1);
        CHECK(m.state == LG_GATE_DOWN && m.position_mdeg == POS_DOWN);
    }
}

static void test_reversal_midtravel_and_stop_at_height(void) {
    lg_gate_model_t m;
    int32_t at_reversal;

    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    run_fed(&m, 0, 2000);
    CHECK(m.position_mdeg == 45000);
    at_reversal = m.position_mdeg;
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_LOWER, 2000) == 0);
    CHECK(m.state == LG_GATE_LOWERING && m.position_mdeg == at_reversal);
    run_fed(&m, 2000, 3000);
    CHECK(m.position_mdeg > at_reversal); /* going down again */
    CHECK(m.state == LG_GATE_LOWERING);
    /* Reaching the end position from a partial travel reports a partial last_travel_ms. */
    run_fed(&m, 3000, 3000 + 2100);
    CHECK(m.state == LG_GATE_DOWN && m.position_mdeg == POS_DOWN);
    CHECK(m.last_travel_ms <= 2100 && m.last_travel_ms >= 1900);
}

static void test_invalid_commands_do_not_feed(void) {
    uint8_t bad[] = {0, 7, 8, 100, 255};
    lg_gate_model_t m;
    uint32_t t_up;
    uint32_t t;
    size_t i = 0;
    int ret;

    lg_gate_model_init(&m, &cfg, 0);
    t_up = raise_up(&m, 0);
    lg_gate_model_feed(&m, t_up);
    for (t = t_up + 1; t <= t_up + 1000 && m.watchdog_trips == 0; t++) {
        if (t % 5 == 0) {
            ret = lg_gate_model_command(&m, bad[i++ % sizeof bad], t);
            CHECK(ret == -1);
        }
        lg_gate_model_step(&m, t);
    }
    CHECK(m.watchdog_trips == 1);
    CHECK(t - 1 == t_up + cfg.watchdog_ms + 1 || t - 1 == t_up + cfg.watchdog_ms + 2 ||
          t - 1 == t_up + cfg.watchdog_ms + 5);
    CHECK(m.rejected > 0);
    /* Invalid commands also do not alter lights, bell or target. */
    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_BELL_OFF, 1) == 0);
    CHECK(lg_gate_model_command(&m, 0, 2) == -1 && m.bell == 0 && m.lights == 1);
    CHECK(lg_gate_model_command(&m, 7, 2) == -1 && m.bell == 0 && m.target == LG_GATE_TARGET_DOWN);
}

static void test_lights_bell_do_not_move(void) {
    lg_gate_model_t m;
    uint8_t cmds[] = {LG_GATE_CMD_LIGHTS_ON, LG_GATE_CMD_LIGHTS_OFF, LG_GATE_CMD_BELL_ON,
                      LG_GATE_CMD_BELL_OFF};
    uint32_t t;
    size_t i;

    lg_gate_model_init(&m, &cfg, 0);
    for (t = 1; t < 2000; t++) {
        lg_gate_model_command(&m, cmds[t % 4], t);
        lg_gate_model_step(&m, t);
        CHECK(m.state == LG_GATE_DOWN && m.position_mdeg == POS_DOWN);
        CHECK(m.target == LG_GATE_TARGET_DOWN);
    }
    (void)i;
    /* While raising, lights and bell commands do not change the motion. */
    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    for (t = 1; t <= 1000; t++) {
        lg_gate_model_command(&m, cmds[t % 4], t);
    }
    CHECK(m.position_mdeg == POS_DOWN - 22500); /* 1000 ms of 4000 ms */
    CHECK(m.target == LG_GATE_TARGET_UP);
}

static void test_commands_while_fault(void) {
    lg_gate_model_t m;
    uint32_t t;

    lg_gate_model_init(&m, &cfg, 0);
    (void)raise_up(&m, 0);
    t = 4100;
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_STUCK_UP, 0, t) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_LOWER, t) == 0);
    for (; t <= 4100 + 13000; t++) {
        lg_gate_model_feed(&m, t);
        lg_gate_model_step(&m, t);
    }
    CHECK(m.state == LG_GATE_FAULT && m.position_mdeg == POS_UP);
    CHECK(m.current_ma == cfg.current_moving_ma); /* energised against the obstruction */
    /* RAISE while STUCK_UP and in FAULT: target UP, already UP: leaves FAULT, reports UP. */
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, t) == 0);
    CHECK(m.state == LG_GATE_UP && m.position_mdeg == POS_UP);
    CHECK(m.current_ma == cfg.current_hold_ma);
    /* Another LOWER gives FAULT again after the timeout, never DOWN. */
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_LOWER, t) == 0);
    for (; t <= 4100 + 13000 + 13000; t++) {
        lg_gate_model_feed(&m, t);
        lg_gate_model_step(&m, t);
        CHECK(m.state != LG_GATE_DOWN);
    }
    CHECK(m.state == LG_GATE_FAULT);
}

static void test_fault_toggle_spam(void) {
    lg_gate_model_t m;
    uint32_t t;
    int32_t prev;
    uint8_t modes[] = {LG_GFAULT_STUCK_UP, LG_GFAULT_NONE, LG_GFAULT_STUCK_DOWN, LG_GFAULT_NONE,
                       LG_GFAULT_SLOW, LG_GFAULT_HIGH_CURRENT};

    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    prev = m.position_mdeg;
    for (t = 1; t <= 20000; t++) {
        if (t % 3 == 0) {
            uint8_t mode = modes[(t / 3) % sizeof modes];
            int32_t before;
            lg_gate_model_step(&m, t);
            before = m.position_mdeg;
            CHECK(lg_gate_model_set_fault(&m, mode, 100 + (t % 900), t) == 0);
            CHECK(m.position_mdeg == before); /* fault injection does not teleport the gate */
            CHECK(m.fault == mode);
        }
        if (t % 50 == 0) {
            lg_gate_model_feed(&m, t);
        }
        lg_gate_model_step(&m, t);
        CHECK(m.position_mdeg >= POS_UP && m.position_mdeg <= POS_DOWN);
        CHECK(m.state != LG_GATE_UP || m.position_mdeg == POS_UP);
        CHECK(m.state != LG_GATE_DOWN || m.position_mdeg == POS_DOWN);
        CHECK(m.target == LG_GATE_TARGET_UP ? m.position_mdeg <= prev : 1);
        prev = m.position_mdeg;
        if (m.fault == LG_GFAULT_STUCK_DOWN && m.target == LG_GATE_TARGET_UP) {
            CHECK(m.moving == 0);
        }
    }
    /* Clear the fault: the gate completes. */
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_NONE, 0, t) == 0);
    for (; t <= 20000 + 2 * cfg.travel_ms; t++) {
        if (t % 50 == 0) {
            lg_gate_model_feed(&m, t);
        }
        lg_gate_model_step(&m, t);
    }
    CHECK(m.state == LG_GATE_UP);
}

static void test_fault_same_instant_as_command(void) {
    lg_gate_model_t m;

    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_STUCK_DOWN, 0, 100) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 100) == 0);
    run_fed(&m, 100, 600);
    CHECK(m.position_mdeg == POS_DOWN && m.state == LG_GATE_RAISING);
    /* Command first, fault at the same instant. */
    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 100) == 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_STUCK_DOWN, 0, 100) == 0);
    run_fed(&m, 100, 600);
    CHECK(m.position_mdeg == POS_DOWN);
    /* STUCK_UP on a gate that is already DOWN with target DOWN: nothing happens. */
    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_STUCK_UP, 0, 5) == 0);
    run_steps(&m, 5, 20000, 10);
    CHECK(m.state == LG_GATE_DOWN && m.position_mdeg == POS_DOWN);
}

static void test_slow_factor_change_mid_movement(void) {
    lg_gate_model_t m;
    int32_t p1;
    int32_t p2;

    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    run_fed(&m, 0, 1000);
    CHECK(m.position_mdeg == POS_DOWN - 22500);
    p1 = m.position_mdeg;
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_SLOW, 200, 1000) == 0);
    CHECK(m.position_mdeg == p1);
    run_fed(&m, 1000, 2000); /* now half speed: 11250 mdeg per 1000 ms */
    p2 = m.position_mdeg;
    CHECK(p1 - p2 == 11250);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_SLOW, 1000, 2000) == 0);
    run_fed(&m, 2000, 3000); /* 1/10 speed: 2250 per 1000 ms */
    CHECK(p2 - m.position_mdeg == 2250);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_NONE, 0, 3000) == 0);
    p1 = m.position_mdeg;
    run_fed(&m, 3000, 4000);
    CHECK(p1 - m.position_mdeg == 22500);
}

static void test_no_drift_small_vs_coarse_steps(void) {
    lg_gate_model_t a;
    lg_gate_model_t b;
    lg_gate_cfg_t c = cfg;
    uint32_t t;
    uint32_t ta = 0;
    uint32_t tb = 0;
    int i;

    /* Exactness: position(t) == floor(t * 90000 / eff) for any step size (SLOW 1000: eff 40000). */
    lg_gate_model_init(&a, &c, 0);
    lg_gate_model_init(&b, &c, 0);
    CHECK(lg_gate_model_set_fault(&a, LG_GFAULT_SLOW, 1000, 0) == 0);
    CHECK(lg_gate_model_set_fault(&b, LG_GFAULT_SLOW, 1000, 0) == 0);
    CHECK(lg_gate_model_command(&a, LG_GATE_CMD_RAISE, 0) == 0);
    CHECK(lg_gate_model_command(&b, LG_GATE_CMD_RAISE, 0) == 0);
    for (t = 1; t <= 39999; t++) {
        lg_gate_model_feed(&a, t);
        lg_gate_model_step(&a, t);
        ta = t;
        if (t % 7 == 0) {
            lg_gate_model_feed(&b, t);
            lg_gate_model_step(&b, t);
            tb = t;
        }
        CHECK(POS_DOWN - a.position_mdeg == (int32_t)((uint64_t)ta * 90000u / 40000u));
        CHECK(POS_DOWN - b.position_mdeg == (int32_t)((uint64_t)tb * 90000u / 40000u));
    }
    lg_gate_model_feed(&a, 40000);
    lg_gate_model_step(&a, 40000);
    CHECK(a.state == LG_GATE_UP && a.last_travel_ms == 40000);
    for (t = 40000; b.state != LG_GATE_UP && t < 50000; t += 7) {
        lg_gate_model_feed(&b, tb + 7);
        lg_gate_model_step(&b, tb + 7);
        tb += 7;
        t = tb;
    }
    CHECK(b.state == LG_GATE_UP && b.last_travel_ms == 40005); /* first multiple of 7 >= 40000 */

    /* 100000 steps of 1 ms in repeated full cycles: every cycle takes exactly travel_ms. */
    lg_gate_model_init(&a, &c, 0);
    t = 0;
    for (i = 0; i < 12; i++) {
        uint8_t cmd = (i % 2 == 0) ? LG_GATE_CMD_RAISE : LG_GATE_CMD_LOWER;
        uint32_t t0 = t;
        CHECK(lg_gate_model_command(&a, cmd, t) == 0);
        while ((i % 2 == 0 ? a.state != LG_GATE_UP : a.state != LG_GATE_DOWN) && t - t0 < 10000) {
            t++;
            if (t % 20 == 0) {
                lg_gate_model_feed(&a, t);
            }
            lg_gate_model_step(&a, t);
        }
        CHECK(a.last_travel_ms == c.travel_ms);
        CHECK(t - t0 == c.travel_ms);
    }
    /* A long idle at the end position with 1 ms steps (100000 steps): nothing drifts or trips. */
    for (i = 0; i < 100000; i++) {
        lg_gate_model_step(&a, ++t);
    }
    CHECK(a.state == LG_GATE_DOWN && a.position_mdeg == POS_DOWN && a.watchdog_trips == 0);
}

static void test_fault_timeout_boundaries_and_wrap(void) {
    lg_gate_model_t m;
    uint32_t t0 = 0xFFFFFFFFu - 5000u; /* the FAULT timeout (12000 ms) is crossed after the wrap */
    uint32_t t;
    uint32_t pct;

    lg_gate_model_init(&m, &cfg, t0);
    (void)raise_up(&m, t0);
    t = t0 + 4100;
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_STUCK_UP, 0, t) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_LOWER, t) == 0);
    {
        uint32_t t1 = t;
        for (; (uint32_t)(t - t1) < 11999; ) {
            t++;
            lg_gate_model_feed(&m, t);
            lg_gate_model_step(&m, t);
            CHECK(m.state == LG_GATE_LOWERING);
        }
        t++;
        lg_gate_model_feed(&m, t);
        lg_gate_model_step(&m, t);
        CHECK(m.state == LG_GATE_FAULT); /* exactly timeout_factor * travel_ms after the command */
    }
    /* Slow but within 3x nominal: never FAULT. Beyond 3x: FAULT. */
    for (pct = 100; pct <= 400; pct += 100) {
        uint32_t u;
        int saw = 0;
        lg_gate_model_init(&m, &cfg, 0);
        CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_SLOW, pct, 0) == 0);
        CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
        for (u = 1; u <= 5 * cfg.travel_ms; u++) {
            if (u % 20 == 0) {
                lg_gate_model_feed(&m, u);
            }
            lg_gate_model_step(&m, u);
            saw |= m.state == LG_GATE_FAULT;
        }
        CHECK(saw == (pct > 300));
    }
}

static void test_feed_in_future_and_clock_back(void) {
    lg_gate_model_t m;
    int32_t pos;

    /* A feed stamped slightly after the step time (two threads) is not an elapsed silence. */
    lg_gate_model_init(&m, &cfg, 0);
    (void)raise_up(&m, 0);
    lg_gate_model_feed(&m, 5000 + 50);
    lg_gate_model_step(&m, 5000 + 10);
    CHECK(m.watchdog_trips == 0 && m.state == LG_GATE_UP);

    /* The clock steps back by 1 ms and by a lot: the model does not move or jump. */
    lg_gate_model_init(&m, &cfg, 10000);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 10000) == 0);
    run_fed(&m, 10000, 11000);
    pos = m.position_mdeg;
    lg_gate_model_step(&m, 10999);
    CHECK(m.position_mdeg == pos && m.state == LG_GATE_RAISING && m.watchdog_trips == 0);
    lg_gate_model_step(&m, 5);
    CHECK(m.position_mdeg == pos && m.state == LG_GATE_RAISING && m.watchdog_trips == 0);
    lg_gate_model_step(&m, 11001);
    CHECK(m.position_mdeg == pos - 22 || m.position_mdeg == pos - 23);
}

static void test_integer_limits(void) {
    lg_gate_model_t m;
    lg_gate_state_msg_t msg;
    lg_gate_cfg_t c;

    /* SLOW 1000 percent. */
    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_SLOW, LG_GATE_FACTOR_MAX_PCT, 0) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    {
        uint32_t t;
        int saw_fault = 0;
        for (t = 1; t <= 10 * cfg.travel_ms + 5; t++) {
            if (t % 50 == 0) {
                lg_gate_model_feed(&m, t);
            }
            lg_gate_model_step(&m, t);
            saw_fault |= m.state == LG_GATE_FAULT;
            CHECK(m.position_mdeg >= POS_UP && m.position_mdeg <= POS_DOWN);
        }
        CHECK(saw_fault && m.state == LG_GATE_UP && m.last_travel_ms == 10 * cfg.travel_ms);
    }
    /* HIGH_CURRENT 1000 percent. */
    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_HIGH_CURRENT, 1000, 0) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    run_steps(&m, 0, 100, 1);
    CHECK(m.current_ma == cfg.current_moving_ma * 10);
    lg_gate_model_fill_state(&m, &msg);
    CHECK(msg.current_ma == cfg.current_moving_ma * 10);

    /* Current above 16 bit saturates in the message, not wraps. */
    c = cfg;
    c.current_moving_ma = 100000;
    lg_gate_model_init(&m, &c, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_HIGH_CURRENT, 1000, 0) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    run_steps(&m, 0, 10, 1);
    CHECK(m.current_ma == 1000000u);
    lg_gate_model_fill_state(&m, &msg);
    CHECK(msg.current_ma == 65535);

    /* Zero travel time config: no division by zero, completes at once. */
    c = cfg;
    c.travel_ms = 0;
    lg_gate_model_init(&m, &c, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    lg_gate_model_step(&m, 1);
    CHECK(m.position_mdeg == POS_UP);

    /* Very long travel time: position only moves, never leaves the range. */
    c = cfg;
    c.travel_ms = 0xFFFFFFFFu;
    lg_gate_model_init(&m, &c, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_SLOW, 1000, 0) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    lg_gate_model_feed(&m, 0x7FFFFFFFu);
    lg_gate_model_step(&m, 0x7FFFFFFFu);
    CHECK(m.position_mdeg >= POS_UP && m.position_mdeg <= POS_DOWN);
}

static void test_state_name_and_msg_extremes(void) {
    lg_gate_model_t m;
    lg_gate_state_msg_t msg;

    lg_gate_model_init(&m, &cfg, 0);
    m.current_ma = 0xFFFFFFFFu;
    m.last_travel_ms = 0xFFFFFFFFu;
    memset(&msg, 0xAA, sizeof msg);
    lg_gate_model_fill_state(&m, &msg);
    CHECK(msg.current_ma == 65535);
    CHECK(msg.travel_ms == 0xFFFFFFFFu);
    CHECK(msg.position_deg == 90);
    CHECK(msg.reserved == 0 && msg.reserved2 == 0 && msg.reserved3 == 0); /* no stack garbage */
    m.current_ma = 65535;
    lg_gate_model_fill_state(&m, &msg);
    CHECK(msg.current_ma == 65535);
    m.current_ma = 65536;
    lg_gate_model_fill_state(&m, &msg);
    CHECK(msg.current_ma == 65535);
    m.position_mdeg = 0;
    lg_gate_model_fill_state(&m, &msg);
    CHECK(msg.position_deg == 0);
}

static void test_text_tiny_buffers(void) {
    lg_gate_model_t m;
    char big[400];
    char buf[64];
    size_t len;
    size_t n;
    size_t full;

    lg_gate_model_init(&m, &cfg, 0);
    m.current_ma = 0xFFFFFFFFu;
    m.last_travel_ms = 0xFFFFFFFFu;
    m.fault_pct = 0xFFFFFFFFu;
    m.watchdog_trips = 0xFFFFFFFFu;
    m.rejected = 0xFFFFFFFFu;
    m.state = 200; /* unknown */
    m.fault = 200;
    m.lights = 255;
    m.bell = 255;
    full = lg_gate_model_text(&m, big, sizeof big);
    CHECK(full == strlen(big) && full < sizeof big - 1 && big[full - 1] == '\n');
    CHECK(strstr(big, "state=UNKNOWN") != NULL && strstr(big, "fault=UNKNOWN") != NULL);
    CHECK(strstr(big, "current_ma=4294967295") != NULL);
    CHECK(strstr(big, "lights=255") != NULL);

    CHECK(lg_gate_model_text(&m, NULL, 0) == 0); /* len 0: nothing written, no crash */
    for (len = 1; len <= 40; len++) {
        memset(buf, 0x5A, sizeof buf);
        n = lg_gate_model_text(&m, buf, len);
        CHECK(n < len);
        CHECK(buf[n] == '\0');
        CHECK(strlen(buf) == n);
        CHECK(memcmp(buf, big, n) == 0);              /* a prefix of the full line */
        CHECK((unsigned char)buf[len] == 0x5A);       /* no write past the buffer */
        CHECK((unsigned char)buf[len + 1] == 0x5A);
    }
    /* Exactly the needed size: full line fits (with NUL), one less truncates the newline. */
    memset(buf, 0x5A, sizeof buf);
    {
        char exact[400];
        n = lg_gate_model_text(&m, exact, full + 1);
        CHECK(n == full && exact[full - 1] == '\n');
        n = lg_gate_model_text(&m, exact, full);
        CHECK(n == full - 1 && exact[n] == '\0');
    }
}

/* ---- seeded random walk ---------------------------------------------------------------- */

static uint32_t rng_state;

static uint32_t rnd(void) {
    uint32_t x = rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

typedef struct {
    uint32_t last_feed; /* shadow of the last valid command or feed */
    uint32_t trips;
    uint32_t invalid;
    int32_t pos;
} shadow_t;

static void check_invariants(const lg_gate_model_t *m, shadow_t *s, uint32_t now, int after_step,
                             int32_t pos_before, uint32_t trips_before, uint32_t seed, long iter) {
    int ok = 1;
    uint32_t moving_ma = m->cfg.current_moving_ma;
    uint32_t hc_ma = (uint32_t)((uint64_t)moving_ma * m->fault_pct / 100u);

    ok &= m->position_mdeg >= POS_UP && m->position_mdeg <= POS_DOWN;
    ok &= m->state != LG_GATE_UP || (m->position_mdeg == POS_UP && m->target == LG_GATE_TARGET_UP);
    ok &= m->state != LG_GATE_DOWN ||
          (m->position_mdeg == POS_DOWN && m->target == LG_GATE_TARGET_DOWN);
    ok &= !(m->position_mdeg == POS_UP && m->target == LG_GATE_TARGET_UP) || m->state == LG_GATE_UP;
    ok &= !(m->position_mdeg == POS_DOWN && m->target == LG_GATE_TARGET_DOWN) ||
          m->state == LG_GATE_DOWN;
    ok &= m->state != LG_GATE_FAULT || (m->position_mdeg != (m->target ? POS_UP : POS_DOWN));
    ok &= m->state != LG_GATE_LOWERING || m->target == LG_GATE_TARGET_DOWN;
    ok &= m->state != LG_GATE_RAISING || m->target == LG_GATE_TARGET_UP;
    ok &= m->state <= LG_GATE_FAULT && m->target <= 1;
    ok &= m->watchdog_trips >= trips_before;
    ok &= m->rejected == s->invalid;
    ok &= !m->moving || m->state == LG_GATE_LOWERING || m->state == LG_GATE_RAISING ||
          m->state == LG_GATE_FAULT;
    ok &= m->current_ma == m->cfg.current_hold_ma || m->current_ma == moving_ma ||
          m->current_ma == hc_ma;
    if (after_step) {
        /* Direction: the step never moves the gate against its target. */
        if (m->target == LG_GATE_TARGET_DOWN) {
            ok &= m->position_mdeg >= pos_before || m->watchdog_trips > trips_before;
        } else {
            ok &= m->position_mdeg <= pos_before;
        }
        /* A blocked motor does not move. */
        if ((m->target == LG_GATE_TARGET_DOWN && m->fault == LG_GFAULT_STUCK_UP) ||
            (m->target == LG_GATE_TARGET_UP && m->fault == LG_GFAULT_STUCK_DOWN)) {
            ok &= m->position_mdeg == pos_before;
        }
        /* Silent for more than the watchdog time and still target UP: violation. */
        ok &= !(m->target == LG_GATE_TARGET_UP &&
                (uint32_t)(now - s->last_feed) > m->cfg.watchdog_ms &&
                (uint32_t)(now - s->last_feed) < 0x80000000u);
        /* A trip forces lights and bell on and target DOWN. */
        if (m->watchdog_trips > trips_before) {
            ok &= m->lights == 1 && m->bell == 1 && m->target == LG_GATE_TARGET_DOWN;
            ok &= m->watchdog_trips == trips_before + 1;
            ok &= (uint32_t)(now - s->last_feed) > m->cfg.watchdog_ms;
        }
    }
    if (!ok) {
        printf("invariant violated seed=%u iter=%ld now=%u pos=%d state=%d target=%d fault=%d\n",
               (unsigned)seed, iter, (unsigned)now, (int)m->position_mdeg, m->state, m->target,
               m->fault);
    }
    CHECK(ok);
}

static void random_walk(uint32_t seed, uint32_t start, long iters, int fast_cfg) {
    lg_gate_model_t m;
    lg_gate_cfg_t c = cfg;
    shadow_t s;
    uint32_t now = start;
    long i;

    if (fast_cfg) {
        c.travel_ms = 300;
        c.watchdog_ms = 40;
    }
    rng_state = seed;
    lg_gate_model_init(&m, &c, now);
    s.last_feed = now;
    s.trips = 0;
    s.invalid = 0;
    s.pos = m.position_mdeg;

    for (i = 0; i < iters; i++) {
        uint32_t r = rnd();
        uint32_t trips_before = m.watchdog_trips;
        int32_t pos_before = m.position_mdeg;
        uint32_t adv;
        uint32_t pick = r % 100;

        /* time advance: mostly small, sometimes zero, sometimes big */
        switch ((r >> 8) % 10) {
        case 0: adv = 0; break;
        case 1: adv = 100 + (rnd() % 100000); break;
        case 2: adv = 1; break;
        default: adv = 1 + (rnd() % 30); break;
        }
        now += adv;

        if (pick < 25) {
            uint8_t cmd = (uint8_t)(rnd() % 9);
            int ret;
            if (rnd() % 20 == 0) {
                cmd = (uint8_t)(rnd() % 256);
            }
            ret = lg_gate_model_command(&m, cmd, now);
            if (cmd >= 1 && cmd <= 6) {
                CHECK(ret == 0);
                s.last_feed = now;
            } else {
                CHECK(ret == -1);
                s.invalid++;
            }
            /* command steps internally: check state invariants but not the silent-time rule */
            pos_before = m.position_mdeg;
            check_invariants(&m, &s, now, 0, pos_before, m.watchdog_trips, seed, i);
        } else if (pick < 45) {
            lg_gate_model_feed(&m, now);
            s.last_feed = now;
            lg_gate_model_step(&m, now);
            check_invariants(&m, &s, now, 1, pos_before, trips_before, seed, i);
        } else if (pick < 52) {
            uint8_t mode = (uint8_t)(rnd() % 8);
            uint32_t factor = 90 + (rnd() % 1015);
            int ret;
            lg_gate_model_step(&m, now);
            check_invariants(&m, &s, now, 1, pos_before, trips_before, seed, i);
            pos_before = m.position_mdeg;
            trips_before = m.watchdog_trips;
            ret = lg_gate_model_set_fault(&m, mode, factor, now);
            if (mode == LG_GFAULT_NONE || mode == LG_GFAULT_STUCK_UP || mode == LG_GFAULT_STUCK_DOWN) {
                CHECK(ret == 0);
            } else if (mode == LG_GFAULT_SLOW || mode == LG_GFAULT_HIGH_CURRENT) {
                CHECK((ret == 0) == (factor >= 100 && factor <= 1000));
            } else {
                CHECK(ret == -1);
            }
            CHECK(m.position_mdeg == pos_before);
            check_invariants(&m, &s, now, 0, pos_before, trips_before, seed, i);
        } else {
            lg_gate_model_step(&m, now);
            check_invariants(&m, &s, now, 1, pos_before, trips_before, seed, i);
        }
    }
}

static void test_random_walk(void) {
    random_walk(0x1234ABCDu, 0, 40000, 0);
    random_walk(0xCAFEBABEu, 0xFFFFF000u, 40000, 1); /* crosses the 32-bit wrap */
    random_walk(0x0BADF00Du, 0x7FFFF000u, 40000, 1);
    random_walk(0x00C0FFEEu, 1000, 40000, 0);
}

/* ---- defects found by the tester agent, fixed in gate_model.c ----------------------- */

static void test_known_watchdog_max_config(void) {
    lg_gate_model_t m;
    lg_gate_cfg_t c = cfg;

    /* watchdog_ms + 1u wraps to 0 for watchdog_ms == UINT32_MAX: instant spurious trip. */
    c.watchdog_ms = 0xFFFFFFFFu;
    lg_gate_model_init(&m, &c, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    lg_gate_model_step(&m, 1);
    CHECK(m.watchdog_trips == 0);
}

static void test_known_fault_resets_travel_timer(void) {
    lg_gate_model_t m;

    /* set_fault restarts move_start_ms: last_travel_ms of the whole movement is under-reported. */
    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    run_fed(&m, 0, 1000);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_NONE, 0, 1000) == 0);
    run_fed(&m, 1000, 4000);
    CHECK(m.state == LG_GATE_UP);
    CHECK(m.last_travel_ms == 4000);
}

static void test_known_repeated_fault_hides_fault_state(void) {
    lg_gate_model_t m;
    uint32_t t;
    int saw_fault = 0;

    /* Re-injecting STUCK_UP every second restarts the FAULT timeout: FAULT is never reported. */
    lg_gate_model_init(&m, &cfg, 0);
    (void)raise_up(&m, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_STUCK_UP, 0, 4100) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_LOWER, 4100) == 0);
    for (t = 4101; t < 4100 + 40000; t++) {
        lg_gate_model_feed(&m, t);
        if ((t - 4100) % 1000 == 0) {
            CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_STUCK_UP, 0, t) == 0);
        }
        lg_gate_model_step(&m, t);
        saw_fault |= m.state == LG_GATE_FAULT;
    }
    CHECK(saw_fault);
}

static void test_known_remainder_dropped_on_fault_spam(void) {
    lg_gate_model_t m;
    lg_gate_cfg_t c = cfg;
    uint32_t t;

    /* Slow actuator (100 s travel = 0.9 mdeg/ms): set_fault every ms zeroes move_acc, so the
     * fractional progress is lost every time and the gate never moves. */
    c.travel_ms = 100000;
    lg_gate_model_init(&m, &c, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    for (t = 1; t <= 10000; t++) {
        lg_gate_model_feed(&m, t);
        lg_gate_model_step(&m, t);
        CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_NONE, 0, t) == 0);
    }
    CHECK(m.position_mdeg < POS_DOWN - 8000);
}

static void test_known_current_truncation(void) {
    lg_gate_model_t m;
    lg_gate_cfg_t c = cfg;

    c.current_moving_ma = 0xFFFFFFFFu;
    lg_gate_model_init(&m, &c, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_HIGH_CURRENT, 200, 0) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    lg_gate_model_step(&m, 10);
    CHECK(m.current_ma >= c.current_moving_ma); /* high current must not read lower than nominal */
}

int main(void) {
    cfg = lg_gate_cfg_default();
    test_watchdog_boundary_matrix();
    test_watchdog_huge_silent_gap();
    test_huge_step_while_moving();
    test_same_timestamp_repeated();
    test_command_spam_reversal();
    test_reversal_midtravel_and_stop_at_height();
    test_invalid_commands_do_not_feed();
    test_lights_bell_do_not_move();
    test_commands_while_fault();
    test_fault_toggle_spam();
    test_fault_same_instant_as_command();
    test_slow_factor_change_mid_movement();
    test_no_drift_small_vs_coarse_steps();
    test_fault_timeout_boundaries_and_wrap();
    test_feed_in_future_and_clock_back();
    test_integer_limits();
    test_state_name_and_msg_extremes();
    test_text_tiny_buffers();
    test_random_walk();
    test_known_watchdog_max_config();
    test_known_fault_resets_travel_timer();
    test_known_repeated_fault_hides_fault_state();
    test_known_remainder_dropped_on_fault_spam();
    test_known_current_truncation();
    LG_TEST_DONE("test_gate_model_extra");
}
