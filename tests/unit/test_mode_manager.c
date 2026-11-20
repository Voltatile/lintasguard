#include "lg_mode.h"
#include "lg_test.h"

static lg_mode_cfg_t cfg;

#define COMP(dir, id) (LG_COMP_SENSOR_BASE + (dir) * LG_SENSOR_ID_COUNT + (id))
#define PROC(dir, id) (LG_PROC_SENSOR_BASE + (dir) * LG_SENSOR_ID_COUNT + (id))

/* Step once and return the resulting level. */
static uint8_t step(lg_mode_t *m, const lg_mode_in_t *in, uint32_t t) {
    return lg_mode_step(m, &cfg, in, t).to;
}

/* Boot into level 0: everything healthy for t_boot_stable. Returns the time after release. */
static uint32_t boot_to_normal(lg_mode_t *m, lg_mode_in_t *in) {
    lg_mode_result_t r;

    lg_mode_in_all_healthy(in);
    lg_mode_init(m, 0);
    r = lg_mode_step(m, &cfg, in, 0);
    CHECK(r.to == LG_LEVEL_FAILSAFE);
    r = lg_mode_step(m, &cfg, in, cfg.t_boot_stable_ms);
    CHECK(r.changed && r.to == LG_LEVEL_NORMAL && r.reason == LG_REASON_BOOT_RELEASED);
    return cfg.t_boot_stable_ms;
}

/* Hold the inputs for t_stable_ms starting at t; returns the level at the end. */
static uint8_t hold_stable(lg_mode_t *m, const lg_mode_in_t *in, uint32_t t) {
    (void)step(m, in, t);
    return step(m, in, t + cfg.t_stable_ms);
}

static void test_required(void) {
    lg_mode_in_t in;
    uint8_t reason = 0;

    lg_mode_in_all_healthy(&in);
    CHECK(lg_mode_required(&in, &reason) == LG_LEVEL_NORMAL && reason == LG_REASON_NONE);
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_NORMAL);

    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_WARNING;
    CHECK(lg_mode_required(&in, &reason) == LG_LEVEL_EARLY_WARNING && reason == LG_REASON_WARNING);

    lg_mode_in_all_healthy(&in);
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    CHECK(lg_mode_required(&in, &reason) == LG_LEVEL_DEGRADED && reason == LG_REASON_SENSOR_FAILED);

    /* Directions are independent: West both failed with schedule gives level 3. */
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_B)] = LG_VERDICT_FAILED;
    CHECK(lg_mode_required(&in, &reason) == LG_LEVEL_UNCERTAIN &&
          reason == LG_REASON_DETECT_LOST_SCHEDULE);

    /* Same without timetable information for that direction: level 4. */
    in.schedule_ok[LG_DIR_WEST] = 0;
    CHECK(lg_mode_required(&in, &reason) == LG_LEVEL_FAILSAFE &&
          reason == LG_REASON_DETECT_LOST_NO_INFO);

    /* Schedule missing for the other direction does not matter while its sensors work. */
    lg_mode_in_all_healthy(&in);
    in.schedule_ok[LG_DIR_EAST] = 0;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_NORMAL);

    /* A restarting sensor driver counts as lost; both restarting means detection lost. */
    lg_mode_in_all_healthy(&in);
    in.proc_state[PROC(LG_DIR_EAST, LG_SENSOR_A)] = LG_PROC_RESTARTING;
    CHECK(lg_mode_required(&in, &reason) == LG_LEVEL_DEGRADED);
    in.proc_state[PROC(LG_DIR_EAST, LG_SENSOR_B)] = LG_PROC_RESTARTING;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_UNCERTAIN);

    /* A dead sensor driver is treated like a failed sensor. */
    lg_mode_in_all_healthy(&in);
    in.proc_state[PROC(LG_DIR_WEST, LG_SENSOR_B)] = LG_PROC_DEAD;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_DEGRADED);

    /* Island sensor lost: level 3 with timetable, level 4 without. */
    lg_mode_in_all_healthy(&in);
    in.verdict[COMP(LG_DIR_EAST, LG_SENSOR_ISLAND)] = LG_VERDICT_FAILED;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_UNCERTAIN);
    in.schedule_ok[LG_DIR_EAST] = 0;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_FAILSAFE);

    /* Critical processes, gate verdict and self check: level 4. */
    lg_mode_in_all_healthy(&in);
    in.proc_state[LG_PROC_GATE] = LG_PROC_DEAD;
    CHECK(lg_mode_required(&in, &reason) == LG_LEVEL_FAILSAFE &&
          reason == LG_REASON_CRITICAL_PROC_DEAD);
    lg_mode_in_all_healthy(&in);
    in.proc_state[LG_PROC_CONTROLLER] = LG_PROC_DEAD;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_FAILSAFE);
    lg_mode_in_all_healthy(&in);
    in.proc_state[LG_PROC_SUPERVISOR] = LG_PROC_DEAD;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_FAILSAFE);
    lg_mode_in_all_healthy(&in);
    in.verdict[LG_COMP_GATE] = LG_VERDICT_FAILED;
    CHECK(lg_mode_required(&in, &reason) == LG_LEVEL_FAILSAFE && reason == LG_REASON_GATE_FAILED);
    lg_mode_in_all_healthy(&in);
    in.self_check_ok = 0;
    CHECK(lg_mode_required(&in, &reason) == LG_LEVEL_FAILSAFE && reason == LG_REASON_SELF_CHECK);

    /* Non-critical processes and the battery. */
    lg_mode_in_all_healthy(&in);
    in.proc_state[LG_PROC_BRIDGE] = LG_PROC_DEAD;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_EARLY_WARNING);
    lg_mode_in_all_healthy(&in);
    in.proc_state[LG_PROC_HEALTH] = LG_PROC_DEAD;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_DEGRADED);
    lg_mode_in_all_healthy(&in);
    in.proc_state[LG_PROC_SCHEDULE] = LG_PROC_RESTARTING;
    CHECK(lg_mode_required(&in, &reason) == LG_LEVEL_DEGRADED && reason == LG_REASON_PROC_RESTART);
    lg_mode_in_all_healthy(&in);
    in.verdict[LG_COMP_BATTERY] = LG_VERDICT_WARNING;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_EARLY_WARNING);
    in.verdict[LG_COMP_BATTERY] = LG_VERDICT_FAILED;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_DEGRADED);

    /* The highest requirement wins. */
    lg_mode_in_all_healthy(&in);
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_WARNING;
    in.verdict[COMP(LG_DIR_EAST, LG_SENSOR_B)] = LG_VERDICT_FAILED;
    CHECK(lg_mode_required(&in, 0) == LG_LEVEL_DEGRADED);
}

static void test_boot(void) {
    lg_mode_t m;
    lg_mode_in_t in;
    lg_mode_result_t r;

    /* Starts in level 4 and stays there until healthy for t_boot_stable. */
    lg_mode_in_all_healthy(&in);
    lg_mode_init(&m, 100);
    CHECK(m.level == LG_LEVEL_FAILSAFE);
    r = lg_mode_step(&m, &cfg, &in, 100);
    CHECK(!r.changed && r.reason == LG_REASON_BOOT);
    CHECK(step(&m, &in, 100 + cfg.t_boot_stable_ms - 1) == LG_LEVEL_FAILSAFE);
    CHECK(step(&m, &in, 100 + cfg.t_boot_stable_ms) == LG_LEVEL_NORMAL);

    /* An unhealthy moment restarts the boot timer. */
    lg_mode_init(&m, 0);
    (void)step(&m, &in, 0);
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_WARNING;
    (void)step(&m, &in, cfg.t_boot_stable_ms - 1);
    lg_mode_in_all_healthy(&in);
    (void)step(&m, &in, cfg.t_boot_stable_ms);
    CHECK(step(&m, &in, cfg.t_boot_stable_ms + cfg.t_boot_stable_ms - 1) == LG_LEVEL_FAILSAFE);
    CHECK(step(&m, &in, cfg.t_boot_stable_ms + cfg.t_boot_stable_ms) == LG_LEVEL_NORMAL);

    /* A sensor that is already failed at boot keeps the crossing in level 4. */
    lg_mode_init(&m, 0);
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    (void)step(&m, &in, 0);
    CHECK(step(&m, &in, 10 * cfg.t_stable_ms) == LG_LEVEL_FAILSAFE);

    /* An ack while the sensor is still failed does not count (void). */
    in.guard_ack = 1;
    (void)step(&m, &in, 10 * cfg.t_stable_ms);
    in.guard_ack = 0;

    /* A guard can release a boot with a WARNING component: it lands on that required level. */
    lg_mode_in_all_healthy(&in);
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_WARNING;
    CHECK(step(&m, &in, 11 * cfg.t_stable_ms) == LG_LEVEL_FAILSAFE); /* the earlier ack was void */
    in.guard_ack = 1;
    (void)step(&m, &in, 12 * cfg.t_stable_ms);
    in.guard_ack = 0;
    CHECK(step(&m, &in, 13 * cfg.t_stable_ms) == LG_LEVEL_EARLY_WARNING);
}

static void test_escalation_and_deescalation(void) {
    lg_mode_t m;
    lg_mode_in_t in;
    lg_mode_result_t r;
    uint32_t t = boot_to_normal(&m, &in);

    /* 0 -> 1 immediately on a warning. */
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_WARNING;
    t += 10;
    r = lg_mode_step(&m, &cfg, &in, t);
    CHECK(r.changed && r.from == LG_LEVEL_NORMAL && r.to == LG_LEVEL_EARLY_WARNING &&
          r.reason == LG_REASON_WARNING);

    /* 1 -> 2 immediately when the sensor fails. */
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    t += 10;
    r = lg_mode_step(&m, &cfg, &in, t);
    CHECK(r.changed && r.to == LG_LEVEL_DEGRADED && r.reason == LG_REASON_SENSOR_FAILED);

    /* 2 -> 3 immediately when the redundant sensor fails too. */
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_B)] = LG_VERDICT_FAILED;
    t += 10;
    r = lg_mode_step(&m, &cfg, &in, t);
    CHECK(r.changed && r.to == LG_LEVEL_UNCERTAIN && r.reason == LG_REASON_DETECT_LOST_SCHEDULE);

    /* 3 -> 4 immediately when the timetable information is lost. */
    in.schedule_ok[LG_DIR_WEST] = 0;
    t += 10;
    r = lg_mode_step(&m, &cfg, &in, t);
    CHECK(r.changed && r.to == LG_LEVEL_FAILSAFE && r.reason == LG_REASON_DETECT_LOST_NO_INFO);

    /* Jumping over levels is allowed on the way up: 0 -> 4 and 0 -> 3. */
    t = boot_to_normal(&m, &in);
    in.self_check_ok = 0;
    r = lg_mode_step(&m, &cfg, &in, t + 1);
    CHECK(r.from == LG_LEVEL_NORMAL && r.to == LG_LEVEL_FAILSAFE);
    t = boot_to_normal(&m, &in);
    in.verdict[COMP(LG_DIR_EAST, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    in.verdict[COMP(LG_DIR_EAST, LG_SENSOR_B)] = LG_VERDICT_FAILED;
    r = lg_mode_step(&m, &cfg, &in, t + 1);
    CHECK(r.from == LG_LEVEL_NORMAL && r.to == LG_LEVEL_UNCERTAIN);
}

static void test_descent(void) {
    lg_mode_t m;
    lg_mode_in_t in;
    lg_mode_result_t r;
    uint32_t t = boot_to_normal(&m, &in);

    /* 1 -> 0 only after t_stable (exact boundary). */
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_WARNING;
    (void)step(&m, &in, t);
    lg_mode_in_all_healthy(&in);
    t += 100;
    (void)step(&m, &in, t);
    CHECK(step(&m, &in, t + cfg.t_stable_ms - 1) == LG_LEVEL_EARLY_WARNING);
    r = lg_mode_step(&m, &cfg, &in, t + cfg.t_stable_ms);
    CHECK(r.changed && r.to == LG_LEVEL_NORMAL && r.reason == LG_REASON_STABLE);

    /* 2 -> 0 after t_stable once a restarted driver is running again. */
    t += 10 * cfg.t_stable_ms;
    in.proc_state[PROC(LG_DIR_WEST, LG_SENSOR_A)] = LG_PROC_RESTARTING;
    CHECK(step(&m, &in, t) == LG_LEVEL_DEGRADED);
    in.proc_state[PROC(LG_DIR_WEST, LG_SENSOR_A)] = LG_PROC_RUNNING;
    CHECK(hold_stable(&m, &in, t + 50) == LG_LEVEL_NORMAL);

    /* 3 -> 2 when one sensor of the lost pair returns, 2 -> 0 when both are back. */
    t += 20 * cfg.t_stable_ms;
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_B)] = LG_VERDICT_FAILED;
    CHECK(step(&m, &in, t) == LG_LEVEL_UNCERTAIN);
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_B)] = LG_VERDICT_OK;
    CHECK(hold_stable(&m, &in, t + 50) == LG_LEVEL_DEGRADED);
    in.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_OK;
    CHECK(hold_stable(&m, &in, t + 50 + 2 * cfg.t_stable_ms) == LG_LEVEL_NORMAL);
}

static void test_hysteresis(void) {
    lg_mode_t m;
    lg_mode_in_t in;
    lg_mode_in_t bad;
    uint32_t t = boot_to_normal(&m, &in);
    int i;

    bad = in;
    bad.verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    CHECK(step(&m, &bad, t) == LG_LEVEL_DEGRADED);

    /* A flapping input never lets the level drop: the condition is never clear for t_stable. */
    for (i = 1; i <= 50; i++) {
        t += cfg.t_stable_ms / 2;
        CHECK(step(&m, &in, t) == LG_LEVEL_DEGRADED);
        t += 1;
        CHECK(step(&m, &bad, t) == LG_LEVEL_DEGRADED);
    }

    /* A change of the requirement restarts the timer (2 -> required 1 -> required 0). */
    {
        lg_mode_in_t warn = in;
        warn.verdict[COMP(LG_DIR_WEST, LG_SENSOR_B)] = LG_VERDICT_WARNING;
        t += 10;
        (void)step(&m, &warn, t);                        /* required 1, timer starts */
        CHECK(step(&m, &warn, t + cfg.t_stable_ms - 1) == LG_LEVEL_DEGRADED);
        (void)step(&m, &in, t + cfg.t_stable_ms - 1);    /* required 0, timer restarts */
        CHECK(step(&m, &in, t + 2 * cfg.t_stable_ms - 2) == LG_LEVEL_DEGRADED);
        CHECK(step(&m, &in, t + 2 * cfg.t_stable_ms - 1) == LG_LEVEL_NORMAL);
    }

    /* Escalation during the descent timer is still immediate. */
    t += 10 * cfg.t_stable_ms;
    (void)step(&m, &bad, t);
    (void)step(&m, &in, t + 100);
    bad.verdict[COMP(LG_DIR_WEST, LG_SENSOR_B)] = LG_VERDICT_FAILED;
    CHECK(step(&m, &bad, t + 200) == LG_LEVEL_UNCERTAIN);
}

static void test_level4_lock(void) {
    lg_mode_t m;
    lg_mode_in_t in;
    lg_mode_result_t r;
    uint32_t t = boot_to_normal(&m, &in);

    /* Enter level 4 through a failed self check. */
    in.self_check_ok = 0;
    t += 10;
    CHECK(step(&m, &in, t) == LG_LEVEL_FAILSAFE);

    /* Healthy again for a very long time: still level 4 (no automatic return). */
    in.self_check_ok = 1;
    t += 10;
    (void)step(&m, &in, t);
    CHECK(step(&m, &in, t + 100 * cfg.t_stable_ms) == LG_LEVEL_FAILSAFE);
    t += 100 * cfg.t_stable_ms;

    /* A guard ack while still unhealthy is not enough. */
    in.self_check_ok = 0;
    in.guard_ack = 1;
    (void)step(&m, &in, t);
    in.guard_ack = 0;
    CHECK(step(&m, &in, t + 100 * cfg.t_stable_ms) == LG_LEVEL_FAILSAFE);
    t += 100 * cfg.t_stable_ms;

    /* The ack given while unhealthy was void: recovery alone does not release. */
    in.self_check_ok = 1;
    t += 10;
    (void)step(&m, &in, t);
    CHECK(step(&m, &in, t + 100 * cfg.t_stable_ms) == LG_LEVEL_FAILSAFE);
    t += 100 * cfg.t_stable_ms;

    /* A new ack while healthy releases after t_stable (exact boundary). */
    in.guard_ack = 1;
    (void)step(&m, &in, t);
    in.guard_ack = 0;
    CHECK(step(&m, &in, t + cfg.t_stable_ms - 1) == LG_LEVEL_FAILSAFE);
    r = lg_mode_step(&m, &cfg, &in, t + cfg.t_stable_ms);
    CHECK(r.changed && r.to == LG_LEVEL_NORMAL && r.reason == LG_REASON_GUARD_ACK);
    t += cfg.t_stable_ms;

    /* Levels 0 to 3 ignore a guard ack, so it cannot be pre-loaded for a later level 4. */
    in.guard_ack = 1;
    (void)step(&m, &in, t + 10);
    in.guard_ack = 0;
    in.self_check_ok = 0;
    CHECK(step(&m, &in, t + 20) == LG_LEVEL_FAILSAFE);
    in.self_check_ok = 1;
    (void)step(&m, &in, t + 30);
    CHECK(step(&m, &in, t + 30 + 100 * cfg.t_stable_ms) == LG_LEVEL_FAILSAFE);
    t += 30 + 100 * cfg.t_stable_ms;

    /* A guard ack with healthy components, then a new fault resets the timer. */
    in.guard_ack = 1;
    (void)step(&m, &in, t);
    in.guard_ack = 0;
    CHECK(step(&m, &in, t + cfg.t_stable_ms - 1) == LG_LEVEL_FAILSAFE);
    in.verdict[COMP(LG_DIR_EAST, LG_SENSOR_ISLAND)] = LG_VERDICT_FAILED; /* required 3 */
    (void)step(&m, &in, t + cfg.t_stable_ms);
    CHECK(m.level == LG_LEVEL_FAILSAFE);

    /* The fault above voided the ack: a failed component keeps level 4 locked. */
    in.verdict[COMP(LG_DIR_EAST, LG_SENSOR_ISLAND)] = LG_VERDICT_OK;
    in.verdict[COMP(LG_DIR_EAST, LG_SENSOR_A)] = LG_VERDICT_FAILED;
    t += 2 * cfg.t_stable_ms;
    (void)step(&m, &in, t);
    CHECK(step(&m, &in, t + 100 * cfg.t_stable_ms) == LG_LEVEL_FAILSAFE);
    t += 100 * cfg.t_stable_ms;

    /* Leaving level 4 lands on the level the inputs demand, here early warning (level 1). */
    in.verdict[COMP(LG_DIR_EAST, LG_SENSOR_A)] = LG_VERDICT_WARNING;
    in.guard_ack = 1;
    t += 10;
    (void)step(&m, &in, t);
    in.guard_ack = 0;
    CHECK(step(&m, &in, t + cfg.t_stable_ms) == LG_LEVEL_EARLY_WARNING);
}

static void test_level4_reentry_clears_latch(void) {
    lg_mode_t m;
    lg_mode_in_t in;
    uint32_t t = boot_to_normal(&m, &in);

    /* Fault, recovery, ack, release. Then a second level 4 must need a new ack. */
    in.proc_state[LG_PROC_GATE] = LG_PROC_DEAD;
    (void)step(&m, &in, t + 1);
    in.proc_state[LG_PROC_GATE] = LG_PROC_RUNNING;
    in.guard_ack = 1;
    (void)step(&m, &in, t + 3);
    in.guard_ack = 0;
    CHECK(step(&m, &in, t + 3 + cfg.t_stable_ms) == LG_LEVEL_NORMAL);
    t += 3 + cfg.t_stable_ms;

    in.proc_state[LG_PROC_GATE] = LG_PROC_DEAD;
    (void)step(&m, &in, t + 1);
    CHECK(m.level == LG_LEVEL_FAILSAFE);
    in.proc_state[LG_PROC_GATE] = LG_PROC_RUNNING;
    (void)step(&m, &in, t + 2);
    CHECK(step(&m, &in, t + 2 + 100 * cfg.t_stable_ms) == LG_LEVEL_FAILSAFE);
}

static void test_time_wrap(void) {
    lg_mode_t m;
    lg_mode_in_t in;
    uint32_t t0 = 0xFFFFFFFFu - 1000u;

    lg_mode_in_all_healthy(&in);
    lg_mode_init(&m, t0);
    (void)step(&m, &in, t0);
    CHECK(step(&m, &in, t0 + cfg.t_boot_stable_ms) == LG_LEVEL_NORMAL); /* wraps past zero */
}

int main(void) {
    cfg = lg_mode_cfg_default();
    test_required();
    test_boot();
    test_escalation_and_deescalation();
    test_descent();
    test_hysteresis();
    test_level4_lock();
    test_level4_reentry_clears_latch();
    test_time_wrap();
    LG_TEST_DONE("test_mode_manager");
}
