#include <string.h>

#include "lg_controller_logic.h"
#include "lg_gate_model.h"
#include "lg_test.h"

static uint8_t g_west_a_occupied; /* what west/a reports in tick() */

/* Feed a healthy, clear sample from all six sensors and a gate reply, then step once. */
static lg_ctl_out_t tick(lg_ctl_t *c, uint32_t now, uint8_t gate_state, int feed_samples) {
    int d;
    int i;

    if (feed_samples) {
        for (d = 0; d < LG_DIR_COUNT; d++) {
            for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                uint8_t occ = (uint8_t)(d == LG_DIR_WEST && i == LG_SENSOR_A && g_west_a_occupied);
                CHECK(lg_ctl_on_sample(c, (uint8_t)d, (uint8_t)i, occ, 97, 0, now, now) == 0);
            }
        }
    }
    if (gate_state <= LG_GATE_FAULT) {
        CHECK(lg_ctl_on_gate_reply(c, gate_state, now) == 0);
    }
    return lg_ctl_step(c, now);
}

static void test_boot_holds_level4(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t;

    lg_ctl_init(&c, &cfg, 1000);
    for (t = 1010; t < 1000 + LG_T_BOOT_STABLE_MS - 20; t += 10) {
        o = tick(&c, t, LG_GATE_DOWN, 1);
        CHECK(o.level == LG_LEVEL_FAILSAFE);
        CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights && o.bell);
    }
    /* After t_boot_stable of all healthy the lock is released and the gate may rise. */
    for (; t < 1000 + LG_T_BOOT_STABLE_MS + 100; t += 10) {
        o = tick(&c, t, LG_GATE_DOWN, 1);
    }
    CHECK(o.level == LG_LEVEL_NORMAL);
    CHECK(o.gate_target == LG_GATE_TARGET_UP);
}

static void test_nothing_without_samples(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t;

    lg_ctl_init(&c, &cfg, 0);
    for (t = 10; t < 20000; t += 10) {
        o = tick(&c, t, LG_GATE_DOWN, 0); /* the gate answers, no sensor ever does */
    }
    CHECK(o.level == LG_LEVEL_FAILSAFE); /* never released without information */
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN);
}

static void release_boot(lg_ctl_t *c, uint32_t *t) {
    lg_ctl_out_t o;

    for (; *t < 6000; *t += 10) {
        o = tick(c, *t, LG_GATE_DOWN, 1);
    }
    CHECK(o.level == LG_LEVEL_NORMAL);
}

static void test_silent_gate_is_level4(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t = 10;
    uint32_t end;

    lg_ctl_init(&c, &cfg, 0);
    release_boot(&c, &t);
    /* Gate stops answering: after gate_silence_ms level 4, gate target DOWN, lights on. */
    end = t + cfg.gate_silence_ms + 50;
    for (; t < end; t += 10) {
        o = tick(&c, t, 255, 1);
    }
    CHECK(o.level == LG_LEVEL_FAILSAFE);
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights);
    /* A gate that answers again does not release level 4 by itself. */
    for (end = t + 60000; t < end; t += 10) {
        o = tick(&c, t, LG_GATE_UP, 1);
    }
    CHECK(o.level == LG_LEVEL_FAILSAFE);
}

static void test_one_stale_tick_changes_nothing(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t = 10;

    lg_ctl_init(&c, &cfg, 0);
    release_boot(&c, &t);
    /* 40 ms without any sample (a VM stall): samples are stale, verdicts stay OK, no activation. */
    for (; t < 6050; t += 10) {
        o = tick(&c, t, LG_GATE_UP, 0);
        CHECK(o.level == LG_LEVEL_NORMAL);
        CHECK(o.gate_target == LG_GATE_TARGET_UP);
    }
    CHECK(c.activations == 0);
}

static void test_dead_sensor_degrades(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t = 10;
    uint32_t end;

    lg_ctl_init(&c, &cfg, 0);
    release_boot(&c, &t);
    /* west/a goes silent for longer than sensor_silence_ms: the level rises above 0. */
    end = t + cfg.sensor_silence_ms + 100;
    for (; t < end; t += 10) {
        int d;
        int i;

        for (d = 0; d < LG_DIR_COUNT; d++) {
            for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                if (!(d == LG_DIR_WEST && i == LG_SENSOR_A)) {
                    CHECK(lg_ctl_on_sample(&c, (uint8_t)d, (uint8_t)i, 0, 97, 0, t, t) == 0);
                }
            }
        }
        (void)lg_ctl_on_gate_reply(&c, LG_GATE_UP, t);
        o = lg_ctl_step(&c, t);
    }
    CHECK(o.level >= LG_LEVEL_DEGRADED);
}

static void test_blind_sensor_is_failed(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t = 10;
    int d;
    int i;

    lg_ctl_init(&c, &cfg, 0);
    release_boot(&c, &t);
    /* quality 0 (no truth, fully noisy) is FAILED at once, not a clear track. */
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            uint8_t q = (d == LG_DIR_EAST && i == LG_SENSOR_B) ? 0 : 97;
            CHECK(lg_ctl_on_sample(&c, (uint8_t)d, (uint8_t)i, 0, q, 0, t, t) == 0);
        }
    }
    (void)lg_ctl_on_gate_reply(&c, LG_GATE_UP, t);
    o = lg_ctl_step(&c, t);
    CHECK(o.level >= LG_LEVEL_DEGRADED);
}

/* West approach A reports a train: lights at once, the gate follows after the pre-warning delay. */
static void test_activation_and_delay(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t = 10;
    uint32_t t0;

    lg_ctl_init(&c, &cfg, 0);
    release_boot(&c, &t);
    t0 = t;
    /* West approach A reports occupied: activation, lights on at once, gate still UP. */
    g_west_a_occupied = 1;
    o = tick(&c, t, LG_GATE_UP, 1);
    CHECK(o.activated_mask == 1u << LG_DIR_WEST);
    CHECK(o.lights && o.bell);
    CHECK(o.gate_target == LG_GATE_TARGET_UP); /* the pre-warning delay has not passed */

    /* Until the delay has passed the gate stays up, then it is commanded down. */
    for (t += 10; t < t0 + cfg.vote.prewarning_ms - 20; t += 10) {
        o = tick(&c, t, LG_GATE_UP, 1);
        CHECK(o.gate_target == LG_GATE_TARGET_UP);
        CHECK(o.lights);
    }
    for (; t < t0 + cfg.vote.prewarning_ms + 40; t += 10) {
        o = tick(&c, t, LG_GATE_UP, 1);
    }
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN);
    CHECK(c.activations == 1);
    g_west_a_occupied = 0;
}

/* Healthy samples from all sensors with chosen axle counters and island occupancy (west only). */
static void feed(lg_ctl_t *c, uint32_t t, uint16_t west_in, uint16_t west_out, uint8_t west_island) {
    int d;
    int i;

    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            uint16_t ax = 0;
            uint8_t occ = 0;

            if (d == LG_DIR_WEST) {
                ax = i == LG_SENSOR_ISLAND ? west_out : west_in;
                occ = (uint8_t)(i == LG_SENSOR_ISLAND && west_island);
            }
            CHECK(lg_ctl_on_sample(c, (uint8_t)d, (uint8_t)i, occ, 97, ax, t, t) == 0);
        }
    }
}

/*
 * A controller that restarts while a train is between the approach zone and the island has no
 * approach sensor to see it: the axle counts (in 8, out 0) say it is on its way. Found by the
 * reviewer: without this the restarted controller released the boot lock, raised the gate and the
 * train reached the island with the gate not down.
 */
static void test_restart_with_train_in_transit(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t;

    lg_ctl_init(&c, &cfg, 1000);
    for (t = 1010; t < 9000; t += 10) { /* boot check passes, the train is somewhere in between */
        feed(&c, t, 8, 0, 0);
        (void)lg_ctl_on_gate_reply(&c, LG_GATE_DOWN, t);
        o = lg_ctl_step(&c, t);
        CHECK(o.gate_target == LG_GATE_TARGET_DOWN);
    }
    CHECK(o.level == LG_LEVEL_NORMAL); /* the boot lock was released ... */
    CHECK(c.active[LG_DIR_WEST] == 1); /* ... but the train is accounted for */
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights);
    for (; t < 12000; t += 10) { /* the train is on the island */
        feed(&c, t, 8, 0, 1);
        (void)lg_ctl_on_gate_reply(&c, LG_GATE_DOWN, t);
        o = lg_ctl_step(&c, t);
        CHECK(o.gate_target == LG_GATE_TARGET_DOWN);
    }
    for (; t < 14100; t += 10) { /* it left: counts equal, island clear, but not for t_clear yet */
        feed(&c, t, 8, 8, 0);
        (void)lg_ctl_on_gate_reply(&c, LG_GATE_DOWN, t);
        o = lg_ctl_step(&c, t);
    }
    CHECK(o.gate_target == LG_GATE_TARGET_UP); /* released only now */
    CHECK(c.active[LG_DIR_WEST] == 0 && c.cycles == 1);
}

/* The counters wrap modulo 65536: in 3 after the wrap against out 65531 still means 8 axles apart. */
static void test_axle_counts_wrap(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t = 10;

    lg_ctl_init(&c, &cfg, 0);
    release_boot(&c, &t);
    feed(&c, t, 3, 65531, 0);
    (void)lg_ctl_on_gate_reply(&c, LG_GATE_UP, t);
    o = lg_ctl_step(&c, t);
    CHECK(c.active[LG_DIR_WEST] == 1);
    CHECK(o.activated_mask == 1u << LG_DIR_WEST);
}

/* A gate that answers late at boot (slow start) must not remove the boot lock for good. */
static void test_late_gate_keeps_boot_lock(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t;

    lg_ctl_init(&c, &cfg, 0);
    for (t = 10; t < 20000; t += 10) {
        int d;
        int i;

        for (d = 0; d < LG_DIR_COUNT; d++) {
            for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                CHECK(lg_ctl_on_sample(&c, (uint8_t)d, (uint8_t)i, 0, 97, 0, t, t) == 0);
            }
        }
        if (t >= 1500) { /* first answer 1.5 s after the first step: inside the grace period */
            (void)lg_ctl_on_gate_reply(&c, LG_GATE_DOWN, t);
        }
        o = lg_ctl_step(&c, t);
    }
    CHECK(o.level == LG_LEVEL_NORMAL);
    CHECK(c.mode.boot_lock == 0);
}

/* A gate that never answers is a hard event after the grace period: level 4, boot lock gone. */
static void test_gate_never_heard(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t;
    int d;
    int i;

    lg_ctl_init(&c, &cfg, 0);
    for (t = 10; t < 20000; t += 10) {
        for (d = 0; d < LG_DIR_COUNT; d++) {
            for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                CHECK(lg_ctl_on_sample(&c, (uint8_t)d, (uint8_t)i, 0, 97, 0, t, t) == 0);
            }
        }
        o = lg_ctl_step(&c, t);
    }
    CHECK(o.level == LG_LEVEL_FAILSAFE);
    CHECK(c.mode.boot_lock == 0);
    CHECK(c.gate_silent == 1);
}

/* A sensor that has been dead for more than 2^31 ms must not look alive again (32-bit wrap). */
static void test_dead_sensor_stays_dead_across_wrap(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t;
    uint32_t end;
    int d;
    int i;

    lg_ctl_init(&c, &cfg, 0);
    for (t = 10; t <= 10000; t += 10) {
        feed(&c, t, 0, 0, 0);
        (void)lg_ctl_on_gate_reply(&c, LG_GATE_UP, t);
        o = lg_ctl_step(&c, t);
    }
    CHECK(o.level == LG_LEVEL_NORMAL);
    end = 10000u + 2147483648u + 60000u;
    for (t = 10000; t != end; t += 1000) { /* west/a is silent from here on, 1 s steps */
        for (d = 0; d < LG_DIR_COUNT; d++) {
            for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                if (!(d == LG_DIR_WEST && i == LG_SENSOR_A)) {
                    CHECK(lg_ctl_on_sample(&c, (uint8_t)d, (uint8_t)i, 0, 97, 0, t, t) == 0);
                }
            }
        }
        (void)lg_ctl_on_gate_reply(&c, LG_GATE_UP, t);
        o = lg_ctl_step(&c, t);
    }
    CHECK(o.level >= LG_LEVEL_DEGRADED);
}

static void test_rejects_bad_input(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    char buf[16];
    char big[512];
    size_t n;

    lg_ctl_init(&c, &cfg, 0);
    CHECK(lg_ctl_on_sample(&c, LG_DIR_COUNT, 0, 0, 90, 0, 0, 0) == -1);
    CHECK(lg_ctl_on_sample(&c, 0, LG_SENSOR_ID_COUNT, 0, 90, 0, 0, 0) == -1);
    CHECK(lg_ctl_on_sample(&c, 0, 0, 2, 90, 0, 0, 0) == -1);
    CHECK(lg_ctl_on_sample(&c, 0, 0, 0, 101, 0, 0, 0) == -1);
    CHECK(c.samples_rejected == 4 && c.samples == 0);
    CHECK(lg_ctl_on_gate_reply(&c, LG_GATE_FAULT + 1, 0) == -1);
    CHECK(c.gate_replies == 0);

    /* The status text is always NUL terminated, also when the buffer is too small. */
    n = lg_ctl_text(&c, buf, sizeof buf);
    CHECK(n == sizeof buf - 1 && buf[sizeof buf - 1] == '\0');
    n = lg_ctl_text(&c, big, sizeof big);
    CHECK(n > 0 && big[n - 1] == '\n' && strstr(big, "level=4") != NULL);
    CHECK(lg_ctl_text(&c, NULL, 0) == 0);
}

int main(void) {
    test_boot_holds_level4();
    test_nothing_without_samples();
    test_silent_gate_is_level4();
    test_one_stale_tick_changes_nothing();
    test_dead_sensor_degrades();
    test_blind_sensor_is_failed();
    test_activation_and_delay();
    test_restart_with_train_in_transit();
    test_axle_counts_wrap();
    test_late_gate_keeps_boot_lock();
    test_gate_never_heard();
    test_dead_sensor_stays_dead_across_wrap();
    test_rejects_bad_input();
    LG_TEST_DONE("test_controller_logic");
}
