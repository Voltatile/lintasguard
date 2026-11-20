/* Extra coverage for voting, clearing, mode manager and metrics (Step 4). Safety direction first. */
#include <stdint.h>
#include <string.h>

#include "lg_metrics.h"
#include "lg_mode.h"
#include "lg_test.h"
#include "lg_voting.h"

#define TS (LG_T_STABLE_MS)

static lg_mode_in_t healthy(void) {
    lg_mode_in_t in;

    memset(&in, 0, sizeof in);
    lg_mode_in_all_healthy(&in);
    return in;
}

static int comp(int dir, int id) {
    return LG_COMP_SENSOR_BASE + dir * LG_SENSOR_ID_COUNT + id;
}
static int proc(int dir, int id) {
    return LG_PROC_SENSOR_BASE + dir * LG_SENSOR_ID_COUNT + id;
}

/* Boot and release into level 0, starting at now. Returns the time after release. */
static uint32_t boot_to_normal(lg_mode_t *m, const lg_mode_cfg_t *cfg, uint32_t now) {
    lg_mode_in_t in = healthy();
    uint32_t t;

    lg_mode_init(m, now);
    for (t = 0; t <= cfg->t_boot_stable_ms + 100; t += 100) {
        lg_mode_step(m, cfg, &in, now + t);
    }
    return now + t;
}

/* Run the manager with constant input for dur ms in 100 ms steps; returns the end time. */
static uint32_t run(lg_mode_t *m, const lg_mode_cfg_t *cfg, const lg_mode_in_t *in, uint32_t now,
                    uint32_t dur) {
    uint32_t t;

    for (t = 0; t < dur; t += 100) {
        lg_mode_step(m, cfg, in, now + t);
    }
    return now + dur;
}

/* ---- required level ---------------------------------------------------- */

static void test_required_extra(void) {
    lg_mode_in_t in;
    uint8_t why = 0;
    int p;
    int dir;

    /* Two directions fail independently: one lost approach pair in each. */
    in = healthy();
    in.verdict[comp(0, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    CHECK(lg_mode_required(&in, &why) == LG_LEVEL_DEGRADED);
    in.verdict[comp(1, LG_SENSOR_B)] = LG_VERDICT_FAILED;
    CHECK(lg_mode_required(&in, &why) == LG_LEVEL_DEGRADED); /* different directions: still redundant */
    in.verdict[comp(1, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    CHECK(lg_mode_required(&in, &why) == LG_LEVEL_UNCERTAIN); /* east now has none */
    in.schedule_ok[LG_DIR_EAST] = 0;
    CHECK(lg_mode_required(&in, &why) == LG_LEVEL_FAILSAFE);
    CHECK(why == LG_REASON_DETECT_LOST_NO_INFO);

    /* Schedule of the other direction must not matter. */
    in = healthy();
    in.verdict[comp(0, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    in.verdict[comp(0, LG_SENSOR_B)] = LG_VERDICT_FAILED;
    in.schedule_ok[LG_DIR_EAST] = 0;
    CHECK(lg_mode_required(&in, &why) == LG_LEVEL_UNCERTAIN);
    in.schedule_ok[LG_DIR_WEST] = 0;
    CHECK(lg_mode_required(&in, &why) == LG_LEVEL_FAILSAFE);

    /* Sensor process RESTARTING or DEAD counts as lost even when verdict is OK. */
    for (dir = 0; dir < LG_DIR_COUNT; dir++) {
        int id;
        for (id = 0; id < LG_SENSOR_ID_COUNT; id++) {
            uint8_t st;
            for (st = LG_PROC_RESTARTING; st <= LG_PROC_DEAD; st++) {
                uint8_t want = id == LG_SENSOR_ISLAND ? LG_LEVEL_UNCERTAIN : LG_LEVEL_DEGRADED;

                in = healthy();
                in.proc_state[proc(dir, id)] = st;
                CHECK(lg_mode_required(&in, &why) == want);
            }
        }
    }

    /* Island lost without schedule is level 4. */
    in = healthy();
    in.verdict[comp(0, LG_SENSOR_ISLAND)] = LG_VERDICT_FAILED;
    in.schedule_ok[0] = 0;
    CHECK(lg_mode_required(&in, &why) == LG_LEVEL_FAILSAFE);

    /* Island sensor WARNING is only level 1. */
    in = healthy();
    in.verdict[comp(1, LG_SENSOR_ISLAND)] = LG_VERDICT_WARNING;
    CHECK(lg_mode_required(&in, &why) == LG_LEVEL_EARLY_WARNING);

    /* Every non-sensor process: RESTARTING vs DEAD. */
    for (p = 0; p < LG_PROC_COUNT; p++) {
        uint8_t st;
        if (p >= LG_PROC_SENSOR_BASE && p < LG_PROC_PLANT) {
            continue;
        }
        for (st = LG_PROC_RESTARTING; st <= LG_PROC_DEAD; st++) {
            uint8_t want;
            in = healthy();
            in.proc_state[p] = st;
            if (st == LG_PROC_DEAD &&
                (p == LG_PROC_CONTROLLER || p == LG_PROC_SUPERVISOR || p == LG_PROC_GATE)) {
                want = LG_LEVEL_FAILSAFE;
            } else if (p == LG_PROC_BRIDGE) {
                want = LG_LEVEL_EARLY_WARNING;
            } else {
                want = LG_LEVEL_DEGRADED;
            }
            CHECK(lg_mode_required(&in, &why) == want);
        }
    }

    /* Battery. */
    in = healthy();
    in.verdict[LG_COMP_BATTERY] = LG_VERDICT_WARNING;
    CHECK(lg_mode_required(&in, &why) == LG_LEVEL_EARLY_WARNING);
    in.verdict[LG_COMP_BATTERY] = LG_VERDICT_FAILED;
    CHECK(lg_mode_required(&in, &why) == LG_LEVEL_DEGRADED);

    /* Gate verdict: WARNING -> 1, FAILED -> 4. Self check -> 4 even if all else is fine. */
    in = healthy();
    in.verdict[LG_COMP_GATE] = LG_VERDICT_WARNING;
    CHECK(lg_mode_required(&in, &why) == LG_LEVEL_EARLY_WARNING);
    in.verdict[LG_COMP_GATE] = LG_VERDICT_FAILED;
    CHECK(lg_mode_required(&in, &why) == LG_LEVEL_FAILSAFE);
    in = healthy();
    in.self_check_ok = 0;
    CHECK(lg_mode_required(&in, &why) == LG_LEVEL_FAILSAFE);

    /* Highest requirement wins and NULL reason is accepted. */
    in = healthy();
    in.verdict[0] = LG_VERDICT_WARNING;
    in.proc_state[LG_PROC_HEALTH] = LG_PROC_DEAD;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_DEGRADED);
}

/* ---- sequences ----------------------------------------------------------- */

static void test_repeated_boot(void) {
    lg_mode_t m;
    lg_mode_cfg_t cfg = lg_mode_cfg_default();
    lg_mode_in_t in = healthy();
    uint32_t now = 1000;
    int round;

    for (round = 0; round < 3; round++) {
        now = boot_to_normal(&m, &cfg, now);
        CHECK(m.level == LG_LEVEL_NORMAL);
        CHECK(m.boot_lock == 0);
        /* Boot again with a failing component: must stay in 4. */
        lg_mode_init(&m, now);
        in.verdict[LG_COMP_BATTERY] = LG_VERDICT_WARNING;
        now = run(&m, &cfg, &in, now, cfg.t_boot_stable_ms * 4);
        CHECK(m.level == LG_LEVEL_FAILSAFE);
        in.verdict[LG_COMP_BATTERY] = LG_VERDICT_OK;
    }
}

static void test_boot_timer_resets_on_unhealthy(void) {
    lg_mode_t m;
    lg_mode_cfg_t cfg = lg_mode_cfg_default();
    lg_mode_in_t ok = healthy();
    lg_mode_in_t bad = healthy();
    uint32_t now = 0;

    bad.verdict[0] = LG_VERDICT_WARNING;
    lg_mode_init(&m, 0);
    now = run(&m, &cfg, &ok, now, cfg.t_boot_stable_ms - 100); /* almost there */
    lg_mode_step(&m, &cfg, &bad, now);                          /* one unhealthy tick */
    now += 100;
    /* Needs a fresh full window of healthy. */
    now = run(&m, &cfg, &ok, now, cfg.t_boot_stable_ms - 100);
    CHECK(m.level == LG_LEVEL_FAILSAFE);
    lg_mode_step(&m, &cfg, &ok, now + 100);
    lg_mode_step(&m, &cfg, &ok, now + 200);
    CHECK(m.level == LG_LEVEL_NORMAL);
}

static void test_boot_all_healthy_but_guard_not_needed_and_ack_ignored_when_unhealthy(void) {
    lg_mode_t m;
    lg_mode_cfg_t cfg = lg_mode_cfg_default();
    lg_mode_in_t in = healthy();
    uint32_t now = 0;

    /* Boot with a level-3 requirement plus a guard ack: stays in 4. */
    in.verdict[comp(0, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    in.verdict[comp(0, LG_SENSOR_B)] = LG_VERDICT_FAILED;
    in.guard_ack = 1;
    lg_mode_init(&m, 0);
    now = run(&m, &cfg, &in, now, 3 * TS);
    CHECK(m.level == LG_LEVEL_FAILSAFE);
    /* Level 2 requirement: still not allowed out (guard ack needs <= level 1). */
    in = healthy();
    in.verdict[comp(0, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    in.guard_ack = 1;
    now = run(&m, &cfg, &in, now, 3 * TS);
    CHECK(m.level == LG_LEVEL_FAILSAFE);
}

static void test_guard_ack_same_tick_as_clear(void) {
    lg_mode_t m;
    lg_mode_cfg_t cfg = lg_mode_cfg_default();
    lg_mode_in_t bad = healthy();
    lg_mode_in_t ok = healthy();
    uint32_t now = boot_to_normal(&m, &cfg, 0);
    lg_mode_result_t r;

    bad.verdict[LG_COMP_GATE] = LG_VERDICT_FAILED;
    lg_mode_step(&m, &cfg, &bad, now);
    CHECK(m.level == LG_LEVEL_FAILSAFE);
    now = run(&m, &cfg, &bad, now + 100, 5000);

    /* Ack and recovery in the very same tick: still has to wait t_stable. */
    ok.guard_ack = 1;
    r = lg_mode_step(&m, &cfg, &ok, now);
    CHECK(!r.changed && m.level == LG_LEVEL_FAILSAFE);
    ok.guard_ack = 0;
    {
        uint32_t ack_t = now; /* release timer started here */
        now = run(&m, &cfg, &ok, now + 100, TS - 200);
        CHECK(m.level == LG_LEVEL_FAILSAFE);
        r = lg_mode_step(&m, &cfg, &ok, ack_t + TS - 1);
        CHECK(!r.changed && m.level == LG_LEVEL_FAILSAFE);
        r = lg_mode_step(&m, &cfg, &ok, ack_t + TS);
        CHECK(m.level == LG_LEVEL_NORMAL);
        CHECK(r.changed && r.reason == LG_REASON_GUARD_ACK);
    }
    CHECK(m.guard_latched == 0);
}

static void test_guard_ack_ignored_outside_level4(void) {
    lg_mode_t m;
    lg_mode_cfg_t cfg = lg_mode_cfg_default();
    lg_mode_in_t in = healthy();
    uint32_t now = boot_to_normal(&m, &cfg, 0);
    lg_mode_in_t bad = healthy();

    /* An ack while in level 0 must not be remembered for a later level 4. */
    in.guard_ack = 1;
    lg_mode_step(&m, &cfg, &in, now);
    CHECK(m.guard_latched == 0);
    bad.self_check_ok = 0;
    lg_mode_step(&m, &cfg, &bad, now + 100);
    CHECK(m.level == LG_LEVEL_FAILSAFE);
    in.guard_ack = 0;
    run(&m, &cfg, &in, now + 200, 3 * TS);
    CHECK(m.level == LG_LEVEL_FAILSAFE);

    /* Same for an ack in level 2, 3. */
    now = boot_to_normal(&m, &cfg, now + 10 * TS);
    in = healthy();
    in.verdict[comp(0, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    in.guard_ack = 1;
    lg_mode_step(&m, &cfg, &in, now);
    CHECK(m.level == LG_LEVEL_DEGRADED && m.guard_latched == 0);
}

static void test_guard_ack_void_while_unhealthy(void) {
    /* An ack given while the inputs still demand level 2 or more is void: after recovery
     * the guard must acknowledge again. */
    lg_mode_t m;
    lg_mode_cfg_t cfg = lg_mode_cfg_default();
    lg_mode_in_t bad = healthy();
    lg_mode_in_t ok = healthy();
    uint32_t now = boot_to_normal(&m, &cfg, 0);

    bad.self_check_ok = 0;
    lg_mode_step(&m, &cfg, &bad, now);
    bad.guard_ack = 1;
    lg_mode_step(&m, &cfg, &bad, now + 100);
    bad.guard_ack = 0;
    CHECK(m.level == LG_LEVEL_FAILSAFE);
    run(&m, &cfg, &bad, now + 200, 3 * TS);
    CHECK(m.level == LG_LEVEL_FAILSAFE); /* unhealthy: never released */
    now += 200 + 3 * TS;
    run(&m, &cfg, &ok, now, 3 * TS);
    CHECK(m.level == LG_LEVEL_FAILSAFE); /* the void ack is not remembered */
    now += 3 * TS;
    ok.guard_ack = 1;
    lg_mode_step(&m, &cfg, &ok, now);
    ok.guard_ack = 0;
    run(&m, &cfg, &ok, now + 10, TS + 200);
    CHECK(m.level == LG_LEVEL_NORMAL); /* a fresh ack while healthy releases after t_stable */
}

static void test_level4_flapping_health_resets_release_timer(void) {
    lg_mode_t m;
    lg_mode_cfg_t cfg = lg_mode_cfg_default();
    lg_mode_in_t bad = healthy();
    lg_mode_in_t ok = healthy();
    uint32_t now = boot_to_normal(&m, &cfg, 0);
    int i;

    bad.self_check_ok = 0;
    lg_mode_step(&m, &cfg, &bad, now);
    ok.guard_ack = 1;
    lg_mode_step(&m, &cfg, &ok, now + 100);
    ok.guard_ack = 0;
    now += 100;
    /* Alternate: good for 20 s, bad for 100 ms, over and over. Must never release. */
    for (i = 0; i < 5; i++) {
        now = run(&m, &cfg, &ok, now + 100, TS - 10000);
        lg_mode_step(&m, &cfg, &bad, now);
        CHECK(m.level == LG_LEVEL_FAILSAFE);
    }
}

static void test_level4_every_trigger_and_no_early_exit(void) {
    lg_mode_cfg_t cfg = lg_mode_cfg_default();
    int variant;

    for (variant = 0; variant < 6; variant++) {
        lg_mode_t m;
        lg_mode_in_t bad = healthy();
        lg_mode_in_t ok = healthy();
        uint32_t now = boot_to_normal(&m, &cfg, 0);

        switch (variant) {
        case 0: bad.self_check_ok = 0; break;
        case 1: bad.proc_state[LG_PROC_CONTROLLER] = LG_PROC_DEAD; break;
        case 2: bad.proc_state[LG_PROC_SUPERVISOR] = LG_PROC_DEAD; break;
        case 3: bad.proc_state[LG_PROC_GATE] = LG_PROC_DEAD; break;
        case 4: bad.verdict[LG_COMP_GATE] = LG_VERDICT_FAILED; break;
        default:
            bad.verdict[comp(0, LG_SENSOR_A)] = LG_VERDICT_FAILED;
            bad.verdict[comp(0, LG_SENSOR_B)] = LG_VERDICT_FAILED;
            bad.schedule_ok[0] = 0;
            break;
        }
        lg_mode_step(&m, &cfg, &bad, now);
        CHECK(m.level == LG_LEVEL_FAILSAFE);
        /* Recovery without ack, however long: stays. */
        run(&m, &cfg, &ok, now + 100, 10 * TS);
        CHECK(m.level == LG_LEVEL_FAILSAFE);
        CHECK(m.boot_lock == 0);
        /* Ack arrives, the controller must still wait full t_stable. */
        ok.guard_ack = 1;
        now += 100 + 10 * TS;
        lg_mode_step(&m, &cfg, &ok, now);
        ok.guard_ack = 0;
        run(&m, &cfg, &ok, now + 100, TS - 200);
        CHECK(m.level == LG_LEVEL_FAILSAFE);
        lg_mode_step(&m, &cfg, &ok, now + TS);
        CHECK(m.level == LG_LEVEL_NORMAL);
    }
}

static void test_descent_timer_behaviour(void) {
    lg_mode_t m;
    lg_mode_cfg_t cfg = lg_mode_cfg_default();
    lg_mode_in_t bad3 = healthy();
    lg_mode_in_t req2 = healthy();
    lg_mode_in_t req1 = healthy();
    lg_mode_in_t ok = healthy();
    uint32_t now = boot_to_normal(&m, &cfg, 0);

    bad3.verdict[comp(0, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    bad3.verdict[comp(0, LG_SENSOR_B)] = LG_VERDICT_FAILED;
    req2.verdict[comp(0, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    req1.verdict[0] = LG_VERDICT_WARNING;

    lg_mode_step(&m, &cfg, &bad3, now);
    CHECK(m.level == LG_LEVEL_UNCERTAIN);

    /* Required drops to 2 for 20 s, then to 1: timer restarts at the change. */
    now = run(&m, &cfg, &req2, now + 100, 20000);
    CHECK(m.level == LG_LEVEL_UNCERTAIN);
    now = run(&m, &cfg, &req1, now, TS - 1000);
    CHECK(m.level == LG_LEVEL_UNCERTAIN); /* 29 s at req 1, 49 s total: not yet */
    now = run(&m, &cfg, &req1, now, 1100);
    CHECK(m.level == LG_LEVEL_EARLY_WARNING);

    /* Now back up and down: relapse to the current level cancels the timer. */
    now = run(&m, &cfg, &ok, now, TS - 1000); /* 29 s toward 0 */
    lg_mode_step(&m, &cfg, &req1, now);       /* back to requirement == level */
    now = run(&m, &cfg, &ok, now + 100, TS - 1000);
    CHECK(m.level == LG_LEVEL_EARLY_WARNING); /* timer restarted: still only 29 s */
    now = run(&m, &cfg, &ok, now, 2000);
    CHECK(m.level == LG_LEVEL_NORMAL);

    /* Flicker: a one-tick escalation in the middle of descent from 2 resets the timer. */
    now = boot_to_normal(&m, &cfg, now + TS);
    lg_mode_step(&m, &cfg, &req2, now);
    CHECK(m.level == LG_LEVEL_DEGRADED);
    now = run(&m, &cfg, &ok, now + 100, TS - 1000);
    lg_mode_step(&m, &cfg, &bad3, now); /* escalate to 3 */
    CHECK(m.level == LG_LEVEL_UNCERTAIN);
    now = run(&m, &cfg, &ok, now + 100, TS - 1000);
    CHECK(m.level == LG_LEVEL_UNCERTAIN);
    now = run(&m, &cfg, &ok, now, 2000);
    CHECK(m.level == LG_LEVEL_NORMAL);
}

static void test_descent_exact_boundary(void) {
    lg_mode_t m;
    lg_mode_cfg_t cfg = lg_mode_cfg_default();
    lg_mode_in_t bad = healthy();
    lg_mode_in_t ok = healthy();
    uint32_t now = boot_to_normal(&m, &cfg, 0);

    bad.verdict[0] = LG_VERDICT_WARNING;
    lg_mode_step(&m, &cfg, &bad, now);
    CHECK(m.level == LG_LEVEL_EARLY_WARNING);
    lg_mode_step(&m, &cfg, &ok, now + 1000); /* timer starts here */
    lg_mode_step(&m, &cfg, &ok, now + 1000 + TS - 1);
    CHECK(m.level == LG_LEVEL_EARLY_WARNING);
    lg_mode_step(&m, &cfg, &ok, now + 1000 + TS);
    CHECK(m.level == LG_LEVEL_NORMAL);
}

static void test_no_early_drop_anywhere(void) {
    /* Safety sweep: from every level 1..3 with random-ish inputs, the level never goes below
     * the required level, and after any tick with required > level the level equals required. */
    lg_mode_t m;
    lg_mode_cfg_t cfg = lg_mode_cfg_default();
    uint32_t now = boot_to_normal(&m, &cfg, 0);
    uint32_t seed = 12345;
    int i;

    for (i = 0; i < 20000; i++) {
        lg_mode_in_t in = healthy();
        uint8_t req;
        uint8_t before = m.level;
        int k;

        seed = seed * 1664525u + 1013904223u;
        for (k = 0; k < LG_COMP_COUNT; k++) {
            if (((seed >> (k * 3)) & 7u) == 0) {
                in.verdict[k] = (uint8_t)(1 + ((seed >> 24) & 1u));
            }
        }
        seed = seed * 1664525u + 1013904223u;
        for (k = 0; k < LG_PROC_COUNT; k++) {
            if (((seed >> k) & 15u) == 0) {
                in.proc_state[k] = (uint8_t)(1 + ((seed >> 20) & 1u));
            }
        }
        in.schedule_ok[0] = (uint8_t)((seed >> 29) & 1u);
        in.self_check_ok = (uint8_t)(((seed >> 28) & 7u) != 0);
        in.guard_ack = (uint8_t)(((seed >> 30) & 3u) == 0);
        req = lg_mode_required(&in, 0);
        now += 1000;
        lg_mode_step(&m, &cfg, &in, now);
        if (before != LG_LEVEL_FAILSAFE) {
            CHECK(m.level >= req);
        }
        if (before == LG_LEVEL_FAILSAFE) {
            /* Leaving 4 only with a latched ack or boot lock: here boot lock is 0. */
            if (m.level != LG_LEVEL_FAILSAFE) {
                CHECK(req <= LG_LEVEL_EARLY_WARNING);
            }
        }
        if (req > before && before != LG_LEVEL_FAILSAFE) {
            CHECK(m.level == req);
        }
        CHECK(m.level <= LG_LEVEL_FAILSAFE);
    }
}

/* ---- voting and clearing ------------------------------------------------- */

static lg_vsensor_t vs(uint8_t verdict, uint8_t fresh, uint8_t occ) {
    lg_vsensor_t s;
    s.verdict = verdict;
    s.fresh = fresh;
    s.occupied = occ;
    s.quality = 100;
    return s;
}

static void test_voting_extra(void) {
    lg_vdir_t d;
    lg_vote_t v;
    lg_vote_cfg_t cfg = lg_vote_cfg_default();
    uint8_t lvl;

    /* Failed sensor reporting occupied is ignored; healthy one decides. */
    d.a = vs(LG_VERDICT_FAILED, 1, 1);
    d.b = vs(LG_VERDICT_OK, 1, 0);
    v = lg_vote_direction(&d);
    CHECK(v.detection == LG_DET_TRUSTED && !v.approaching);
    /* Stale occupied is ignored. */
    d.a = vs(LG_VERDICT_OK, 0, 1);
    v = lg_vote_direction(&d);
    CHECK(v.detection == LG_DET_TRUSTED && !v.approaching);
    /* Both stale: no detection. */
    d.b = vs(LG_VERDICT_OK, 0, 0);
    v = lg_vote_direction(&d);
    CHECK(v.detection == LG_DET_NONE && !v.approaching);
    /* Warning used; either sensor triggers. */
    d.a = vs(LG_VERDICT_WARNING, 1, 0);
    d.b = vs(LG_VERDICT_WARNING, 1, 1);
    v = lg_vote_direction(&d);
    CHECK(v.detection == LG_DET_TRUSTED && v.approaching);
    d.a = vs(LG_VERDICT_WARNING, 1, 1);
    d.b = vs(LG_VERDICT_FAILED, 1, 0);
    CHECK(lg_vote_direction(&d).approaching);
    /* Both failed and occupied: not approaching, no detection. */
    d.a = vs(LG_VERDICT_FAILED, 1, 1);
    d.b = vs(LG_VERDICT_FAILED, 1, 1);
    v = lg_vote_direction(&d);
    CHECK(v.detection == LG_DET_NONE && !v.approaching);

    /* Lower delay: level 2 and above start lowering sooner (floor 0). */
    for (lvl = 0; lvl <= LG_LEVEL_FAILSAFE; lvl++) {
        uint32_t want = cfg.prewarning_ms;
        if (lvl >= LG_LEVEL_DEGRADED) {
            want = cfg.prewarning_ms > cfg.extra_margin_ms ? cfg.prewarning_ms - cfg.extra_margin_ms : 0;
        }
        CHECK(lg_lower_delay_ms(&cfg, lvl) == want);
    }
}

static lg_clear_in_t clear_in(uint32_t now, uint8_t level) {
    lg_clear_in_t in;

    memset(&in, 0, sizeof in);
    in.now_ms = now;
    in.level = level;
    in.island = vs(LG_VERDICT_OK, 1, 0);
    in.axle_counting_healthy = 1;
    in.axles_in = in.axles_out = 4;
    in.gate_down = 1;
    in.gate_down_since_ms = 0;
    return in;
}

static void test_clearing_extra(void) {
    lg_clear_t c;
    lg_clear_cfg_t cfg = lg_clear_cfg_default();
    lg_clear_in_t in;
    uint32_t t;
    uint8_t lvl;

    /* Exact t_clear boundary. */
    lg_clear_init(&c);
    in = clear_in(100000, LG_LEVEL_NORMAL);
    lg_clear_step(&c, &cfg, &in);
    in.now_ms = 100000 + cfg.t_clear_ms - 1;
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_ISLAND_NOT_CLEAR);
    in.now_ms = 100000 + cfg.t_clear_ms;
    CHECK(lg_clear_step(&c, &cfg, &in) == 0);

    /* Island blips occupied for one tick: restarts the clock. */
    in.island.occupied = 1;
    in.now_ms += 10;
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_ISLAND_NOT_CLEAR);
    in.island.occupied = 0;
    in.now_ms += 10;
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_ISLAND_NOT_CLEAR);
    in.now_ms += cfg.t_clear_ms - 1;
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_ISLAND_NOT_CLEAR);

    /* Stale or failed island reading "clear" does not count, and resets tracking. */
    lg_clear_init(&c);
    in = clear_in(50000, LG_LEVEL_NORMAL);
    lg_clear_step(&c, &cfg, &in);
    in.now_ms += cfg.t_clear_ms / 2;
    in.island.fresh = 0;
    lg_clear_step(&c, &cfg, &in);
    in.island.fresh = 1;
    in.now_ms += cfg.t_clear_ms / 2 + 10;
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_ISLAND_NOT_CLEAR);
    in.island.verdict = LG_VERDICT_FAILED;
    in.now_ms += 10 * cfg.t_clear_ms;
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_ISLAND_NOT_CLEAR);

    /* Reset (new activation) forgets earlier clear time. */
    lg_clear_init(&c);
    in = clear_in(0, LG_LEVEL_NORMAL);
    lg_clear_step(&c, &cfg, &in);
    in.now_ms = 10 * cfg.t_clear_ms;
    CHECK(lg_clear_step(&c, &cfg, &in) == 0);
    lg_clear_reset(&c);
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_ISLAND_NOT_CLEAR);

    /* Min gate down time boundary, gate not down, other direction. */
    lg_clear_init(&c);
    in = clear_in(1000000, LG_LEVEL_NORMAL);
    in.gate_down_since_ms = in.now_ms - (cfg.min_gate_down_ms - 1);
    lg_clear_step(&c, &cfg, &in);
    in.now_ms += cfg.t_clear_ms;
    in.gate_down_since_ms = in.now_ms - (cfg.min_gate_down_ms - 1);
    CHECK(lg_clear_step(&c, &cfg, &in) == LG_CLEAR_MIN_DOWN_TIME);
    in.gate_down_since_ms = in.now_ms - cfg.min_gate_down_ms;
    CHECK(lg_clear_step(&c, &cfg, &in) == 0);
    in.gate_down = 0;
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_GATE_NOT_DOWN);
    in.gate_down = 1;
    in.other_dir_approaching = 1;
    CHECK(lg_clear_step(&c, &cfg, &in) == LG_CLEAR_OTHER_APPROACHING);

    /* Axle: mismatch blocks only when counting healthy; in<out also blocks. */
    in.other_dir_approaching = 0;
    in.axles_in = 4;
    in.axles_out = 3;
    CHECK(lg_clear_step(&c, &cfg, &in) == LG_CLEAR_AXLE_MISMATCH);
    in.axles_in = 3;
    in.axles_out = 4;
    CHECK(lg_clear_step(&c, &cfg, &in) == LG_CLEAR_AXLE_MISMATCH);
    /* Without a healthy axle counter nothing proves the train passed except the island sighting. */
    in.axle_counting_healthy = 0;
    CHECK(lg_clear_step(&c, &cfg, &in) == LG_CLEAR_ISLAND_NOT_CLEAR);
    in.island.occupied = 1; /* train seen on the island */
    in.now_ms += 10;
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_ISLAND_NOT_CLEAR);
    in.island.occupied = 0;
    in.now_ms += 10;
    (void)lg_clear_step(&c, &cfg, &in);
    in.now_ms += cfg.t_clear_ms;
    in.gate_down_since_ms = in.now_ms - cfg.min_gate_down_ms;
    CHECK(lg_clear_step(&c, &cfg, &in) == 0);

    /* Level 4 never clears, whatever else is given (even guard confirm). */
    in = clear_in(500000, LG_LEVEL_FAILSAFE);
    in.guard_confirm = 1;
    lg_clear_init(&c);
    lg_clear_step(&c, &cfg, &in);
    in.now_ms += 10 * cfg.t_clear_ms;
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_LEVEL4);

    /* Guard confirm only counts in level 3, and never overrides the other conditions. */
    for (lvl = 0; lvl <= LG_LEVEL_FAILSAFE; lvl++) {
        lg_clear_init(&c);
        in = clear_in(900000, lvl);
        in.island.occupied = 1;
        in.axles_in = 5;
        in.guard_confirm = 1;
        in.gate_down_since_ms = in.now_ms - 100000;
        t = lg_clear_step(&c, &cfg, &in);
        if (lvl == LG_LEVEL_UNCERTAIN) {
            /* The guard replaces the island condition only: a healthy axle counter still blocks. */
            CHECK(t == LG_CLEAR_AXLE_MISMATCH);
            in.axles_in = 4;
            CHECK(lg_clear_step(&c, &cfg, &in) == 0);
            in.other_dir_approaching = 1;
            CHECK(lg_clear_step(&c, &cfg, &in) == LG_CLEAR_OTHER_APPROACHING);
            in.other_dir_approaching = 0;
            in.gate_down_since_ms = in.now_ms - 1;
            CHECK(lg_clear_step(&c, &cfg, &in) == LG_CLEAR_MIN_DOWN_TIME);
            in.gate_down_since_ms = in.now_ms - 100000;
            in.gate_down = 0;
            CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_GATE_NOT_DOWN);
        } else {
            CHECK(t & LG_CLEAR_ISLAND_NOT_CLEAR);
            CHECK(t & LG_CLEAR_AXLE_MISMATCH);
        }
    }
    /* Level 3 without guard: island occupied blocks. */
    lg_clear_init(&c);
    in = clear_in(900000, LG_LEVEL_UNCERTAIN);
    in.island.occupied = 1;
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_ISLAND_NOT_CLEAR);
}

/* ---- metrics ---------------------------------------------------------------- */

static void test_metrics_extra(void) {
    lg_metrics_t m;
    uint32_t base = 0xFFFFFF00u; /* ticks straddle the 32-bit wrap */

    lg_metrics_init(&m, base);
    CHECK(lg_metrics_closure_ms_per_hour(&m) == 0);
    lg_metrics_tick(&m, base, LG_GATE_DOWN, 0, 0, 0);
    lg_metrics_tick(&m, base + 100, LG_GATE_UP, 0, 0, 0);       /* rising: boot closure not counted */
    CHECK(m.closure.count == 0);
    lg_metrics_tick(&m, base + 200, LG_GATE_UP, 1, 0, 0);       /* lights on */
    lg_metrics_tick(&m, base + 300, LG_GATE_LOWERING, 1, 0, 0);
    lg_metrics_tick(&m, base + 1000, LG_GATE_DOWN, 1, 0, 0);    /* wraps past 2^32 */
    lg_metrics_tick(&m, base + 1500, LG_GATE_DOWN, 1, 1, 0);    /* train arrives */
    CHECK(m.warning.count == 1 && m.warning.last_ms == 1300);
    lg_metrics_tick(&m, base + 2000, LG_GATE_RAISING, 0, 0, 0);
    CHECK(m.closure.count == 1 && m.closure.last_ms == 1000);
    CHECK(m.wrong_side_events == 0 && lg_metrics_wrong_side_ok(&m));
    CHECK(m.closed_ms == 1800);
    lg_metrics_tick(&m, base + 2500, LG_GATE_UP, 0, 0, 0);
    CHECK(m.closed_ms == 2300);
    CHECK(lg_metrics_closure_ms_per_hour(&m) == (uint64_t)2300 * 3600000u / 2500u);

    /* Gate raises while train still on island: wrong side, once per episode. */
    lg_metrics_init(&m, 0);
    lg_metrics_tick(&m, 0, LG_GATE_DOWN, 1, 1, 0);
    lg_metrics_tick(&m, 10, LG_GATE_RAISING, 1, 1, 0);
    lg_metrics_tick(&m, 20, LG_GATE_UP, 1, 1, 0);
    CHECK(m.wrong_side_events == 1 && m.wrong_side_ticks == 2);
    CHECK(!lg_metrics_wrong_side_ok(&m));
    lg_metrics_tick(&m, 30, LG_GATE_DOWN, 1, 1, 0);
    lg_metrics_tick(&m, 40, LG_GATE_FAULT, 1, 1, 0); /* FAULT is not DOWN: counts */
    CHECK(m.wrong_side_events == 2);

    /* Gate LOWERING with train on island is wrong side. */
    lg_metrics_init(&m, 0);
    lg_metrics_tick(&m, 0, LG_GATE_UP, 0, 0, 0);
    lg_metrics_tick(&m, 10, LG_GATE_LOWERING, 1, 1, 0);
    CHECK(m.wrong_side_events == 1);

    /* Train arriving with lights off is counted, not a warning sample. */
    lg_metrics_init(&m, 0);
    lg_metrics_tick(&m, 0, LG_GATE_DOWN, 0, 0, 0);
    lg_metrics_tick(&m, 10, LG_GATE_DOWN, 0, 1, 0);
    CHECK(m.trains == 1 && m.trains_without_warning == 1 && m.warning.count == 0);
    /* Lights off then on again: warning time counts from the latest switch-on. */
    lg_metrics_tick(&m, 20, LG_GATE_DOWN, 0, 0, 0);
    lg_metrics_tick(&m, 30, LG_GATE_DOWN, 1, 0, 0);
    lg_metrics_tick(&m, 40, LG_GATE_DOWN, 0, 0, 0);
    lg_metrics_tick(&m, 50, LG_GATE_DOWN, 1, 0, 0);
    lg_metrics_tick(&m, 90, LG_GATE_DOWN, 1, 1, 0);
    CHECK(m.warning.count == 1 && m.warning.last_ms == 40);

    /* Recovery: end without begin ignored; double begin keeps first start; max tracked. */
    lg_metrics_init(&m, 0);
    lg_metrics_fault_end(&m, 100);
    CHECK(m.recovery.count == 0);
    lg_metrics_fault_begin(&m, 200);
    lg_metrics_fault_begin(&m, 300);
    lg_metrics_fault_end(&m, 1000);
    CHECK(m.recovery.last_ms == 800 && m.recovery.count == 1);
    lg_metrics_fault_begin(&m, 2000);
    lg_metrics_fault_end(&m, 2100);
    CHECK(m.recovery.count == 2 && m.recovery.max_ms == 800 && m.recovery.sum_ms == 900);
    /* Wrap in recovery. */
    lg_metrics_fault_begin(&m, 0xFFFFFFF0u);
    lg_metrics_fault_end(&m, 0x10u);
    CHECK(m.recovery.last_ms == 0x20u);

    /* Deadline counter and lg_tick_late boundary. */
    CHECK(!lg_tick_late(0, (uint64_t)(LG_TICK_MS + LG_TICK_TOLERANCE_MS) * 1000000u));
    CHECK(lg_tick_late(0, (uint64_t)(LG_TICK_MS + LG_TICK_TOLERANCE_MS) * 1000000u + 1));
    CHECK(!lg_tick_late(5000000000ull, 5010000000ull));
    CHECK(lg_tick_late(0xFFFFFFFFull, 0xFFFFFFFFull + 50000000ull));

    /* Closure per hour with a long run: no 32-bit overflow in the product. */
    lg_metrics_init(&m, 0);
    lg_metrics_tick(&m, 0, LG_GATE_DOWN, 0, 0, 0);
    lg_metrics_tick(&m, 3600000u, LG_GATE_DOWN, 0, 0, 0);
    CHECK(m.closed_ms == 3600000u);
    CHECK(lg_metrics_closure_ms_per_hour(&m) == 3600000u);
    lg_metrics_tick(&m, 3600000u * 2, LG_GATE_UP, 0, 0, 0);
    CHECK(lg_metrics_closure_ms_per_hour(&m) == 3600000u);
}

int main(void) {
    test_required_extra();
    test_repeated_boot();
    test_boot_timer_resets_on_unhealthy();
    test_boot_all_healthy_but_guard_not_needed_and_ack_ignored_when_unhealthy();
    test_guard_ack_same_tick_as_clear();
    test_guard_ack_ignored_outside_level4();
    test_guard_ack_void_while_unhealthy();
    test_level4_flapping_health_resets_release_timer();
    test_level4_every_trigger_and_no_early_exit();
    test_descent_timer_behaviour();
    test_descent_exact_boundary();
    test_no_early_drop_anywhere();
    test_voting_extra();
    test_clearing_extra();
    test_metrics_extra();
    LG_TEST_DONE("test_core_extra");
}
