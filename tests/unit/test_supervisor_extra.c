#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lg_controller_logic.h"
#include "lg_gate_model.h"
#include "lg_guards.h"
#include "lg_supervisor_logic.h"
#include "lg_test.h"

/*
 * Step 8, extra edge cases for supervisor_logic.c, guards.c and lg_ctl_on_proc_status().
 * A test that fails here points at a defect in core/ (or at an unclear requirement, see the comments).
 */

#define SENS(dir, id) (LG_PROC_SENSOR_BASE + (dir) * LG_SENSOR_ID_COUNT + (id))

static int find_act(const lg_sup_action_t *a, size_t n, uint8_t kind, uint8_t proc) {
    size_t i;

    for (i = 0; i < n; i++) {
        if (a[i].kind == kind && a[i].proc == proc) {
            return (int)i;
        }
    }
    return -1;
}

/* A config with only the gate installed: a single process makes the restart budget easy to follow. */
static lg_sup_cfg_t gate_only(void) {
    lg_sup_cfg_t c = lg_sup_cfg_default();
    int i;

    for (i = 0; i < LG_PROC_COUNT; i++) {
        c.installed[i] = (uint8_t)(i == LG_PROC_GATE);
    }
    return c;
}

/* One failure of the gate: spawn it (pid), then let it exit. Returns the phase afterwards. */
static uint8_t gate_fail_by_exit(lg_sup_t *s, uint32_t pid, uint32_t t) {
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n = lg_sup_step(s, t, a, LG_SUP_MAX_ACTIONS);

    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_GATE) >= 0);
    CHECK(lg_sup_on_spawn(s, LG_PROC_GATE, pid, t) == 0);
    CHECK(lg_sup_on_exit(s, pid, t + 1) == 1);
    return s->proc[LG_PROC_GATE].phase;
}

/* Budget n: the n-th failure still restarts, failure n + 1 inside the window gives DEAD. */
static void budget_dies_after(uint8_t budget_cfg, unsigned expect_budget) {
    lg_sup_cfg_t c = gate_only();
    lg_sup_t s;
    unsigned k;
    uint32_t t = 100;

    c.budget = budget_cfg;
    c.window_ms = 1000000; /* all failures stay in one window */
    lg_sup_init(&s, &c);
    CHECK(s.cfg.budget == expect_budget);
    for (k = 1; k <= expect_budget; k++) {
        CHECK(gate_fail_by_exit(&s, 1000 + k, t) == LG_SUP_IDLE);
        t += 10;
    }
    CHECK(lg_sup_restarts_in_window(&s, LG_PROC_GATE, t) == expect_budget);
    CHECK(gate_fail_by_exit(&s, 2000, t) == LG_SUP_DEAD);
    CHECK(lg_sup_reported_state(&s, LG_PROC_GATE) == LG_PROC_DEAD);
    CHECK(s.proc[LG_PROC_GATE].total_restarts == expect_budget);
}

static void test_budget_clamp(void) {
    budget_dies_after(0, 1);   /* 0 is raised to 1 */
    budget_dies_after(1, 1);
    budget_dies_after(3, 3);
    budget_dies_after(LG_SUP_BUDGET_MAX, LG_SUP_BUDGET_MAX);
    budget_dies_after(LG_SUP_BUDGET_MAX + 1, LG_SUP_BUDGET_MAX); /* no write past restart_ms[] */
    budget_dies_after(255, LG_SUP_BUDGET_MAX);
}

/*
 * window_ms = 0: prune() drops every entry at once (elapsed >= 0 is always true), so the budget can never
 * be used up and a crash-looping process is restarted for ever. A window of 0 should not switch the
 * budget off. Expectation: still DEAD after budget + 1 failures.
 */
static void test_window_zero(void) {
    lg_sup_cfg_t c = gate_only();
    lg_sup_t s;
    uint32_t t = 100;
    int k;
    uint8_t ph = LG_SUP_IDLE;

    c.window_ms = 0;
    lg_sup_init(&s, &c);
    for (k = 0; k < 40 && ph != LG_SUP_DEAD; k++) {
        ph = gate_fail_by_exit(&s, 500 + (uint32_t)k, t);
        t += 10;
    }
    CHECK(ph == LG_SUP_DEAD);
}

static void test_window_boundary_and_expiry(void) {
    lg_sup_cfg_t c = gate_only();
    lg_sup_t s;
    uint32_t t0 = 1000; /* first failure is counted at t0 + 1 */

    lg_sup_init(&s, &c);
    CHECK(s.cfg.window_ms == 10000);
    CHECK(gate_fail_by_exit(&s, 11, t0) == LG_SUP_IDLE);
    CHECK(lg_sup_restarts_in_window(&s, LG_PROC_GATE, t0 + 1 + 9999) == 1);
    CHECK(lg_sup_restarts_in_window(&s, LG_PROC_GATE, t0 + 1 + 10000) == 0); /* exactly window_ms: expired */
    /* Failures at +4 s and +8 s, then one at +12 s: the first has left the window, so no DEAD. */
    CHECK(gate_fail_by_exit(&s, 12, t0 + 4000) == LG_SUP_IDLE);
    CHECK(gate_fail_by_exit(&s, 13, t0 + 8000) == LG_SUP_IDLE);
    CHECK(gate_fail_by_exit(&s, 14, t0 + 12000) == LG_SUP_IDLE);
    CHECK(lg_sup_restarts_in_window(&s, LG_PROC_GATE, t0 + 12002) == 3);
    /* Now three are in the window: the next one is the budget breaker. */
    CHECK(gate_fail_by_exit(&s, 15, t0 + 12010) == LG_SUP_DEAD);
}

static void test_window_across_clock_wrap(void) {
    lg_sup_cfg_t c = gate_only();
    lg_sup_t s;

    lg_sup_init(&s, &c);
    CHECK(gate_fail_by_exit(&s, 21, 0xFFFFFF00u) == LG_SUP_IDLE);
    CHECK(gate_fail_by_exit(&s, 22, 0xFFFFFF40u) == LG_SUP_IDLE);
    CHECK(gate_fail_by_exit(&s, 23, 0xFFFFFF80u) == LG_SUP_IDLE);
    CHECK(lg_sup_restarts_in_window(&s, LG_PROC_GATE, 0x00000100u) == 3); /* 0x200 ms later, still inside */
    CHECK(gate_fail_by_exit(&s, 24, 0x00000100u) == LG_SUP_DEAD);
}

/* Two processes with the same pid: on_spawn should refuse a pid that another live process already has. */
static void test_same_pid_two_processes(void) {
    lg_sup_cfg_t c = lg_sup_cfg_default();
    lg_sup_t s;

    lg_sup_init(&s, &c);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 100, 0) == 0);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_PLANT, 100, 0) == -1); /* DEFECT if accepted */
    /* If it was accepted, one exit only takes down one of them and the other keeps a dead pid. */
    CHECK(lg_sup_on_exit(&s, 100, 5) == 1);
    CHECK(s.proc[LG_PROC_GATE].pid != 100 || s.proc[LG_PROC_PLANT].pid != 100);
    CHECK(!(s.proc[LG_PROC_PLANT].pid == 100 && s.proc[LG_PROC_PLANT].phase == LG_SUP_STARTING));
}

static void test_pid_reuse_after_exit(void) {
    lg_sup_cfg_t c = lg_sup_cfg_default();
    lg_sup_t s;

    lg_sup_init(&s, &c);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 100, 0) == 0);
    CHECK(lg_sup_on_exit(&s, 100, 10) == 1);
    /* The OS gives pid 100 to another process; the old gate must not claim it. */
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 7, 20) == 0);
    CHECK(lg_sup_on_exit(&s, 100, 30) == 0); /* stale exit of the old pid */
    CHECK(s.exits_ignored == 1 && s.proc[LG_PROC_GATE].phase == LG_SUP_STARTING);
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 100, 31) == 0); /* beat from the old pid */
    CHECK(s.proc[LG_PROC_GATE].hb_ignored == 1);
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 7, 32) == 1);
    /* The same pid for the same process again after another exit is fine. */
    CHECK(lg_sup_on_exit(&s, 7, 40) == 1);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 7, 50) == 0);
    CHECK(lg_sup_on_exit(&s, 0, 60) == 0 && s.exits_ignored == 2);
}

static void test_exit_while_starting(void) {
    lg_sup_cfg_t c = gate_only();
    lg_sup_t s;
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n;

    lg_sup_init(&s, &c);
    n = lg_sup_step(&s, 0, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_GATE) >= 0);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 9, 10) == 0);
    CHECK(s.proc[LG_PROC_GATE].phase == LG_SUP_STARTING);
    CHECK(lg_sup_on_exit(&s, 9, 30) == 1);
    CHECK(s.proc[LG_PROC_GATE].phase == LG_SUP_IDLE && s.proc[LG_PROC_GATE].why == LG_SUP_WHY_EXIT);
    CHECK(s.proc[LG_PROC_GATE].pid == 0 && s.proc[LG_PROC_GATE].total_restarts == 1);
    CHECK(lg_sup_reported_state(&s, LG_PROC_GATE) == LG_PROC_RESTARTING);
    /* Respawn, first beat 50 ms later: recovery time counts from the detection. */
    n = lg_sup_step(&s, 40, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_GATE) >= 0);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 10, 40) == 0);
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 10, 80) == 1);
    CHECK(s.proc[LG_PROC_GATE].recovery_ms == 50 && s.proc[LG_PROC_GATE].recovery_pending == 0);
    CHECK(s.proc[LG_PROC_GATE].phase == LG_SUP_RUNNING);
    /* A first start without a failure never sets a recovery time. */
}

static void test_first_start_has_no_recovery_time(void) {
    lg_sup_cfg_t c = gate_only();
    lg_sup_t s;

    lg_sup_init(&s, &c);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 3, 100) == 0);
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 3, 150) == 1);
    CHECK(s.proc[LG_PROC_GATE].recovery_ms == 0);
}

static void test_exit_while_killing_keeps_detection_time(void) {
    lg_sup_cfg_t c = gate_only();
    lg_sup_t s;
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n;
    int k;

    lg_sup_init(&s, &c);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 5, 0) == 0);
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 5, 10) == 1);
    n = lg_sup_step(&s, 10 + c.heartbeat_timeout_ms, a, LG_SUP_MAX_ACTIONS);
    k = find_act(a, n, LG_SUP_ACT_KILL, LG_PROC_GATE);
    CHECK(k >= 0 && a[k].pid == 5);
    CHECK(s.proc[LG_PROC_GATE].phase == LG_SUP_KILLING);
    /* A beat from the dying process must not revive it. */
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 5, 10 + c.heartbeat_timeout_ms + 1) == 0);
    CHECK(s.proc[LG_PROC_GATE].phase == LG_SUP_KILLING);
    /* KILL is not repeated before kill_retry_ms and is repeated afterwards. */
    n = lg_sup_step(&s, 10 + c.heartbeat_timeout_ms + c.kill_retry_ms - 1, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_KILL, LG_PROC_GATE) < 0);
    n = lg_sup_step(&s, 10 + c.heartbeat_timeout_ms + c.kill_retry_ms, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_KILL, LG_PROC_GATE) >= 0);
    CHECK(lg_sup_on_exit(&s, 5, 10 + c.heartbeat_timeout_ms + 600) == 1);
    CHECK(s.proc[LG_PROC_GATE].why == LG_SUP_WHY_HANG && s.proc[LG_PROC_GATE].phase == LG_SUP_IDLE);
    CHECK(s.proc[LG_PROC_GATE].detect_ms == 10 + c.heartbeat_timeout_ms);
}

static void test_heartbeat_edge_cases(void) {
    lg_sup_cfg_t c = lg_sup_cfg_default();
    lg_sup_t s;

    lg_sup_init(&s, &c);
    /* IDLE: nothing was spawned. */
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 5, 10) == 0);
    CHECK(s.proc[LG_PROC_GATE].phase == LG_SUP_IDLE && s.proc[LG_PROC_GATE].hb_ignored == 1);
    CHECK(s.proc[LG_PROC_GATE].hb_rx == 0);
    /* pid 0 never matches the empty pid of an IDLE process. */
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 0, 10) == 0);
    CHECK(s.proc[LG_PROC_GATE].phase == LG_SUP_IDLE);
    /* Not installed (supervisor, health, schedule, bridge) and outside the table. */
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_HEALTH, 5, 10) == 0 && s.proc[LG_PROC_HEALTH].hb_ignored == 1);
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_COUNT, 5, 10) == 0);
    CHECK(lg_sup_on_heartbeat(&s, 255, 5, 10) == 0);
    CHECK(s.hb_unknown == 2);
    /* Wrong pid while RUNNING does not refresh the liveness: the hang is still found. */
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 5, 0) == 0);
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 5, 10) == 1);
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 6, 100) == 0);
    CHECK(s.proc[LG_PROC_GATE].last_hb_ms == 10);
    {
        lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
        size_t n = lg_sup_step(&s, 10 + c.heartbeat_timeout_ms, a, LG_SUP_MAX_ACTIONS);

        CHECK(find_act(a, n, LG_SUP_ACT_KILL, LG_PROC_GATE) >= 0);
    }
}

static void test_heartbeat_timeout_boundary(void) {
    lg_sup_cfg_t c = gate_only();
    lg_sup_t s;
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n;

    lg_sup_init(&s, &c);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 5, 0) == 0);
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 5, 100) == 1);
    n = lg_sup_step(&s, 100 + c.heartbeat_timeout_ms - 1, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_KILL, LG_PROC_GATE) < 0);
    n = lg_sup_step(&s, 100 + c.heartbeat_timeout_ms, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_KILL, LG_PROC_GATE) >= 0);
}

static void test_start_grace_hang(void) {
    lg_sup_cfg_t c = gate_only();
    lg_sup_t s;
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n;

    lg_sup_init(&s, &c);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 5, 1000) == 0);
    n = lg_sup_step(&s, 1000 + c.start_grace_ms - 1, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_KILL, LG_PROC_GATE) < 0);
    n = lg_sup_step(&s, 1000 + c.start_grace_ms, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_KILL, LG_PROC_GATE) >= 0);
    CHECK(s.proc[LG_PROC_GATE].why == LG_SUP_WHY_HANG);
}

static void test_spawn_refusals(void) {
    lg_sup_cfg_t c = lg_sup_cfg_default();
    lg_sup_t s;

    lg_sup_init(&s, &c);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 0, 0) == -1);              /* pid 0 */
    CHECK(lg_sup_on_spawn(&s, LG_PROC_HEALTH, 4, 0) == -1);            /* not installed */
    CHECK(lg_sup_on_spawn(&s, LG_PROC_COUNT, 4, 0) == -1);             /* outside the table */
    CHECK(lg_sup_on_spawn(&s, 255, 4, 0) == -1);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 4, 0) == 0);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 5, 0) == -1);              /* not IDLE any more */
    CHECK(s.proc[LG_PROC_GATE].pid == 4);
    /* A failed spawn report for a process that is not IDLE or not installed changes nothing. */
    lg_sup_on_spawn_failed(&s, LG_PROC_GATE, 5);
    lg_sup_on_spawn_failed(&s, LG_PROC_HEALTH, 5);
    lg_sup_on_spawn_failed(&s, 255, 5);
    CHECK(s.proc[LG_PROC_GATE].phase == LG_SUP_STARTING && s.proc[LG_PROC_GATE].total_restarts == 0);
    CHECK(s.proc[LG_PROC_HEALTH].total_restarts == 0);
}

static void test_dead_is_absorbing(void) {
    lg_sup_cfg_t c = gate_only();
    lg_sup_t s;
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n;
    int k;
    uint32_t t = 100;

    c.budget = 1;
    lg_sup_init(&s, &c);
    CHECK(gate_fail_by_exit(&s, 1, t) == LG_SUP_IDLE);
    CHECK(gate_fail_by_exit(&s, 2, t + 10) == LG_SUP_DEAD);
    n = lg_sup_step(&s, t + 20, a, LG_SUP_MAX_ACTIONS);
    k = find_act(a, n, LG_SUP_ACT_STATUS, LG_PROC_GATE);
    CHECK(k >= 0);
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_GATE) < 0);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 3, t + 30) == -1);
    lg_sup_on_spawn_failed(&s, LG_PROC_GATE, t + 30);
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 2, t + 30) == 0);
    CHECK(lg_sup_on_exit(&s, 2, t + 30) == 0);
    /* Even after the window has long passed. */
    n = lg_sup_step(&s, t + 10 * c.window_ms, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_GATE) < 0);
    CHECK(s.proc[LG_PROC_GATE].phase == LG_SUP_DEAD && lg_sup_reported_state(&s, LG_PROC_GATE) == LG_PROC_DEAD);
}

static void test_spawn_failures_lead_to_dead(void) {
    lg_sup_cfg_t c = lg_sup_cfg_default();
    lg_sup_t s;
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n;
    unsigned k;
    uint32_t t = 0;

    c.window_ms = 1000000;
    lg_sup_init(&s, &c);
    /* The gate (stage 0) never starts: after the budget it is DEAD and the next stage is released. */
    for (k = 0; k <= c.budget; k++) {
        n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);
        CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_GATE) >= 0);
        CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_PLANT) < 0); /* held back by stage 0 */
        lg_sup_on_spawn_failed(&s, LG_PROC_GATE, t);
        t += 10;
    }
    CHECK(s.proc[LG_PROC_GATE].phase == LG_SUP_DEAD && s.proc[LG_PROC_GATE].why == LG_SUP_WHY_SPAWN);
    n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_PLANT) >= 0);
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_GATE) < 0);
}

static void test_boot_order(void) {
    lg_sup_cfg_t c = lg_sup_cfg_default();
    lg_sup_t s;
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n;
    int i;

    lg_sup_init(&s, &c);
    n = lg_sup_step(&s, 0, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_GATE) >= 0);
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_PLANT) < 0);
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_CONTROLLER) < 0);
    CHECK(n <= LG_SUP_MAX_ACTIONS);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 10, 0) == 0);
    /* STARTING is not enough: it must have beaten. */
    n = lg_sup_step(&s, 10, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_PLANT) < 0);
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 10, 20) == 1);
    n = lg_sup_step(&s, 20, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_PLANT) >= 0);
    for (i = 0; i < LG_DIR_COUNT * LG_SENSOR_ID_COUNT; i++) {
        CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, (uint8_t)(LG_PROC_SENSOR_BASE + i)) >= 0);
    }
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_CONTROLLER) < 0); /* stage 2 waits for stage 1 */
}

static void test_restart_never_waits(void) {
    lg_sup_cfg_t c = lg_sup_cfg_default();
    lg_sup_t s;
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n;
    uint32_t t = 0;
    int p;
    uint32_t pid = 100;

    lg_sup_init(&s, &c);
    /* Bring everything up stage by stage. */
    for (p = 0; p < 3; p++) {
        int q;

        n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);
        for (q = 0; q < (int)n; q++) {
            if (a[q].kind == LG_SUP_ACT_SPAWN) {
                CHECK(lg_sup_on_spawn(&s, a[q].proc, pid, t) == 0);
                CHECK(lg_sup_on_heartbeat(&s, a[q].proc, pid, t) == 1);
                pid++;
            }
        }
        t += 10;
    }
    for (p = 0; p < LG_PROC_COUNT; p++) {
        if (c.installed[p]) {
            CHECK(s.proc[p].phase == LG_SUP_RUNNING);
        }
    }
    /* The gate (stage 0) is down and not yet restarted; the controller (stage 2) fails: it restarts at once. */
    CHECK(lg_sup_on_exit(&s, s.proc[LG_PROC_GATE].pid, t) == 1);
    CHECK(lg_sup_on_exit(&s, s.proc[LG_PROC_CONTROLLER].pid, t) == 1);
    n = lg_sup_step(&s, t + 1, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_CONTROLLER) >= 0);
    CHECK(find_act(a, n, LG_SUP_ACT_SPAWN, LG_PROC_GATE) >= 0);
}

static void test_uninstalled_only(void) {
    lg_sup_cfg_t c = lg_sup_cfg_default();
    lg_sup_t s;
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    int i;

    for (i = 0; i < LG_PROC_COUNT; i++) {
        c.installed[i] = 0;
    }
    lg_sup_init(&s, &c);
    CHECK(lg_sup_step(&s, 0, a, LG_SUP_MAX_ACTIONS) == 0);
    CHECK(lg_sup_reported_state(&s, LG_PROC_GATE) == LG_PROC_RUNNING); /* not part of the system */
    CHECK(lg_sup_restarts_in_window(&s, LG_PROC_GATE, 0) == 0);
    CHECK(lg_sup_reported_state(&s, 255) == LG_PROC_RUNNING);
    CHECK(lg_sup_restarts_in_window(&s, 255, 0) == 0);
    CHECK(strcmp(lg_sup_proc_name(255), "unknown") == 0 && strcmp(lg_sup_proc_name(LG_PROC_COUNT), "unknown") == 0);
    CHECK(strcmp(lg_sup_proc_name(LG_PROC_GATE), "gate") == 0);
}

/* The status action carries the change; it must not be lost when the caller's buffer is small. */
static void test_small_action_buffer(void) {
    lg_sup_cfg_t c = lg_sup_cfg_default();
    lg_sup_t s;
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n;
    size_t first;
    size_t statuses = 0;
    size_t i;
    int installed_n = 0;

    for (i = 0; i < LG_PROC_COUNT; i++) {
        installed_n += c.installed[i] ? 1 : 0;
    }
    lg_sup_init(&s, &c);
    CHECK(lg_sup_step(&s, 0, a, 0) == 0); /* no room: no write, no crash */
    lg_sup_init(&s, &c);
    first = lg_sup_step(&s, 0, a, 1);
    CHECK(first == 1);
    /* Next step with room: every status that was not delivered must still come (DEFECT if lost). */
    n = lg_sup_step(&s, 10, a, LG_SUP_MAX_ACTIONS);
    for (i = 0; i < n; i++) {
        statuses += a[i].kind == LG_SUP_ACT_STATUS ? 1 : 0;
    }
    CHECK(statuses >= (size_t)installed_n - 1);
}

static void test_status_only_on_change(void) {
    lg_sup_cfg_t c = gate_only();
    lg_sup_t s;
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n;

    lg_sup_init(&s, &c);
    n = lg_sup_step(&s, 0, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_STATUS, LG_PROC_GATE) >= 0); /* first report */
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 5, 1) == 0);
    n = lg_sup_step(&s, 2, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_STATUS, LG_PROC_GATE) < 0); /* IDLE -> STARTING: both RESTARTING */
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 5, 3) == 1);
    n = lg_sup_step(&s, 4, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_STATUS, LG_PROC_GATE) >= 0); /* RUNNING */
    /* A crash and restart inside one step interval still shows through the restart counter. */
    CHECK(lg_sup_on_exit(&s, 5, 5) == 1);
    CHECK(lg_sup_on_spawn(&s, LG_PROC_GATE, 6, 5) == 0);
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_GATE, 6, 5) == 1);
    n = lg_sup_step(&s, 6, a, LG_SUP_MAX_ACTIONS);
    CHECK(find_act(a, n, LG_SUP_ACT_STATUS, LG_PROC_GATE) >= 0); /* restarts 0 -> 1 */
}

/* ---- lg_sup_text ---- */

static void make_busy(lg_sup_t *s) {
    int i;

    for (i = 0; i < LG_PROC_COUNT; i++) {
        lg_sup_proc_t *p = &s->proc[i];

        p->phase = LG_SUP_STARTING;
        p->pid = 0xFFFFFFFFu;
        p->total_restarts = 0xFFFFFFFFu;
        p->recovery_ms = 0xFFFFFFFFu;
        p->hb_rx = 0xFFFFFFFFu;
        p->hb_ignored = 0xFFFFFFFFu;
        p->why = LG_SUP_WHY_SPAWN;
    }
    s->exits_ignored = 0xFFFFFFFFu;
    s->hb_unknown = 0xFFFFFFFFu;
}

static void test_text_sizes(void) {
    lg_sup_cfg_t c = lg_sup_cfg_default();
    lg_sup_t s;
    static char big[4096];
    char small[600];
    size_t full;
    size_t len;
    size_t lines = 0;
    size_t i;
    int installed_n = 0;
    uint8_t canary_ok = 1;

    for (i = 0; i < LG_PROC_COUNT; i++) {
        installed_n += c.installed[i] ? 1 : 0;
    }
    lg_sup_init(&s, &c);
    make_busy(&s);
    full = lg_sup_text(&s, 1000, big, sizeof big);
    CHECK(full == strlen(big) && big[full - 1] == '\n');
    for (i = 0; i < full; i++) {
        lines += big[i] == '\n' ? 1 : 0;
    }
    CHECK(lines == (size_t)installed_n + 1); /* no line was dropped for being too long (largest counters) */
    CHECK(strstr(big, "pid=4294967295") != NULL && strstr(big, "hb_ignored=4294967295\n") != NULL);
    CHECK(strstr(big, "proc=sensor/east/island") != NULL && strstr(big, "proc=health") == NULL);

    /* Every length from 0 up: always NUL terminated, never beyond len, always cut at a line end. */
    for (len = 0; len <= full + 2 && len < sizeof small; len++) {
        size_t n;

        memset(small, 0x5A, sizeof small);
        n = lg_sup_text(&s, 1000, small, len);
        if (len == 0) {
            CHECK(n == 0);
            CHECK(small[0] == 0x5A); /* nothing written at all */
        } else {
            CHECK(n < len);
            CHECK(n == strlen(small));
            CHECK(n == 0 || small[n - 1] == '\n');
            CHECK(strncmp(small, big, n) == 0); /* a prefix of the full text */
        }
        for (i = len; i < sizeof small; i++) {
            if (small[i] != 0x5A) {
                canary_ok = 0;
            }
        }
    }
    CHECK(canary_ok);
    /* Exactly the full size + 1 gives everything; the full size gives all but the last line. */
    CHECK(lg_sup_text(&s, 1000, big + 2048, full + 1) == full);
    CHECK(lg_sup_text(&s, 1000, big + 2048, full) < full);
}

static void test_text_content_idle(void) {
    lg_sup_cfg_t c = gate_only();
    lg_sup_t s;
    char buf[400];
    size_t n;

    c.budget = 200; /* clamped, shows in the header */
    lg_sup_init(&s, &c);
    n = lg_sup_text(&s, 0, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "supervisor procs=1 budget=8 ") != NULL);
    CHECK(strstr(buf, "proc=gate id=2 state=RESTARTING phase=IDLE pid=0 restarts=0/8") != NULL);
}

/* ---- guards ---- */

static lg_json_doc_t g_doc;

static int gload(const char *json, lg_guards_t *g, char *err, size_t errlen) {
    CHECK(lg_json_parse(&g_doc, json, strlen(json)) == LG_JSON_OK);
    return lg_guards_from_json(&g_doc, g, err, errlen);
}

static void test_guards_edges(void) {
    lg_guards_t g;
    char err[96];
    char eight[LG_GUARD_ID_LEN];

    /* Size limits. */
    CHECK(gload("{\"guards\": [\"1\", \"2\", \"3\", \"4\", \"5\", \"6\", \"7\", \"8\"]}", &g, err, sizeof err) == 0);
    CHECK(g.count == LG_GUARDS_MAX && lg_guards_allowed(&g, "8", 2) && lg_guards_allowed(&g, "1", 2));
    CHECK(gload("{\"guards\": [\"1\", \"2\", \"3\", \"4\", \"5\", \"6\", \"7\", \"8\", \"9\"]}", &g, err, sizeof err) == -1);
    CHECK(g.count == 0 && !lg_guards_allowed(&g, "1", 2)); /* refused file: nothing left behind */
    CHECK(gload("{\"guards\": [\"1234567\"]}", &g, err, sizeof err) == 0 && g.count == 1);
    CHECK(lg_guards_allowed(&g, "1234567", 8));
    CHECK(gload("{\"guards\": [\"12345678\"]}", &g, err, sizeof err) == -1);
    CHECK(gload("{\"guards\": []}", &g, err, sizeof err) == 0 && g.count == 0);
    CHECK(!lg_guards_allowed(&g, "", 1));

    /* A valid entry followed by an invalid one: the whole list is refused and cleared. */
    CHECK(gload("{\"guards\": [\"G01\", \"G 2\"]}", &g, err, sizeof err) == -1);
    CHECK(g.count == 0 && g.id[0][0] == '\0' && !lg_guards_allowed(&g, "G01", 4));
    CHECK(gload("{\"guards\": [\"G01\", \"G01\"]}", &g, err, sizeof err) == -1 && strstr(err, "duplicate") != NULL);
    CHECK(gload("{\"guards\": [\"G01\", \"g01\"]}", &g, err, sizeof err) == 0 && g.count == 2); /* case matters */
    CHECK(lg_guards_allowed(&g, "g01", 4) && lg_guards_allowed(&g, "G01", 4));

    /* Characters. */
    CHECK(gload("{\"guards\": [\"G.1\"]}", &g, err, sizeof err) == -1);
    CHECK(gload("{\"guards\": [\"G\\n1\"]}", &g, err, sizeof err) == -1);
    CHECK(gload("{\"guards\": [\"G/1\"]}", &g, err, sizeof err) == -1);
    CHECK(gload("{\"guards\": [\"\xC3\xA9\"]}", &g, err, sizeof err) == -1);
    CHECK(gload("{\"guards\": [\"a_b-C9\"]}", &g, err, sizeof err) == 0 && lg_guards_allowed(&g, "a_b-C9", 7));

    /* Types. */
    CHECK(gload("[\"G01\"]", &g, err, sizeof err) == -1 && strstr(err, "object") != NULL);
    CHECK(gload("\"G01\"", &g, err, sizeof err) == -1);
    CHECK(gload("{\"guards\": null}", &g, err, sizeof err) == -1);
    CHECK(gload("{\"guards\": {\"a\": \"G01\"}}", &g, err, sizeof err) == -1);
    CHECK(gload("{\"guards\": [[\"G01\"]]}", &g, err, sizeof err) == -1);
    CHECK(gload("{\"guards\": [null]}", &g, err, sizeof err) == -1);
    CHECK(gload("{\"guards\": [true]}", &g, err, sizeof err) == -1);
    CHECK(gload("{}", &g, err, sizeof err) == 0 && g.count == 0);

    /* err may be NULL or tiny. */
    CHECK(gload("{\"guards\": \"x\"}", &g, NULL, 0) == -1);
    CHECK(gload("{\"guards\": \"x\"}", &g, err, 0) == -1);
    CHECK(gload("{\"guards\": \"x\"}", &g, err, 1) == -1 && err[0] == '\0');
    CHECK(gload("{\"guards\": \"x\"}", &g, err, 5) == -1 && strlen(err) == 4);

    /* Lookup: field lengths. */
    CHECK(gload("{\"guards\": [\"G01\"]}", &g, err, sizeof err) == 0);
    memset(eight, 'G', sizeof eight);
    CHECK(!lg_guards_allowed(&g, eight, sizeof eight)); /* no NUL at all */
    memcpy(eight, "G01\0", 4);
    CHECK(lg_guards_allowed(&g, eight, 4) && lg_guards_allowed(&g, eight, 8));
    CHECK(!lg_guards_allowed(&g, eight, 0));
    CHECK(!lg_guards_allowed(&g, eight, 3));
    {
        lg_guards_t empty;

        memset(&empty, 0, sizeof empty);
        CHECK(!lg_guards_allowed(&empty, "", 1)); /* empty id never matches an empty list */
    }
}

/* ---- controller: lg_ctl_on_proc_status ---- */

static lg_ctl_out_t g_last;

static void ctl_tick(lg_ctl_t *c, uint32_t now) {
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

static void ctl_run(lg_ctl_t *c, uint32_t *t, uint32_t ms) {
    uint32_t end = *t + ms;

    for (; *t != end; *t += 10) {
        ctl_tick(c, *t);
    }
}

static void ctl_released(lg_ctl_t *c, uint32_t *t) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();

    lg_ctl_init(c, &cfg, *t);
    ctl_run(c, t, LG_T_BOOT_STABLE_MS + 500);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
}

static void test_ctl_restarts_255_and_states(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    int p;

    ctl_released(&c, &t);
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_PLANT, LG_PROC_RUNNING, 255, t) == 0);
    CHECK(c.proc_restarts[LG_PROC_PLANT] == 255);
    ctl_run(&c, &t, 100);
    CHECK(g_last.level == LG_LEVEL_NORMAL); /* the counter is informational, only the state counts */
    /* Every allowed process id is accepted with the largest counter and every state value. */
    for (p = 0; p < LG_PROC_COUNT; p++) {
        if (p != LG_PROC_SUPERVISOR && p != LG_PROC_CONTROLLER) {
            CHECK(lg_ctl_on_proc_status(&c, (uint8_t)p, LG_PROC_RUNNING, 255, t) == 0);
            CHECK(c.proc_state[p] == LG_PROC_RUNNING && c.proc_restarts[p] == 255);
        }
    }
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_PLANT, 255, 255, t) == 0);
    CHECK(c.proc_state[LG_PROC_PLANT] == LG_PROC_DEAD && c.proc_status_unknown == 1);
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_PLANT, 3, 0, t) == 0);
    CHECK(c.proc_state[LG_PROC_PLANT] == LG_PROC_DEAD && c.proc_status_unknown == 2);
    ctl_run(&c, &t, 100);
    CHECK(g_last.level >= LG_LEVEL_DEGRADED);
}

static void test_ctl_bridge_only_early_warning(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    ctl_released(&c, &t);
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_BRIDGE, LG_PROC_DEAD, 3, t) == 0);
    ctl_run(&c, &t, 100);
    CHECK(g_last.level == LG_LEVEL_EARLY_WARNING); /* the bridge has no part in gate control */
    CHECK(g_last.gate_target == LG_GATE_TARGET_UP);
}

/* A status report must never open the gate: with a lost direction the gate stays down or goes down. */
static void test_ctl_all_sensors_of_one_direction_lost(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    int i;

    ctl_released(&c, &t);
    for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
        CHECK(lg_ctl_on_proc_status(&c, (uint8_t)SENS(LG_DIR_WEST, i), LG_PROC_DEAD, 3, t) == 0);
    }
    ctl_run(&c, &t, 100);
    CHECK(g_last.level >= LG_LEVEL_UNCERTAIN);
    CHECK(g_last.gate_target == LG_GATE_TARGET_DOWN);
}

static void test_ctl_silence_edge(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();

    /* sup_silence_ms = 0 : armed, then judged silent in the very next step (no status in that step). */
    cfg.sup_silence_ms = 0;
    lg_ctl_init(&c, &cfg, t);
    ctl_run(&c, &t, LG_T_BOOT_STABLE_MS + 500);
    CHECK(lg_ctl_on_proc_status(&c, LG_PROC_PLANT, LG_PROC_RUNNING, 0, t) == 0);
    ctl_tick(&c, t); /* same ms as the status: elapsed 0 >= 0 */
    CHECK(c.sup_silent == 1);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    CHECK(g_last.gate_target == LG_GATE_TARGET_DOWN);
}

int main(void) {
    test_budget_clamp();
    test_window_zero();
    test_window_boundary_and_expiry();
    test_window_across_clock_wrap();
    test_same_pid_two_processes();
    test_pid_reuse_after_exit();
    test_exit_while_starting();
    test_first_start_has_no_recovery_time();
    test_exit_while_killing_keeps_detection_time();
    test_heartbeat_edge_cases();
    test_heartbeat_timeout_boundary();
    test_start_grace_hang();
    test_spawn_refusals();
    test_dead_is_absorbing();
    test_spawn_failures_lead_to_dead();
    test_boot_order();
    test_restart_never_waits();
    test_uninstalled_only();
    test_small_action_buffer();
    test_status_only_on_change();
    test_text_sizes();
    test_text_content_idle();
    test_guards_edges();
    test_ctl_restarts_255_and_states();
    test_ctl_bridge_only_early_warning();
    test_ctl_all_sensors_of_one_direction_lost();
    test_ctl_silence_edge();
    LG_TEST_DONE("test_supervisor_extra");
}
