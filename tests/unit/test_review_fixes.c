/* Regression tests for the findings of the Step 4 review. Each block names the failing input. */

#include "lg_mode.h"
#include "lg_test.h"
#include "lg_time.h"
#include "lg_voting.h"

static lg_mode_cfg_t cfg;

#define COMP(dir, id) (LG_COMP_SENSOR_BASE + (dir) * LG_SENSOR_ID_COUNT + (id))

static lg_vsensor_t vs(uint8_t verdict, uint8_t fresh, uint8_t occupied) {
    lg_vsensor_t s;
    s.verdict = verdict;
    s.fresh = fresh;
    s.occupied = occupied;
    return s;
}

static uint8_t step(lg_mode_t *m, const lg_mode_in_t *in, uint32_t t) {
    return lg_mode_step(m, &cfg, in, t).to;
}

/* Finding 2: a real level 4 event during boot must not be hidden by the boot lock. */
static void test_boot_lock_does_not_hide_real_level4(void) {
    lg_mode_t m;
    lg_mode_in_t in;
    uint8_t ev;

    for (ev = 0; ev < 4; ev++) {
        lg_mode_in_all_healthy(&in);
        lg_mode_init(&m, 0);
        (void)step(&m, &in, 0);
        switch (ev) {
        case 0: in.self_check_ok = 0; break;
        case 1: in.proc_state[LG_PROC_SUPERVISOR] = LG_PROC_DEAD; break;
        case 2: in.verdict[LG_COMP_GATE] = LG_VERDICT_FAILED; break;
        default: in.proc_state[LG_PROC_GATE] = LG_PROC_DEAD; break;
        }
        (void)step(&m, &in, 100);
        lg_mode_in_all_healthy(&in);
        (void)step(&m, &in, 1000);
        /* Healthy for far longer than t_boot_stable and t_stable: still locked. */
        CHECK(step(&m, &in, 1000 + 100 * cfg.t_stable_ms) == LG_LEVEL_FAILSAFE);
        /* Only a guard ack while healthy releases it. */
        in.guard_ack = 1;
        (void)step(&m, &in, 2000 + 100 * cfg.t_stable_ms);
        in.guard_ack = 0;
        CHECK(step(&m, &in, 2000 + 101 * cfg.t_stable_ms) == LG_LEVEL_NORMAL);
    }
}

/* Not-ready-yet conditions at boot (no schedule info yet) still release by the boot check. */
static void test_boot_not_ready_is_not_a_real_level4(void) {
    lg_mode_t m;
    lg_mode_in_t in;

    lg_mode_in_all_healthy(&in);
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_B)] = LG_VERDICT_FAILED;
    in.schedule_ok[LG_DIR_WEST] = 0; /* required 4, but only "no information" */
    lg_mode_init(&m, 0);
    (void)step(&m, &in, 0);
    lg_mode_in_all_healthy(&in);
    (void)step(&m, &in, 100);
    CHECK(step(&m, &in, 100 + cfg.t_boot_stable_ms) == LG_LEVEL_NORMAL);
}

/* Finding 5: boot timer and guard timer must not share a start time. */
static void test_boot_and_guard_timers_are_separate(void) {
    lg_mode_t m;
    lg_mode_in_t in;

    lg_mode_in_all_healthy(&in);
    in.verdict[COMP(LG_DIR_EAST, LG_SENSOR_B)] = LG_VERDICT_WARNING; /* required 1 */
    in.guard_ack = 1;
    lg_mode_init(&m, 0);
    (void)step(&m, &in, 0); /* guard release timer starts */
    in.guard_ack = 0;
    lg_mode_in_all_healthy(&in);
    /* Became fully healthy at t_boot_stable: the boot timer must start now, not at t = 0. */
    CHECK(step(&m, &in, cfg.t_boot_stable_ms) == LG_LEVEL_FAILSAFE);
    CHECK(step(&m, &in, 2 * cfg.t_boot_stable_ms - 1) == LG_LEVEL_FAILSAFE);
    CHECK(step(&m, &in, 2 * cfg.t_boot_stable_ms) == LG_LEVEL_NORMAL);
}

/* Latch review: an ack given while a fault is active must never release a later state. */
static void test_ack_during_fault_never_releases_later(void) {
    lg_mode_t m;
    lg_mode_in_t in;
    lg_mode_in_t bad;
    uint32_t t = 0;

    lg_mode_in_all_healthy(&in);
    lg_mode_init(&m, 0);
    (void)step(&m, &in, 0);
    (void)step(&m, &in, cfg.t_boot_stable_ms);
    CHECK(m.level == LG_LEVEL_NORMAL);
    t = cfg.t_boot_stable_ms;

    bad = in;
    bad.verdict[LG_COMP_GATE] = LG_VERDICT_FAILED;
    (void)step(&m, &bad, t + 7000);
    bad.guard_ack = 1; /* ack while the gate is FAILED */
    (void)step(&m, &bad, t + 7001);
    bad.guard_ack = 0;
    bad.proc_state[LG_PROC_HEALTH] = LG_PROC_DEAD; /* another fault appears */
    (void)step(&m, &bad, t + 8000);
    /* Two hours later everything is healthy: still level 4. */
    CHECK(step(&m, &in, t + 2 * 3600 * 1000u) == LG_LEVEL_FAILSAFE);
    CHECK(step(&m, &in, t + 2 * 3600 * 1000u + cfg.t_stable_ms) == LG_LEVEL_FAILSAFE);
}

/* An ack followed by a new fault before the release is voided. */
static void test_ack_voided_by_new_fault(void) {
    lg_mode_t m;
    lg_mode_in_t in;
    lg_mode_in_t bad;
    uint32_t t;

    lg_mode_in_all_healthy(&in);
    lg_mode_init(&m, 0);
    (void)step(&m, &in, 0);
    (void)step(&m, &in, cfg.t_boot_stable_ms);
    in.self_check_ok = 0;
    t = cfg.t_boot_stable_ms + 10;
    (void)step(&m, &in, t);
    in.self_check_ok = 1;
    in.guard_ack = 1;
    (void)step(&m, &in, t + 1);
    in.guard_ack = 0;
    bad = in;
    bad.verdict[COMP(LG_DIR_EAST, LG_SENSOR_ISLAND)] = LG_VERDICT_FAILED; /* required 3 */
    (void)step(&m, &bad, t + 2);
    (void)step(&m, &in, t + 3); /* healthy again, but the ack is void */
    CHECK(step(&m, &in, t + 3 + 100 * cfg.t_stable_ms) == LG_LEVEL_FAILSAFE);
}

/* Finding 4: unknown enum values fail closed. */
static void test_invalid_values_fail_closed(void) {
    lg_mode_in_t in;
    lg_clear_t c;
    lg_clear_cfg_t cc = lg_clear_cfg_default();
    lg_clear_in_t ci;
    lg_vote_t v;
    lg_vdir_t d;

    lg_mode_in_all_healthy(&in);
    in.verdict[LG_COMP_GATE] = 9;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_FAILSAFE);

    lg_mode_in_all_healthy(&in);
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = 7;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_DEGRADED); /* treated as a failed sensor */

    lg_mode_in_all_healthy(&in);
    in.proc_state[LG_PROC_GATE] = 9;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_FAILSAFE); /* unknown state = DEAD */

    lg_mode_in_all_healthy(&in);
    in.proc_state[LG_PROC_BRIDGE] = 200;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_EARLY_WARNING);

    d.a = vs(7, 1, 1);
    d.b = vs(LG_VERDICT_OK, 1, 0);
    v = lg_vote_direction(&d);
    CHECK(v.detection == LG_DET_TRUSTED && !v.approaching); /* invalid sensor ignored */
    d.b = vs(9, 1, 0);
    CHECK(lg_vote_direction(&d).detection == LG_DET_NONE);

    ci.now_ms = 100000;
    ci.level = 7; /* out of range: level 4 */
    ci.island = vs(LG_VERDICT_OK, 1, 0);
    ci.axle_counting_healthy = 1;
    ci.axles_in = ci.axles_out = 4;
    ci.other_dir_approaching = 0;
    ci.gate_down = 1;
    ci.gate_down_since_ms = 0;
    ci.guard_confirm = 1;
    lg_clear_init(&c);
    (void)lg_clear_step(&c, &cc, &ci);
    ci.now_ms += cc.t_clear_ms;
    CHECK(lg_clear_step(&c, &cc, &ci) & LG_CLEAR_LEVEL4);
    ci.island = vs(7, 1, 0); /* invalid island verdict never counts as clear */
    ci.level = LG_LEVEL_NORMAL;
    lg_clear_init(&c);
    (void)lg_clear_step(&c, &cc, &ci);
    ci.now_ms += cc.t_clear_ms;
    CHECK(lg_clear_step(&c, &cc, &ci) & LG_CLEAR_ISLAND_NOT_CLEAR);
}

/* Finding 3: a timestamp in the future is "not elapsed", never "a very long time ago". */
static void test_negative_elapsed(void) {
    lg_clear_t c;
    lg_clear_cfg_t cc = lg_clear_cfg_default();
    lg_clear_in_t ci;
    lg_mode_t m;
    lg_mode_in_t in;

    CHECK(lg_elapsed_ge(100, 100, 0));
    CHECK(lg_elapsed_ge(100, 90, 10));
    CHECK(!lg_elapsed_ge(100, 91, 10));
    CHECK(!lg_elapsed_ge(90, 100, 0)); /* since is in the future */
    CHECK(lg_elapsed_ge(5, 0xFFFFFFFFu - 4u, 10)); /* wrap: 10 ms elapsed */
    CHECK(!lg_elapsed_ge(0xFFFFFFFFu, 5, 0));      /* far in the future */

    /* Gate timestamp a little newer than now: the minimum down time is not met. */
    ci.now_ms = 9990;
    ci.level = LG_LEVEL_NORMAL;
    ci.island = vs(LG_VERDICT_OK, 1, 0);
    ci.axle_counting_healthy = 1;
    ci.axles_in = ci.axles_out = 4;
    ci.other_dir_approaching = 0;
    ci.gate_down = 1;
    ci.gate_down_since_ms = 10000;
    ci.guard_confirm = 0;
    lg_clear_init(&c);
    ci.now_ms = 9990 - cc.t_clear_ms;
    (void)lg_clear_step(&c, &cc, &ci);
    ci.now_ms = 9990;
    CHECK(lg_clear_step(&c, &cc, &ci) == LG_CLEAR_MIN_DOWN_TIME);

    /* Mode manager: a clock that steps back must not drop the level. */
    lg_mode_in_all_healthy(&in);
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_B)] = LG_VERDICT_FAILED;
    lg_mode_init(&m, 0);
    (void)step(&m, &in, 0);
    lg_mode_in_all_healthy(&in);
    (void)step(&m, &in, 100);
    (void)step(&m, &in, 100 + cfg.t_boot_stable_ms);
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_B)] = LG_VERDICT_FAILED;
    (void)step(&m, &in, 20000);
    CHECK(m.level == LG_LEVEL_UNCERTAIN);
    lg_mode_in_all_healthy(&in);
    (void)step(&m, &in, 21000); /* timer starts at 21000 */
    CHECK(step(&m, &in, 20999) == LG_LEVEL_UNCERTAIN);
}

int main(void) {
    cfg = lg_mode_cfg_default();
    test_boot_lock_does_not_hide_real_level4();
    test_boot_not_ready_is_not_a_real_level4();
    test_boot_and_guard_timers_are_separate();
    test_ack_during_fault_never_releases_later();
    test_ack_voided_by_new_fault();
    test_invalid_values_fail_closed();
    test_negative_elapsed();
    LG_TEST_DONE("test_review_fixes");
}
