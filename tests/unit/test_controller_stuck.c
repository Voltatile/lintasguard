#include <stdio.h>
#include <string.h>

#include "lg_controller_logic.h"
#include "lg_gate_model.h"
#include "lg_test.h"

/*
 * Step 9: interim stuck rules for the approach sensors (MISSED_TRAIN, STUCK_OCCUPIED), isolation,
 * level 2 and the way back to level 0. The gate is ideal here: it reports the last target at once.
 * Wrong side is checked on every tick: island occupied while the gate is not DOWN.
 */

#define W LG_DIR_WEST
#define E LG_DIR_EAST
#define A LG_SENSOR_A
#define B LG_SENSOR_B
#define I LG_SENSOR_ISLAND

static uint8_t g_occ[LG_DIR_COUNT][LG_SENSOR_ID_COUNT]; /* what each sensor reports */
static uint8_t g_mute[LG_DIR_COUNT][LG_SENSOR_ID_COUNT]; /* 1 = sends nothing */
static uint16_t g_ax[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];  /* cumulative axle counts (ground truth) */
static uint8_t g_gate = LG_GATE_DOWN;
static lg_ctl_out_t g_last;
static int g_wrong_side;
static int g_max_level;
static int g_gate_down_ticks;

static void tick(lg_ctl_t *c, uint32_t now) {
    int d;
    int i;

    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            if (!g_mute[d][i]) {
                CHECK(lg_ctl_on_sample(c, (uint8_t)d, (uint8_t)i, g_occ[d][i], 97, g_ax[d][i], now, now) == 0);
            }
        }
    }
    CHECK(lg_ctl_on_gate_reply(c, g_gate, now) == 0);
    g_last = lg_ctl_step(c, now);
    if ((g_occ[W][I] || g_occ[E][I]) && g_gate != LG_GATE_DOWN) {
        g_wrong_side++;
    }
    g_gate = g_last.gate_target == LG_GATE_TARGET_DOWN ? (uint8_t)LG_GATE_DOWN : (uint8_t)LG_GATE_UP;
    if (g_gate == LG_GATE_DOWN) {
        g_gate_down_ticks++;
    }
    if (g_last.level > g_max_level) {
        g_max_level = g_last.level;
    }
}

static void run(lg_ctl_t *c, uint32_t *t, uint32_t ms) {
    uint32_t end = *t + ms;

    for (; *t != end; *t += 10) {
        tick(c, *t);
    }
}

static void released(lg_ctl_t *c, uint32_t *t) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();

    memset(g_occ, 0, sizeof g_occ);
    memset(g_mute, 0, sizeof g_mute);
    memset(g_ax, 0, sizeof g_ax);
    g_gate = LG_GATE_DOWN;
    g_wrong_side = 0;
    lg_ctl_init(c, &cfg, *t);
    run(c, t, LG_T_BOOT_STABLE_MS + 500);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    CHECK(g_gate == LG_GATE_UP);
    g_max_level = 0;
    g_gate_down_ticks = 0;
}

/*
 * One train in direction d: the approach sensors report [a_on, a_off) and [b_on, b_off) ms after the
 * start (a sensor with on = -1 keeps what it reports), the island [12000, 16000), then 6 s of
 * nothing. Axle counts follow the train as in the simulator, also for a sensor that reports a stuck
 * occupancy: 8 axles in at 2000 ms, out at 16000 ms. Returns with *t at the end.
 */
static void train(lg_ctl_t *c, uint32_t *t, int d, int a_on, int a_off, int b_on, int b_off) {
    uint32_t t0 = *t;
    uint32_t end = t0 + 22000;

    for (; *t != end; *t += 10) {
        int rel = (int)(*t - t0);

        if (a_on >= 0) {
            g_occ[d][A] = (uint8_t)(rel >= a_on && rel < a_off);
        }
        if (b_on >= 0) {
            g_occ[d][B] = (uint8_t)(rel >= b_on && rel < b_off);
        }
        g_occ[d][I] = (uint8_t)(rel >= 12000 && rel < 16000);
        if (rel == 2000) {
            g_ax[d][A] = (uint16_t)(g_ax[d][A] + 8);
            g_ax[d][B] = (uint16_t)(g_ax[d][B] + 8);
        }
        if (rel == 16000) {
            g_ax[d][I] = (uint16_t)(g_ax[d][I] + 8);
        }
        tick(c, *t);
    }
}

static void test_healthy_pair_with_small_offsets_is_not_stuck(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    train(&c, &t, W, 0, 5000, 300, 5300); /* B 300 ms behind: inside t_disagree */
    train(&c, &t, E, 400, 5000, 0, 4600);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE);
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_NONE);
    CHECK(lg_ctl_sensor_diag(&c, E, A) == LG_CTL_DIAG_NONE);
    CHECK(lg_ctl_sensor_diag(&c, E, B) == LG_CTL_DIAG_NONE);
    CHECK(g_max_level == LG_LEVEL_NORMAL);
    CHECK(c.cycles == 2);
    CHECK(g_wrong_side == 0);
}

static void test_delayed_partner_overlapping_is_not_stuck(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    train(&c, &t, W, 0, 5000, 1000, 6000); /* the delay fault: 1000 ms late, runs still overlap */
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE);
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_NONE);
    CHECK(g_max_level == LG_LEVEL_NORMAL);
    CHECK(g_wrong_side == 0);
}

static void test_stuck_clear_is_missed_train_then_recovers(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t t_ok;

    released(&c, &t);
    /* West A stuck clear: B alone sees the train; the 1oo2 vote still protects it. */
    train(&c, &t, W, 0, 0, 0, 5000);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_MISSED_TRAIN);
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_NONE);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    CHECK(c.cycles == 1);
    CHECK(g_wrong_side == 0);
    /* Still latched while nothing passes. */
    run(&c, &t, 40000);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_MISSED_TRAIN);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    /* Fault gone: the next train is seen by both, which clears the latch; level 0 after T_stable. */
    train(&c, &t, W, 0, 5000, 100, 5100);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE);
    CHECK(g_last.level == LG_LEVEL_DEGRADED); /* T_stable has not passed yet */
    /* The latch cleared about 5.6 s into the 22 s train: T_stable (30 s) ends about 13.6 s later. */
    t_ok = t;
    run(&c, &t, 12000);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    run(&c, &t, 3000);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    CHECK(t - t_ok == 15000);
    CHECK(c.cycles == 2);
    CHECK(g_wrong_side == 0);
}

static void test_stuck_clear_on_east_b(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    train(&c, &t, E, 0, 4000, 0, 0);
    CHECK(lg_ctl_sensor_diag(&c, E, B) == LG_CTL_DIAG_MISSED_TRAIN);
    CHECK(lg_ctl_sensor_diag(&c, E, A) == LG_CTL_DIAG_NONE);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    CHECK(g_wrong_side == 0);
}

static void test_stuck_occupied_is_isolated_then_recovers(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t t0;
    int down_before;

    released(&c, &t);
    /* West A stuck occupied, no train: the direction is active (safe side) until it is judged. */
    g_occ[W][A] = 1;
    t0 = t;
    run(&c, &t, LG_SENSOR_MAX_OCCUPIED_MS - 1000);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE);
    CHECK(g_gate == LG_GATE_DOWN);
    CHECK(c.active[W] == 1);
    run(&c, &t, 2000);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_STUCK_OCCUPIED);
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_NONE); /* B is not blamed */
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    /* Isolated: the direction clears and the road opens again although A still says occupied. */
    run(&c, &t, 10000);
    CHECK(c.active[W] == 0);
    CHECK(g_gate == LG_GATE_UP);
    CHECK(t - t0 > LG_SENSOR_MAX_OCCUPIED_MS);
    /* A train while A is isolated: B protects it. */
    train(&c, &t, W, -1, 0, 0, 5000);
    CHECK(c.cycles == 2);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_STUCK_OCCUPIED);
    /* Fault cleared: A's long run ends, that is no train and judges nobody. */
    g_occ[W][A] = 0;
    run(&c, &t, 2000);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_STUCK_OCCUPIED);
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_NONE);
    /* An agreeing train releases the latch, T_stable later level 0. */
    down_before = g_gate_down_ticks;
    train(&c, &t, W, 0, 5000, 0, 5000);
    CHECK(g_gate_down_ticks > down_before);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE);
    run(&c, &t, LG_T_STABLE_MS + 1000);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    CHECK(g_max_level == LG_LEVEL_DEGRADED);
    CHECK(g_wrong_side == 0);
}

static void test_both_occupied_long_blames_nobody(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    g_occ[W][A] = 1;
    g_occ[W][B] = 1;
    run(&c, &t, LG_SENSOR_MAX_OCCUPIED_MS * 2);
    /* Both say a train is there: nothing to judge against, the gate stays down. */
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE);
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_NONE);
    CHECK(g_gate == LG_GATE_DOWN);
    CHECK(g_wrong_side == 0);
}

/*
 * Step 9 review: a silence (a VM stall, a stale stretch) does not prove that the fault is gone, so it
 * keeps the latch. Before, a 200 ms silence cleared it and the system reported level 0 with the stuck
 * sensor back in the vote until the next train judged it again.
 */
static void test_silence_keeps_the_latch(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    train(&c, &t, W, 0, 0, 0, 5000); /* A stuck clear: MISSED_TRAIN */
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_MISSED_TRAIN);
    g_mute[W][A] = 1;
    run(&c, &t, 1000);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_MISSED_TRAIN);
    g_mute[W][A] = 0;
    run(&c, &t, LG_T_STABLE_MS + 1000);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_MISSED_TRAIN);
    CHECK(g_last.level == LG_LEVEL_DEGRADED); /* still isolated, still level 2 */
    CHECK(c.tickets == 1);                    /* the ticket stays open, no second one */
    CHECK(c.ticket_open[W][A] == 1);
    CHECK(g_wrong_side == 0);
}

/* A new process (the supervisor reports a restart) starts without the old fault: the latch goes. */
static void test_supervisor_restart_clears_the_latch(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint8_t west_a = (uint8_t)(LG_PROC_SENSOR_BASE + W * LG_SENSOR_ID_COUNT + A);

    released(&c, &t);
    train(&c, &t, W, 0, 0, 0, 5000);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_MISSED_TRAIN);
    CHECK(lg_ctl_on_proc_status(&c, west_a, LG_PROC_RESTARTING, 1, t) == 0);
    run(&c, &t, 20);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE);
    CHECK(g_last.level == LG_LEVEL_DEGRADED); /* restarting = level 2 */
    /* RUNNING again; the supervisor repeats its table every second, as the real one does (a silent
     * supervisor counts as dead after LG_CTL_SUP_SILENCE_MS). */
    {
        int k;

        for (k = 0; k < (LG_T_STABLE_MS + 1000) / 1000; k++) {
            CHECK(lg_ctl_on_proc_status(&c, west_a, LG_PROC_RUNNING, 1, t) == 0);
            run(&c, &t, 1000);
        }
    }
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    /* A RUNNING report alone (the periodic refresh) clears nothing. */
    CHECK(lg_ctl_on_proc_status(&c, west_a, LG_PROC_RUNNING, 1, t) == 0);
    train(&c, &t, W, 0, 0, 0, 5000);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_MISSED_TRAIN);
    CHECK(lg_ctl_on_proc_status(&c, west_a, LG_PROC_RUNNING, 1, t) == 0);
    run(&c, &t, 20);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_MISSED_TRAIN);
    CHECK(g_wrong_side == 0);
}

static void test_no_judgement_against_a_dead_partner(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    g_mute[W][B] = 1; /* B silent: FAILED, level 2 */
    train(&c, &t, W, 0, 5000, -1, 0);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE);
    g_occ[W][A] = 1;
    run(&c, &t, LG_SENSOR_MAX_OCCUPIED_MS + 5000);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE); /* no healthy partner to compare with */
    CHECK(g_gate == LG_GATE_DOWN);
    CHECK(g_wrong_side == 0);
}

static void test_blips_blame_nobody(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    /* One tick, then a run just below t_disagree, on A only: flicker, not a train. */
    g_occ[W][A] = 1;
    run(&c, &t, 10);
    g_occ[W][A] = 0;
    run(&c, &t, 3000);
    g_occ[W][A] = 1;
    run(&c, &t, LG_T_DISAGREE_MS - 10);
    g_occ[W][A] = 0;
    run(&c, &t, 3000);
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_NONE);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE);
    /* A run of exactly t_disagree counts as a train that B missed. */
    g_occ[W][A] = 1;
    run(&c, &t, LG_T_DISAGREE_MS);
    g_occ[W][A] = 0;
    run(&c, &t, 3000);
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_MISSED_TRAIN);
    CHECK(g_wrong_side == 0);
}

static void test_diag_arguments(void) {
    lg_ctl_t c;
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();

    lg_ctl_init(&c, &cfg, 0);
    CHECK(lg_ctl_sensor_diag(&c, W, I) == LG_CTL_DIAG_NONE);
    CHECK(lg_ctl_sensor_diag(&c, 2, A) == LG_CTL_DIAG_NONE);
    CHECK(lg_ctl_sensor_diag(&c, W, 7) == LG_CTL_DIAG_NONE);
    CHECK(cfg.max_occupied_ms == LG_SENSOR_MAX_OCCUPIED_MS);
    CHECK(cfg.t_disagree_ms == LG_T_DISAGREE_MS);
}

int main(void) {
    test_healthy_pair_with_small_offsets_is_not_stuck();
    test_delayed_partner_overlapping_is_not_stuck();
    test_stuck_clear_is_missed_train_then_recovers();
    test_stuck_clear_on_east_b();
    test_stuck_occupied_is_isolated_then_recovers();
    test_both_occupied_long_blames_nobody();
    test_silence_keeps_the_latch();
    test_supervisor_restart_clears_the_latch();
    test_no_judgement_against_a_dead_partner();
    test_blips_blame_nobody();
    test_diag_arguments();
    LG_TEST_DONE("test_controller_stuck");
}
