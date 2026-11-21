#include <stdio.h>
#include <string.h>

#include "lg_controller_logic.h"
#include "lg_gate_model.h"
#include "lg_test.h"

/* Step 8: what the controller does with the supervisor's PROC_STATUS reports. */

#define SENS(dir, id) (LG_PROC_SENSOR_BASE + (dir) * LG_SENSOR_ID_COUNT + (id))

static lg_ctl_out_t g_last;
static int g_sup_alive = 1; /* the supervisor repeats its table every LG_SUP_STATUS_REFRESH_MS */

/* One tick: all six sensors send a clear healthy sample, the gate answers UP. */
static void tick(lg_ctl_t *c, uint32_t now) {
    int d;
    int i;

    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            CHECK(lg_ctl_on_sample(c, (uint8_t)d, (uint8_t)i, 0, 97, 0, now, now) == 0);
        }
    }
    CHECK(lg_ctl_on_gate_reply(c, LG_GATE_UP, now) == 0);
    g_last = lg_ctl_step(c, now);
}

/* The refresh of a live supervisor: the states the controller already knows, sent again. */
static void refresh(lg_ctl_t *c, uint32_t now) {
    int p;

    for (p = 0; p < LG_PROC_COUNT; p++) {
        if (p != LG_PROC_SUPERVISOR && p != LG_PROC_CONTROLLER && c->sup_armed) {
            CHECK(lg_ctl_on_proc_status(c, (uint8_t)p, c->proc_state[p], c->proc_restarts[p], now) == 0);
        }
    }
}

static void run(lg_ctl_t *c, uint32_t *t, uint32_t ms) {
    uint32_t end = *t + ms;

    for (; *t != end; *t += 10) {
        if (g_sup_alive && (*t % LG_SUP_STATUS_REFRESH_MS) == 0) {
            refresh(c, *t);
        }
        tick(c, *t);
    }
}

static void released(lg_ctl_t *c, uint32_t *t) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();

    lg_ctl_init(c, &cfg, *t);
    run(c, t, LG_T_BOOT_STABLE_MS + 500);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
}

static void test_no_supervisor_is_not_dead(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    run(&c, &t, 120000);
    CHECK(g_last.level == LG_LEVEL_NORMAL); /* never armed: hand-started setups keep working */
    CHECK(c.sup_armed == 0 && c.sup_silent == 0);
}

static void test_sensor_restart_is_level2_until_stable(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t start;
    int west_a = SENS(LG_DIR_WEST, LG_SENSOR_A);

    released(&c, &t);
    CHECK(lg_ctl_on_proc_status(&c, (uint8_t)west_a, LG_PROC_RESTARTING, 1, t) == 0);
    run(&c, &t, 100);
    CHECK(g_last.level == LG_LEVEL_DEGRADED); /* the redundant sensor still works */
    CHECK(c.proc_restarts[west_a] == 1);
    /* Back to RUNNING: level 2 holds for T_stable, then level 0. */
    CHECK(lg_ctl_on_proc_status(&c, (uint8_t)west_a, LG_PROC_RUNNING, 1, t) == 0);
    start = t;
    run(&c, &t, LG_T_STABLE_MS - 200);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    run(&c, &t, 500);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    CHECK(t - start > LG_T_STABLE_MS);
    CHECK(g_last.gate_target == LG_GATE_TARGET_UP);
}

static void test_island_restart_locks_level4_until_guard(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    int isl = SENS(LG_DIR_WEST, LG_SENSOR_ISLAND);

    released(&c, &t);
    CHECK(lg_ctl_on_proc_status(&c, (uint8_t)isl, LG_PROC_RESTARTING, 1, t) == 0);
    run(&c, &t, 100);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE); /* no island, no timetable */
    CHECK(g_last.gate_target == LG_GATE_TARGET_DOWN);
    CHECK(lg_ctl_on_proc_status(&c, (uint8_t)isl, LG_PROC_RUNNING, 1, t) == 0);
    run(&c, &t, 90000);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE); /* level 4 is not left without a guard */
    lg_ctl_on_guard_ack(&c);
    CHECK(c.guard_acks == 1);
    run(&c, &t, LG_T_STABLE_MS + 500);
    CHECK(g_last.level <= LG_LEVEL_EARLY_WARNING);
    CHECK(g_last.gate_target == LG_GATE_TARGET_UP);
}

static void test_gate_and_plant(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_PLANT, LG_PROC_RESTARTING, 1, t) == 0);
    run(&c, &t, 100);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_PLANT, LG_PROC_RUNNING, 1, t) == 0);
    run(&c, &t, LG_T_STABLE_MS + 500);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    /* A gate reported DEAD is level 4 even though it still answers. */
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_GATE, LG_PROC_DEAD, 3, t) == 0);
    run(&c, &t, 100);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    CHECK(g_last.gate_target == LG_GATE_TARGET_DOWN);
}

/* A gate that is being restarted is level 4 by rule, also while it already answers again. */
static void test_gate_restarting_is_level4(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_GATE, LG_PROC_RESTARTING, 1, t) == 0);
    run(&c, &t, 100); /* samples and gate replies keep coming: only the supervisor says "restarting" */
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    CHECK(g_last.gate_target == LG_GATE_TARGET_DOWN);
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_GATE, LG_PROC_RUNNING, 1, t) == 0);
    run(&c, &t, 90000);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE); /* and it stays there until a guard acknowledges */
}

/* A controller started with -r (restart by the supervisor) is never released by the boot check. */
static void test_restarted_controller_needs_guard(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    uint32_t t = 0;

    cfg.restarted = 1;
    lg_ctl_init(&c, &cfg, t);
    CHECK(c.mode.level == LG_LEVEL_FAILSAFE && c.mode.boot_lock == 0);
    run(&c, &t, 120000); /* everything healthy for two minutes */
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    CHECK(g_last.gate_target == LG_GATE_TARGET_DOWN && g_last.lights);
    lg_ctl_on_guard_ack(&c);
    run(&c, &t, LG_T_STABLE_MS - 1000);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE); /* not at once */
    run(&c, &t, 2000);
    CHECK(g_last.level <= LG_LEVEL_EARLY_WARNING);
    CHECK(g_last.gate_target == LG_GATE_TARGET_UP);
    /* A guard ack given while a sensor is down is void. */
    lg_ctl_init(&c, &cfg, t);
    CHECK(lg_ctl_on_proc_status(&c, (uint8_t)SENS(LG_DIR_WEST, LG_SENSOR_ISLAND), LG_PROC_RESTARTING, 1, t) == 0);
    lg_ctl_on_guard_ack(&c);
    run(&c, &t, 5000);
    CHECK(lg_ctl_on_proc_status(&c, (uint8_t)SENS(LG_DIR_WEST, LG_SENSOR_ISLAND), LG_PROC_RUNNING, 1, t) == 0);
    run(&c, &t, 60000);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
}

static void test_supervisor_silence(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t last_status;
    uint32_t i;

    released(&c, &t);
    /* The supervisor talks once a second: fine. */
    for (i = 0; i < 6; i++) {
        CHECK(lg_ctl_on_proc_status(&c, LG_PROC_PLANT, LG_PROC_RUNNING, 0, t) == 0);
        run(&c, &t, LG_SUP_STATUS_REFRESH_MS);
        CHECK(g_last.level == LG_LEVEL_NORMAL);
    }
    CHECK(c.sup_armed == 1 && c.sup_silent == 0);
    g_sup_alive = 0; /* it stops */
    last_status = c.sup_rx_ms;
    /* It stops: level 4 once the silence limit is passed, not before. */
    run(&c, &t, last_status + LG_CTL_SUP_SILENCE_MS - t - 20);
    CHECK(g_last.level == LG_LEVEL_NORMAL && c.sup_silent == 0);
    run(&c, &t, 100);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE && c.sup_silent == 1);
    CHECK(g_last.gate_target == LG_GATE_TARGET_DOWN);
    /* A supervisor that talks again clears the silence but not level 4. */
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_PLANT, LG_PROC_RUNNING, 0, t) == 0);
    g_sup_alive = 1;
    run(&c, &t, 60000);
    CHECK(c.sup_silent == 0 && g_last.level == LG_LEVEL_FAILSAFE);
}

static void test_supervisor_silence_across_wrap(void) {
    lg_ctl_t c;
    uint32_t t = 0xFFFFFE00u;

    released(&c, &t);
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_PLANT, LG_PROC_RUNNING, 0, t) == 0);
    g_sup_alive = 0;
    run(&c, &t, LG_CTL_SUP_SILENCE_MS - 100);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    run(&c, &t, 300);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE && c.sup_silent == 1);
    CHECK(t < 0x80000000u); /* the clock really wrapped during the run */
    g_sup_alive = 1;
}

static void test_refused_and_unknown(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_SUPERVISOR, LG_PROC_DEAD, 0, t) == -1);
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_CONTROLLER, LG_PROC_DEAD, 0, t) == -1);
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_COUNT, LG_PROC_RUNNING, 0, t) == -1);
    CHECK(lg_ctl_on_proc_status(&c, 255, LG_PROC_RUNNING, 0, t) == -1);
    CHECK(c.proc_status_rejected == 4 && c.proc_status_rx == 0);
    CHECK(c.sup_armed == 0); /* a refused status does not arm the supervisor check */
    run(&c, &t, 5000);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    /* A state outside the enum fails closed: DEAD. */
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_PLANT, 7, 0, t) == 0);
    CHECK(c.proc_state[LG_PROC_PLANT] == LG_PROC_DEAD && c.proc_status_unknown == 1);
    run(&c, &t, 100);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
}

static void test_text_fields(void) {
    lg_ctl_t c;
    char buf[800];
    uint32_t t = 0;
    size_t n;

    released(&c, &t);
    CHECK(lg_ctl_on_proc_status(&c, (uint8_t)SENS(LG_DIR_EAST, LG_SENSOR_B), LG_PROC_RESTARTING, 1, t) == 0);
    n = lg_ctl_text(&c, buf, sizeof buf);
    CHECK(n > 0 && n == strlen(buf) && buf[n - 1] == '\n');
    CHECK(strstr(buf, "sup_armed=1 sup_silent=0 proc_status_rx=1 proc_status_rejected=0") != NULL);
    CHECK(strstr(buf, "proc_bad=0x80") != NULL); /* sensor east/b is process id 7 */
    CHECK(strstr(buf, "guard_acks=0") != NULL);
    CHECK(strchr(buf, '\n') == buf + n - 1); /* still one line */
}

int main(void) {
    test_no_supervisor_is_not_dead();
    test_sensor_restart_is_level2_until_stable();
    test_island_restart_locks_level4_until_guard();
    test_gate_and_plant();
    test_gate_restarting_is_level4();
    test_restarted_controller_needs_guard();
    test_supervisor_silence();
    test_supervisor_silence_across_wrap();
    test_refused_and_unknown();
    test_text_fields();
    LG_TEST_DONE("test_controller_proc");
}
