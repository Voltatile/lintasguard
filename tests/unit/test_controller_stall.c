#include <stdio.h>
#include <string.h>

#include "lg_controller_logic.h"
#include "lg_gate_model.h"
#include "lg_test.h"

/*
 * Step 9 campaign: a stall of the controller itself (the whole VM paused) must not be read as
 * silence of the sensors, the gate or the supervisor. When a step comes stall_ms or more after the
 * previous one, the silence clocks are moved forward by the gap; silence before the stall still
 * counts, and a sample that is older than LG_SAMPLE_MAX_AGE_MS is still not usable.
 */

#define W LG_DIR_WEST
#define E LG_DIR_EAST
#define A LG_SENSOR_A
#define B LG_SENSOR_B
#define I LG_SENSOR_ISLAND

static uint8_t g_mute[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
static int g_gate_mute;
static lg_ctl_out_t g_last;
static uint8_t g_gate = LG_GATE_DOWN;

static void tick(lg_ctl_t *c, uint32_t now) {
    int d;
    int i;

    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            if (!g_mute[d][i]) {
                CHECK(lg_ctl_on_sample(c, (uint8_t)d, (uint8_t)i, 0, 97, 0, now, now) == 0);
            }
        }
    }
    if (!g_gate_mute) {
        CHECK(lg_ctl_on_gate_reply(c, g_gate, now) == 0);
    }
    g_last = lg_ctl_step(c, now);
    g_gate = g_last.gate_target == LG_GATE_TARGET_DOWN ? (uint8_t)LG_GATE_DOWN : (uint8_t)LG_GATE_UP;
}

static void run(lg_ctl_t *c, uint32_t *t, uint32_t ms) {
    uint32_t end = *t + ms;

    for (; *t != end; *t += 10) {
        tick(c, *t);
    }
}

static void released(lg_ctl_t *c, uint32_t *t, uint32_t base) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();

    memset(g_mute, 0, sizeof g_mute);
    g_gate_mute = 0;
    g_gate = LG_GATE_DOWN;
    *t = base;
    lg_ctl_init(c, &cfg, *t);
    run(c, t, LG_T_BOOT_STABLE_MS + 500);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
}

/* The whole system pauses for gap_ms: no samples, no gate replies, no steps. */
static void pause_all(uint32_t *t, uint32_t gap_ms) {
    *t += gap_ms;
}

static void test_stall_is_not_silence(uint32_t base) {
    lg_ctl_t c;
    uint32_t t;
    uint32_t gaps[] = {60, 150, 300, 1000};
    size_t k;

    released(&c, &t, base);
    for (k = 0; k < sizeof gaps / sizeof gaps[0]; k++) {
        pause_all(&t, gaps[k]);
        run(&c, &t, 2000); /* everything answers again from the first tick on */
        CHECK(g_last.level == LG_LEVEL_NORMAL);
    }
    CHECK(c.stalls == 4);
    CHECK(c.tickets == 0);
    CHECK(c.events == 1); /* only the boot release */
    CHECK(c.gate_silent == 0);
}

static void test_samples_late_after_the_stall(void) {
    lg_ctl_t c;
    uint32_t t;

    released(&c, &t, 0);
    pause_all(&t, 300);
    /* The controller runs first; the sensors and the gate answer 30 ms later. */
    g_gate_mute = 1;
    memset(g_mute, 1, sizeof g_mute);
    run(&c, &t, 30);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    g_gate_mute = 0;
    memset(g_mute, 0, sizeof g_mute);
    run(&c, &t, 1000);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    CHECK(c.tickets == 0);
}

static void test_a_dead_sensor_is_still_found(void) {
    lg_ctl_t c;
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    uint32_t t;
    uint32_t resume;

    released(&c, &t, 0);
    g_mute[W][A] = 1; /* dies with the stall and never comes back */
    pause_all(&t, 300);
    resume = t;
    /* The stall counts as stall_ms of silence: FAILED sensor_silence_ms - stall_ms after the resume. */
    while (t - resume < cfg.sensor_silence_ms - cfg.stall_ms - 10) {
        tick(&c, t);
        CHECK(g_last.level == LG_LEVEL_NORMAL);
        t += 10;
    }
    run(&c, &t, 30);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    CHECK(c.tickets == 1);
}

static void test_silence_before_the_stall_counts(void) {
    lg_ctl_t c;
    uint32_t t;
    uint32_t resume;

    released(&c, &t, 0);
    g_mute[E][B] = 1;
    run(&c, &t, 100); /* 100 ms silent before the stall */
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    pause_all(&t, 300);
    resume = t;
    run(&c, &t, 40); /* 100 + 50 (the stall) + 30 */
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    run(&c, &t, 30); /* 100 + 50 + 60 > 200 */
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    CHECK(t - resume <= 70);
}

static void test_a_dead_gate_is_still_found(void) {
    lg_ctl_t c;
    uint32_t t;
    uint32_t resume;

    released(&c, &t, 0);
    g_gate_mute = 1;
    pause_all(&t, 300);
    resume = t;
    run(&c, &t, 40); /* 50 (the stall) + 30 < 100 */
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    run(&c, &t, 40);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    CHECK(g_last.gate_target == LG_GATE_TARGET_DOWN);
    CHECK(t - resume <= 80);
}

/*
 * Persistent overload: every step comes 60 ms after the previous one. Each late step still counts as
 * stall_ms of silence, so a dead sensor is FAILED after a few steps (a first version that moved the
 * clocks by the whole gap never found it; test_dead_sensor_stays_dead_across_wrap caught that).
 */
static void test_persistent_overload_still_finds_a_dead_sensor(void) {
    lg_ctl_t c;
    uint32_t t;
    int k;

    released(&c, &t, 0);
    g_mute[W][B] = 1;
    for (k = 0; k < 3; k++) {
        t += 50; /* 60 ms between steps with the 10 ms of the tick below */
        tick(&c, t);
        t += 10;
        CHECK(g_last.level == LG_LEVEL_NORMAL); /* 50, 100, 150 ms of silence */
    }
    for (k = 0; k < 2; k++) {
        t += 50;
        tick(&c, t);
        t += 10;
    }
    CHECK(g_last.level == LG_LEVEL_DEGRADED); /* 250 ms counted after 5 late steps */
    CHECK(c.stalls >= 5);
}

static void test_short_gaps_are_no_stall(void) {
    lg_ctl_t c;
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    uint32_t t;

    released(&c, &t, 0);
    pause_all(&t, cfg.stall_ms - 20); /* with the 10 ms tick: a 40 ms gap between two steps */
    run(&c, &t, 100);
    CHECK(c.stalls == 0);
    /* A sensor that was silent through short gaps is FAILED on time. */
    g_mute[W][B] = 1;
    run(&c, &t, cfg.sensor_silence_ms + 20);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
}

static void test_stall_keeps_the_supervisor_alive(void) {
    lg_ctl_t c;
    uint32_t t;
    int k;

    released(&c, &t, 0);
    for (k = 0; k < 3; k++) { /* the supervisor repeats its table every second */
        CHECK(lg_ctl_on_proc_status(&c, LG_PROC_PLANT, LG_PROC_RUNNING, 0, t) == 0);
        run(&c, &t, 1000);
    }
    /* 2 s of running controller since the last report (1 s in the loop, 1 s here), a stall of 2 s,
     * then 1 s more: 3 s of controller time (below 3.5 s), 5 s of wall time (above). */
    run(&c, &t, 1000);
    pause_all(&t, 2000); /* the supervisor's 1 s refresh fell into the pause */
    for (k = 0; k < 3; k++) {
        run(&c, &t, 1000);
        CHECK(lg_ctl_on_proc_status(&c, LG_PROC_PLANT, LG_PROC_RUNNING, 0, t) == 0);
    }
    CHECK(c.sup_silent == 0);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
}

/*
 * Review finding (Step 9): the stall must not count as observed health. Level 4 (gate silent), a guard
 * acknowledges, 1 s healthy, then a 29.5 s stall: the first step after it, before any new input,
 * left level 4 and commanded the gate UP. T_stable must be 30 s of running controller.
 */
static void test_stall_is_not_stable_time(void) {
    lg_ctl_t c;
    uint32_t t;
    uint32_t resume;

    released(&c, &t, 0);
    g_gate_mute = 1;
    run(&c, &t, 300);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    g_gate_mute = 0;
    run(&c, &t, 500);
    lg_ctl_on_guard_ack(&c);
    run(&c, &t, 1000);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    pause_all(&t, 29500);
    resume = t;
    g_gate_mute = 1; /* the first step runs before anything arrives */
    memset(g_mute, 1, sizeof g_mute);
    tick(&c, t);
    t += 10;
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    CHECK(g_last.gate_target == LG_GATE_TARGET_DOWN);
    g_gate_mute = 0;
    memset(g_mute, 0, sizeof g_mute);
    run(&c, &t, 20000);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE); /* about 21 s of observed health, not 30 */
    run(&c, &t, 10000);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    CHECK(t - resume >= 28000);
}

/*
 * Review finding: a reply or sample that arrives after the resume but before the first step must not
 * be pushed into the future. Everyone answers once, then the gate and west/a die: both are found
 * within their normal limits after that answer (100 ms gate, 200 ms sensor), not a gap later.
 */
static void test_answers_at_the_resume_are_not_moved(void) {
    lg_ctl_t c;
    uint32_t t;
    uint32_t resume;

    released(&c, &t, 0);
    pause_all(&t, 1000);
    resume = t;
    tick(&c, t); /* samples and the gate reply arrive first, then the step */
    t += 10;
    g_gate_mute = 1;
    g_mute[W][A] = 1;
    while (g_last.level != LG_LEVEL_FAILSAFE && t - resume < 2000) {
        tick(&c, t);
        t += 10;
    }
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    CHECK(t - resume <= 130);
    g_gate_mute = 0;
    run(&c, &t, 300);
    CHECK(c.last_verdict[W][A] == LG_VERDICT_FAILED);
}

/* An old sample stays stale after a stall: its timestamp is never moved (mutant M1 of the review). */
static void test_old_sample_stays_unusable(void) {
    lg_ctl_t c;
    uint32_t t;
    uint32_t act;

    released(&c, &t, 0);
    act = c.activations;
    /* West A reports occupied just before the stall; the controller never stepped on it. */
    CHECK(lg_ctl_on_sample(&c, W, A, 1, 97, 0, t - 5, t - 5) == 0);
    pause_all(&t, 300);
    memset(g_mute, 1, sizeof g_mute); /* nothing new arrives on the first step */
    g_gate_mute = 1;
    g_last = lg_ctl_step(&c, t);
    CHECK(c.activations == act); /* a 300 ms old sample is not an approaching train */
    memset(g_mute, 0, sizeof g_mute);
    g_gate_mute = 0;
}

/* The boot grace of a gate never heard is not used up by a stall (mutant M2). */
static void test_stall_in_the_gate_boot_grace(void) {
    lg_ctl_t c;
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    uint32_t t = 0;

    memset(g_mute, 0, sizeof g_mute);
    g_gate_mute = 1;
    lg_ctl_init(&c, &cfg, t);
    run(&c, &t, 1000);
    pause_all(&t, 1500);
    tick(&c, t);
    t += 10;
    CHECK(c.gate_silent == 0); /* 1000 + 50 + 10 ms of the 2000 ms grace */
    run(&c, &t, cfg.gate_boot_grace_ms);
    CHECK(c.gate_silent == 1); /* a gate that never answers is still found */
    g_gate_mute = 0;
}

/* Unusable time before a stall still counts (mutant M3): blind 100 ms, stall, FAILED 50 ms after. */
static void test_blind_time_before_the_stall_counts(void) {
    lg_ctl_t c;
    uint32_t t;
    uint32_t resume;

    released(&c, &t, 0);
    {
        uint32_t end = t + 100;

        for (; t != end; t += 10) {
            int d;
            int i;

            for (d = 0; d < LG_DIR_COUNT; d++) {
                for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                    uint8_t q = (d == E && i == A) ? 0 : 97;
                    CHECK(lg_ctl_on_sample(&c, (uint8_t)d, (uint8_t)i, 0, q, 0, t, t) == 0);
                }
            }
            CHECK(lg_ctl_on_gate_reply(&c, g_gate, t) == 0);
            g_last = lg_ctl_step(&c, t);
        }
    }
    pause_all(&t, 300);
    resume = t;
    while (c.last_verdict[E][A] != LG_VERDICT_FAILED && t - resume < 1000) {
        int d;
        int i;

        for (d = 0; d < LG_DIR_COUNT; d++) {
            for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                uint8_t q = (d == E && i == A) ? 0 : 97;
                CHECK(lg_ctl_on_sample(&c, (uint8_t)d, (uint8_t)i, 0, q, 0, t, t) == 0);
            }
        }
        CHECK(lg_ctl_on_gate_reply(&c, g_gate, t) == 0);
        g_last = lg_ctl_step(&c, t);
        t += 10;
    }
    CHECK(c.last_verdict[E][A] == LG_VERDICT_FAILED);
    CHECK(t - resume >= 40 && t - resume <= 70); /* 100 + 50 + about 50 */
}

/*
 * Review finding: a sensor that alternates stale and blind samples has no usable sample at all and
 * must be FAILED after sensor_silence_ms; two clocks that reset each other kept it OK for good.
 */
static void test_stale_and_blind_alternating_is_failed(void) {
    lg_ctl_t c;
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    uint32_t t;
    uint32_t start;
    int k = 0;

    released(&c, &t, 0);
    start = t;
    while (t - start <= cfg.sensor_silence_ms + 20) {
        int d;
        int i;

        for (d = 0; d < LG_DIR_COUNT; d++) {
            for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                if (d == W && i == B) {
                    /* even ticks: stale with good quality; odd ticks: fresh but blind */
                    if ((k & 1) == 0) {
                        CHECK(lg_ctl_on_sample(&c, W, B, 1, 97, 0, t - 30, t) == 0);
                    } else {
                        CHECK(lg_ctl_on_sample(&c, W, B, 1, 0, 0, t, t) == 0);
                    }
                } else {
                    CHECK(lg_ctl_on_sample(&c, (uint8_t)d, (uint8_t)i, 0, 97, 0, t, t) == 0);
                }
            }
        }
        CHECK(lg_ctl_on_gate_reply(&c, g_gate, t) == 0);
        g_last = lg_ctl_step(&c, t);
        t += 10;
        k++;
    }
    CHECK(c.last_verdict[W][B] == LG_VERDICT_FAILED);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
}

static void test_status_shows_stalls(void) {
    lg_ctl_t c;
    uint32_t t;
    char buf[1024];

    released(&c, &t, 0);
    pause_all(&t, 200);
    run(&c, &t, 50);
    (void)lg_ctl_text(&c, buf, sizeof buf);
    CHECK(strstr(buf, " stalls=1") != NULL);
    CHECK(buf[strlen(buf) - 1] == '\n');
}

int main(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();

    CHECK(cfg.stall_ms == LG_CTL_STALL_MS);
    test_stall_is_not_silence(0);
    test_stall_is_not_silence(0xFFFFFFFFu - 6000u); /* the stalls fall across the 32-bit wrap */
    test_samples_late_after_the_stall();
    test_a_dead_sensor_is_still_found();
    test_silence_before_the_stall_counts();
    test_a_dead_gate_is_still_found();
    test_persistent_overload_still_finds_a_dead_sensor();
    test_short_gaps_are_no_stall();
    test_stall_keeps_the_supervisor_alive();
    test_stall_is_not_stable_time();
    test_answers_at_the_resume_are_not_moved();
    test_old_sample_stays_unusable();
    test_stall_in_the_gate_boot_grace();
    test_blind_time_before_the_stall_counts();
    test_stale_and_blind_alternating_is_failed();
    test_status_shows_stalls();
    LG_TEST_DONE("test_controller_stall");
}
