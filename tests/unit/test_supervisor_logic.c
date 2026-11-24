#include <stdio.h>
#include <string.h>

#include "lg_supervisor_logic.h"
#include "lg_test.h"

#define GATE LG_PROC_GATE
#define SENS_WA LG_PROC_SENSOR_BASE
#define CTL LG_PROC_CONTROLLER
#define PLANT LG_PROC_PLANT

static uint32_t g_next_pid = 1000;

static size_t count_kind(const lg_sup_action_t *a, size_t n, int kind, int proc) {
    size_t c = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        if (a[i].kind == kind && (proc < 0 || a[i].proc == proc)) {
            c++;
        }
    }
    return c;
}

/* One step; every SPAWN is answered with a fresh pid at the same time. */
static size_t step(lg_sup_t *s, uint32_t now, lg_sup_action_t *out) {
    size_t n = lg_sup_step(s, now, out, LG_SUP_MAX_ACTIONS);
    size_t i;

    for (i = 0; i < n; i++) {
        if (out[i].kind == LG_SUP_ACT_SPAWN) {
            CHECK(lg_sup_on_spawn(s, out[i].proc, ++g_next_pid, now) == 0);
        }
    }
    return n;
}

static void beat_all(lg_sup_t *s, uint32_t now) {
    int i;

    for (i = 0; i < LG_PROC_COUNT; i++) {
        if (s->cfg.installed[i] && s->proc[i].pid != 0) {
            (void)lg_sup_on_heartbeat(s, (uint8_t)i, s->proc[i].pid, now);
        }
    }
}

static int all_running(const lg_sup_t *s) {
    int i;

    for (i = 0; i < LG_PROC_COUNT; i++) {
        if (s->cfg.installed[i] && s->proc[i].phase != LG_SUP_RUNNING) {
            return 0;
        }
    }
    return 1;
}

/* Boots everything stage by stage; returns the time when all are RUNNING (0 = never). */
static uint32_t boot(lg_sup_t *s, uint32_t t0) {
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    uint32_t t = t0;
    int guard;

    for (guard = 0; guard < 100; guard++) {
        (void)step(s, t, a);
        beat_all(s, t);
        if (all_running(s)) {
            return t;
        }
        t += 10;
    }
    return 0;
}

static void test_boot_order(void) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n;

    lg_sup_init(&s, &cfg);
    n = lg_sup_step(&s, 0, a, LG_SUP_MAX_ACTIONS);
    CHECK(count_kind(a, n, LG_SUP_ACT_SPAWN, -1) == 1); /* only the gate (stage 0) */
    CHECK(count_kind(a, n, LG_SUP_ACT_SPAWN, GATE) == 1);
    CHECK(lg_sup_on_spawn(&s, GATE, 500, 0) == 0);
    n = step(&s, 10, a);
    CHECK(count_kind(a, n, LG_SUP_ACT_SPAWN, -1) == 0); /* the gate has not beaten yet */
    CHECK(lg_sup_reported_state(&s, GATE) == LG_PROC_RESTARTING);
    CHECK(lg_sup_reported_state(&s, CTL) == LG_PROC_RESTARTING);
    CHECK(lg_sup_reported_state(&s, LG_PROC_SCHEDULE) == LG_PROC_RUNNING); /* not installed */
    CHECK(lg_sup_on_heartbeat(&s, GATE, 500, 20) == 1);
    n = step(&s, 30, a);
    CHECK(count_kind(a, n, LG_SUP_ACT_SPAWN, -1) == 7); /* plant and six sensors */
    CHECK(count_kind(a, n, LG_SUP_ACT_SPAWN, CTL) == 0);
    CHECK(count_kind(a, n, LG_SUP_ACT_SPAWN, PLANT) == 1);
    beat_all(&s, 40);
    n = step(&s, 50, a);
    CHECK(count_kind(a, n, LG_SUP_ACT_SPAWN, CTL) == 1); /* controller last */
    beat_all(&s, 60);
    CHECK(s.proc[CTL].phase == LG_SUP_RUNNING);
    CHECK(lg_sup_reported_state(&s, CTL) == LG_PROC_RUNNING);
    CHECK(s.proc[CTL].recovery_ms == 0 && s.proc[GATE].total_restarts == 0);
}

static void test_dead_lower_stage_does_not_block_boot(void) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    uint32_t t = 0;
    int i;

    lg_sup_init(&s, &cfg);
    (void)step(&s, t, a); /* gate spawned */
    /* The gate keeps dying: 4 exits in a second give up on it. */
    for (i = 0; i < 4; i++) {
        t += 100;
        CHECK(lg_sup_on_exit(&s, s.proc[GATE].pid, t) == 1);
        (void)step(&s, t, a);
    }
    CHECK(s.proc[GATE].phase == LG_SUP_DEAD);
    t += 10;
    (void)step(&s, t, a);
    CHECK(s.proc[PLANT].phase == LG_SUP_STARTING); /* stage 1 went on without the gate */
}

static void test_exit_restart_and_recovery(void) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    uint32_t t = boot((lg_sup_init(&s, &cfg), &s), 0);
    uint32_t old_pid = s.proc[SENS_WA].pid;
    size_t n;

    CHECK(t != 0);
    t += 1000;
    CHECK(lg_sup_on_exit(&s, old_pid, t) == 1);
    CHECK(lg_sup_reported_state(&s, SENS_WA) == LG_PROC_RESTARTING);
    CHECK(s.proc[SENS_WA].total_restarts == 1 && s.proc[SENS_WA].why == LG_SUP_WHY_EXIT);
    n = step(&s, t + 10, a);
    CHECK(count_kind(a, n, LG_SUP_ACT_SPAWN, SENS_WA) == 1); /* no stage wait for a restart */
    CHECK(count_kind(a, n, LG_SUP_ACT_STATUS, SENS_WA) == 1);
    /* A late beat of the old incarnation must not make the new one look alive. */
    CHECK(lg_sup_on_heartbeat(&s, SENS_WA, old_pid, t + 20) == 0);
    CHECK(s.proc[SENS_WA].hb_ignored == 1 && s.proc[SENS_WA].phase == LG_SUP_STARTING);
    CHECK(lg_sup_on_heartbeat(&s, SENS_WA, s.proc[SENS_WA].pid, t + 300) == 1);
    CHECK(s.proc[SENS_WA].phase == LG_SUP_RUNNING);
    CHECK(s.proc[SENS_WA].recovery_ms == 300);
    n = step(&s, t + 310, a);
    CHECK(count_kind(a, n, LG_SUP_ACT_STATUS, SENS_WA) == 1); /* back to RUNNING, restarts=1/3 */
    CHECK(lg_sup_reported_state(&s, SENS_WA) == LG_PROC_RUNNING);
    CHECK(lg_sup_restarts_in_window(&s, SENS_WA, t + 310) == 1);
    /* Other processes were not touched. */
    CHECK(s.proc[GATE].total_restarts == 0 && s.proc[CTL].total_restarts == 0);
}

static void test_hang_detection(void) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    uint32_t t = boot((lg_sup_init(&s, &cfg), &s), 0);
    uint32_t last = t;
    uint32_t pid;
    size_t n;

    pid = s.proc[CTL].pid;
    /* Everything beats except the controller. */
    for (t = last + 10; t < last + LG_HEARTBEAT_TIMEOUT_MS; t += 10) {
        beat_all(&s, t);
        s.proc[CTL].last_hb_ms = last; /* the controller's beat is the one that is missing */
        n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);
        CHECK(count_kind(a, n, LG_SUP_ACT_KILL, -1) == 0); /* not before the timeout */
    }
    t = last + LG_HEARTBEAT_TIMEOUT_MS;
    beat_all(&s, t);
    s.proc[CTL].last_hb_ms = last;
    n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);
    CHECK(count_kind(a, n, LG_SUP_ACT_KILL, CTL) == 1);
    {
        size_t k;
        uint32_t kill_pid = 0;

        for (k = 0; k < n; k++) {
            if (a[k].kind == LG_SUP_ACT_KILL && a[k].proc == CTL) {
                kill_pid = a[k].pid;
            }
        }
        CHECK(kill_pid == pid); /* the kill names the pid of the hung process */
    }
    CHECK(s.proc[CTL].phase == LG_SUP_KILLING);
    CHECK(lg_sup_reported_state(&s, CTL) == LG_PROC_RESTARTING);
    /* The kill is repeated while the process does not go away. */
    n = lg_sup_step(&s, t + 100, a, LG_SUP_MAX_ACTIONS);
    CHECK(count_kind(a, n, LG_SUP_ACT_KILL, CTL) == 0);
    n = lg_sup_step(&s, t + 500, a, LG_SUP_MAX_ACTIONS);
    CHECK(count_kind(a, n, LG_SUP_ACT_KILL, CTL) == 1);
    /* A beat from a process that is being killed does not save it. */
    CHECK(lg_sup_on_heartbeat(&s, CTL, pid, t + 510) == 0);
    CHECK(lg_sup_on_exit(&s, pid, t + 600) == 1);
    CHECK(s.proc[CTL].why == LG_SUP_WHY_HANG && s.proc[CTL].phase == LG_SUP_IDLE);
    n = step(&s, t + 610, a);
    CHECK(count_kind(a, n, LG_SUP_ACT_SPAWN, CTL) == 1);
    CHECK(lg_sup_on_heartbeat(&s, CTL, s.proc[CTL].pid, t + 700) == 1);
    CHECK(s.proc[CTL].recovery_ms == 700); /* from the moment the hang was detected */
}

/* A system-wide stall (a VM pause) must not look like a hang of every process. */
static void test_stall_is_forgiven(void) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    uint32_t t = boot((lg_sup_init(&s, &cfg), &s), 0);
    uint32_t last = t;
    size_t n;

    CHECK(t != 0);
    /* The supervisor itself comes back after 210 ms: every beat is older than the 200 ms limit. */
    t = last + 210;
    n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);
    CHECK(count_kind(a, n, LG_SUP_ACT_KILL, -1) == 0);
    CHECK(s.stalls == 1);
    CHECK(all_running(&s));
    /* The clocks were restarted, not switched off: a process that stays quiet afterwards is killed. */
    beat_all(&s, t + 10);
    s.proc[GATE].last_hb_ms = t; /* the gate stops beating from here */
    for (t = t + 10; t < last + 210 + LG_HEARTBEAT_TIMEOUT_MS; t += 10) {
        beat_all(&s, t);
        s.proc[GATE].last_hb_ms = last + 210;
        n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);
        CHECK(count_kind(a, n, LG_SUP_ACT_KILL, GATE) == 0);
    }
    beat_all(&s, t);
    s.proc[GATE].last_hb_ms = last + 210;
    n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);
    CHECK(count_kind(a, n, LG_SUP_ACT_KILL, GATE) == 1);
    CHECK(s.stalls == 1);
    /* A stall in KILLING does not cancel the kill. */
    t += 100;
    n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);
    CHECK(s.proc[GATE].phase == LG_SUP_KILLING && s.stalls == 2);
    /* Off when stall_ms is 0. */
    cfg.stall_ms = 0;
    t = boot((lg_sup_init(&s, &cfg), &s), 0);
    n = lg_sup_step(&s, t + 210, a, LG_SUP_MAX_ACTIONS);
    CHECK(count_kind(a, n, LG_SUP_ACT_KILL, -1) > 0 && s.stalls == 0);
}

static void test_start_grace(void) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n;

    uint32_t t;

    lg_sup_init(&s, &cfg);
    (void)step(&s, 0, a);
    /* The real loop steps every tick: a jump of 1.5 s would be read as a stall of the whole system. */
    for (t = 10; t < LG_SUP_START_GRACE_MS; t += 10) {
        n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);
        CHECK(count_kind(a, n, LG_SUP_ACT_KILL, -1) == 0);
    }
    n = lg_sup_step(&s, LG_SUP_START_GRACE_MS, a, LG_SUP_MAX_ACTIONS);
    CHECK(count_kind(a, n, LG_SUP_ACT_KILL, GATE) == 1); /* never beat: counts as hung */
    CHECK(s.stalls == 0);
}

static void test_budget(void) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    uint32_t t = boot((lg_sup_init(&s, &cfg), &s), 0);
    int i;
    size_t n;

    t += 100;
    /* Three restarts in 10 s are allowed, the fourth failure gives up. */
    for (i = 0; i < 3; i++) {
        CHECK(lg_sup_on_exit(&s, s.proc[SENS_WA].pid, t) == 1);
        CHECK(s.proc[SENS_WA].phase == LG_SUP_IDLE);
        CHECK(lg_sup_restarts_in_window(&s, SENS_WA, t) == i + 1);
        (void)step(&s, t + 1, a);
        CHECK(lg_sup_on_heartbeat(&s, SENS_WA, s.proc[SENS_WA].pid, t + 5) == 1);
        t += 1000;
    }
    CHECK(lg_sup_on_exit(&s, s.proc[SENS_WA].pid, t) == 1);
    CHECK(s.proc[SENS_WA].phase == LG_SUP_DEAD);
    CHECK(lg_sup_reported_state(&s, SENS_WA) == LG_PROC_DEAD);
    CHECK(s.proc[SENS_WA].total_restarts == 3);
    n = step(&s, t + 1, a);
    CHECK(count_kind(a, n, LG_SUP_ACT_SPAWN, SENS_WA) == 0);
    CHECK(count_kind(a, n, LG_SUP_ACT_STATUS, SENS_WA) == 1);
    /* DEAD is absorbing. */
    CHECK(lg_sup_on_spawn(&s, SENS_WA, 99999, t + 2) == -1);
    CHECK(lg_sup_on_heartbeat(&s, SENS_WA, 99999, t + 3) == 0);
    n = step(&s, t + 100000, a);
    CHECK(count_kind(a, n, LG_SUP_ACT_SPAWN, SENS_WA) == 0 && s.proc[SENS_WA].phase == LG_SUP_DEAD);
    /* The others go on. */
    CHECK(s.proc[SENS_WA + 1].phase != LG_SUP_DEAD);
}

static void test_budget_window_slides(void) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    uint32_t t = boot((lg_sup_init(&s, &cfg), &s), 0);
    int i;

    t += 100;
    /* One failure every 4 s: at most 3 in any 10 s window, so never DEAD. */
    for (i = 0; i < 20; i++) {
        beat_all(&s, t);
        CHECK(lg_sup_on_exit(&s, s.proc[SENS_WA].pid, t) == 1);
        CHECK(s.proc[SENS_WA].phase == LG_SUP_IDLE);
        (void)step(&s, t + 1, a);
        beat_all(&s, t + 5);
        t += 4000;
        (void)step(&s, t - 100, a);
        beat_all(&s, t - 100);
    }
    CHECK(s.proc[SENS_WA].phase != LG_SUP_DEAD);
    CHECK(s.proc[SENS_WA].total_restarts == 20);
}

static void test_spawn_failure_counts(void) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    uint32_t t = 0;
    int i;

    lg_sup_init(&s, &cfg);
    for (i = 0; i < 4; i++) {
        size_t n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);

        CHECK(count_kind(a, n, LG_SUP_ACT_SPAWN, GATE) == 1); /* the first 4 attempts: 1 + budget 3 */
        lg_sup_on_spawn_failed(&s, GATE, t);
        t += 10;
    }
    {
        size_t n = lg_sup_step(&s, t + 10, a, LG_SUP_MAX_ACTIONS);

        CHECK(count_kind(a, n, LG_SUP_ACT_SPAWN, GATE) == 0); /* no 5th attempt */
    }
    CHECK(s.proc[GATE].phase == LG_SUP_DEAD);
    CHECK(s.proc[GATE].why == LG_SUP_WHY_SPAWN);
    CHECK(lg_sup_on_spawn(&s, SENS_WA, 0, t) == -1); /* pid 0 refused */
}

static void test_unknown_input(void) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    uint32_t t = boot((lg_sup_init(&s, &cfg), &s), 0);

    CHECK(lg_sup_on_exit(&s, 424242, t) == 0 && s.exits_ignored == 1);
    CHECK(lg_sup_on_exit(&s, 0, t) == 0 && s.exits_ignored == 2);
    CHECK(lg_sup_on_heartbeat(&s, 31, 5, t) == 0 && s.hb_unknown == 1);
    CHECK(lg_sup_on_heartbeat(&s, LG_PROC_BRIDGE, 5, t) == 0); /* not installed */
    CHECK(lg_sup_on_heartbeat(&s, GATE, 0, t) == 0);           /* pid 0 never matches */
    CHECK(lg_sup_on_heartbeat(&s, GATE, s.proc[GATE].pid + 1, t) == 0);
    CHECK(lg_sup_on_spawn(&s, GATE, 77, t) == -1); /* not IDLE */
    CHECK(lg_sup_on_spawn(&s, LG_PROC_SCHEDULE, 77, t) == -1);
    CHECK(s.proc[GATE].phase == LG_SUP_RUNNING);
}

static void test_status_actions(void) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    size_t n;
    uint32_t t;

    lg_sup_init(&s, &cfg);
    n = lg_sup_step(&s, 0, a, LG_SUP_MAX_ACTIONS);
    CHECK(count_kind(a, n, LG_SUP_ACT_STATUS, -1) == 11); /* gate, plant, six sensors, controller, bridge (Step 10), health (Step 11) */
    CHECK(count_kind(a, n, LG_SUP_ACT_STATUS, LG_PROC_SCHEDULE) == 0);
    t = boot(&s, 10);
    CHECK(t != 0);
    n = lg_sup_step(&s, t + 10, a, LG_SUP_MAX_ACTIONS);
    n = lg_sup_step(&s, t + 20, a, LG_SUP_MAX_ACTIONS);
    CHECK(n == 0); /* nothing changed: nothing to report */
    /* A restart leaves the window after 10 s: the count change is reported once. */
    CHECK(lg_sup_on_exit(&s, s.proc[PLANT].pid, t + 30) == 1);
    (void)step(&s, t + 40, a);
    beat_all(&s, t + 50);
    (void)step(&s, t + 60, a);
    n = lg_sup_step(&s, t + 30 + LG_RESTART_WINDOW_MS, a, LG_SUP_MAX_ACTIONS);
    CHECK(count_kind(a, n, LG_SUP_ACT_STATUS, PLANT) == 1);
    CHECK(lg_sup_restarts_in_window(&s, PLANT, t + 30 + LG_RESTART_WINDOW_MS) == 0);
}

static void test_clock_wrap(void) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    uint32_t t0 = 0xFFFFFF00u;
    uint32_t t = boot((lg_sup_init(&s, &cfg), &s), t0);
    uint32_t end = t + 2000; /* crosses the wrap of the 32-bit counter */
    size_t n;

    CHECK(t != 0);
    for (; t != end; t += 10) {
        beat_all(&s, t);
        n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);
        CHECK(count_kind(a, n, LG_SUP_ACT_KILL, -1) == 0);
    }
    CHECK(all_running(&s));
    /* The gate goes quiet across the wrap. */
    {
        uint32_t last = t;

        for (; t != last + LG_HEARTBEAT_TIMEOUT_MS + 10; t += 10) {
            beat_all(&s, t);
            s.proc[GATE].last_hb_ms = last;
            n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);
            if (count_kind(a, n, LG_SUP_ACT_KILL, GATE) != 0) {
                CHECK(t - last >= LG_HEARTBEAT_TIMEOUT_MS);
                break;
            }
        }
        CHECK(s.proc[GATE].phase == LG_SUP_KILLING);
    }
}

static void test_text(void) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    char buf[4096];
    char small[200];
    size_t n;
    uint32_t t = boot((lg_sup_init(&s, &cfg), &s), 0);

    n = lg_sup_text(&s, t, buf, sizeof buf);
    CHECK(n == strlen(buf) && n > 0);
    CHECK(strstr(buf, "proc=gate id=2 state=RUNNING phase=RUNNING") != NULL);
    CHECK(strstr(buf, "proc=sensor/west/a id=3") != NULL);
    CHECK(strstr(buf, "proc=sensor/east/island id=8") != NULL);
    CHECK(strstr(buf, "proc=bridge id=12") != NULL); /* installed since Step 10 */
    CHECK(strstr(buf, "proc=health id=10") != NULL);   /* installed since Step 11 */
    CHECK(strstr(buf, "proc=schedule") == NULL);       /* not installed (Step 12) */
    CHECK(strstr(buf, "restarts=0/3") != NULL);
    /* A small buffer is cut at a line end and stays NUL terminated. */
    n = lg_sup_text(&s, t, small, sizeof small);
    CHECK(n == strlen(small));
    CHECK(n == 0 || small[n - 1] == '\n');
    CHECK(lg_sup_text(&s, t, small, 0) == 0);
    CHECK(lg_sup_text(&s, t, small, 5) == 0 && small[0] == '\0');
    CHECK(strcmp(lg_sup_proc_name(LG_PROC_GATE), "gate") == 0 && strcmp(lg_sup_proc_name(200), "unknown") == 0);
}

/* Seeded random walk: random exits, silences and spawn failures; invariants checked every step. */
static uint32_t g_rng;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void test_random_walk(uint32_t seed) {
    lg_sup_t s;
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    lg_sup_action_t a[LG_SUP_MAX_ACTIONS];
    uint32_t spawn_ms[LG_PROC_COUNT][64];
    uint32_t spawn_n[LG_PROC_COUNT];
    uint8_t was_dead[LG_PROC_COUNT];
    uint8_t silent[LG_PROC_COUNT];
    uint32_t t = 0xFFFFF000u; /* starts just before the wrap */
    int step_i;
    int viol = 0;
    uint32_t kills = 0;

    g_rng = seed;
    memset(spawn_n, 0, sizeof spawn_n);
    memset(was_dead, 0, sizeof was_dead);
    memset(silent, 0, sizeof silent);
    lg_sup_init(&s, &cfg);
    for (step_i = 0; step_i < 200000; step_i++) {
        size_t n;
        size_t i;
        int p;

        t += 10;
        /* Random events. */
        for (p = 0; p < LG_PROC_COUNT; p++) {
            lg_sup_proc_t *pp = &s.proc[p];

            if (!cfg.installed[p] || pp->pid == 0) {
                continue;
            }
            /* Every third process is flaky (runs out of budget), the others fail now and then. */
            if (rnd() % (p % 3 == 0 ? 300u : 6000u) == 0) {
                (void)lg_sup_on_exit(&s, pp->pid, t);
            } else if (rnd() % (p % 3 == 0 ? 2000u : 9000u) == 0) {
                silent[p] = 1; /* stops beating until it is replaced */
            } else if (!silent[p] && step_i % 5 == p % 5) { /* a beat every 50 ms, like the real ones */
                (void)lg_sup_on_heartbeat(&s, (uint8_t)p, pp->pid, t);
            }
            if (rnd() % 900 == 0) {
                (void)lg_sup_on_heartbeat(&s, (uint8_t)p, pp->pid + 1, t); /* wrong pid */
            }
        }
        n = lg_sup_step(&s, t, a, LG_SUP_MAX_ACTIONS);
        if (n > LG_SUP_MAX_ACTIONS) {
            viol++;
        }
        for (i = 0; i < n; i++) {
            int proc = a[i].proc;

            if (a[i].kind == LG_SUP_ACT_SPAWN) {
                uint32_t k;
                uint32_t in_win = 0;

                if (was_dead[proc] || s.proc[proc].phase != LG_SUP_IDLE) {
                    viol++; /* spawn after DEAD, or for a process that is not IDLE */
                }
                if (rnd() % 50 == 0) {
                    lg_sup_on_spawn_failed(&s, (uint8_t)proc, t);
                } else if (lg_sup_on_spawn(&s, (uint8_t)proc, ++g_next_pid, t) != 0) {
                    viol++;
                }
                silent[proc] = 0;
                if (spawn_n[proc] < 64) {
                    spawn_ms[proc][spawn_n[proc]++] = t;
                }
                for (k = 0; k < spawn_n[proc]; k++) {
                    if ((uint32_t)(t - spawn_ms[proc][k]) < cfg.window_ms) {
                        in_win++;
                    }
                }
                if (in_win > (uint32_t)cfg.budget + 1u) {
                    viol++; /* the first spawn plus the budget is the most in one window */
                }
                if (spawn_n[proc] == 64) {
                    memmove(spawn_ms[proc], spawn_ms[proc] + 32, 32 * sizeof(uint32_t));
                    spawn_n[proc] = 32;
                }
            } else if (a[i].kind == LG_SUP_ACT_KILL) {
                kills++;
                if (s.proc[proc].phase != LG_SUP_KILLING || a[i].pid == 0 || a[i].pid != s.proc[proc].pid) {
                    viol++;
                }
                if (rnd() % 4 == 0) {
                    (void)lg_sup_on_exit(&s, a[i].pid, t); /* the kill works */
                }
            }
        }
        for (p = 0; p < LG_PROC_COUNT; p++) {
            if (was_dead[p] && s.proc[p].phase != LG_SUP_DEAD) {
                viol++; /* DEAD must be absorbing */
            }
            if (s.proc[p].phase == LG_SUP_DEAD) {
                was_dead[p] = 1;
            }
            if (lg_sup_restarts_in_window(&s, (uint8_t)p, t) > cfg.budget) {
                viol++;
            }
            if (cfg.installed[p] != 0 && (lg_sup_reported_state(&s, (uint8_t)p) == LG_PROC_RUNNING) !=
                                             (s.proc[p].phase == LG_SUP_RUNNING)) {
                viol++;
            }
        }
    }
    {
        uint32_t spawns = 0;
        uint32_t restarts = 0;
        int dead = 0;
        int p;

        for (p = 0; p < LG_PROC_COUNT; p++) {
            restarts += s.proc[p].total_restarts;
            dead += s.proc[p].phase == LG_SUP_DEAD ? 1 : 0;
            spawns += spawn_n[p];
        }
        printf("supervisor walk seed=%u steps=200000 restarts=%u dead=%d kills=%u violations=%d\n",
               (unsigned)seed, (unsigned)restarts, dead, (unsigned)kills, viol);
        CHECK(restarts > 100); /* the walk really exercises restarts */
        CHECK(dead >= 1 && dead < LG_PROC_COUNT - 4); /* some give up, most go on */
        CHECK(kills > 10);     /* ... and hang kills */
        (void)spawns;
    }
    CHECK(viol == 0);
}

int main(void) {
    test_boot_order();
    test_dead_lower_stage_does_not_block_boot();
    test_exit_restart_and_recovery();
    test_hang_detection();
    test_stall_is_forgiven();
    test_start_grace();
    test_budget();
    test_budget_window_slides();
    test_spawn_failure_counts();
    test_unknown_input();
    test_status_actions();
    test_clock_wrap();
    test_text();
    test_random_walk(20261120u);
    test_random_walk(20261121u);
    LG_TEST_DONE("test_supervisor_logic");
}
