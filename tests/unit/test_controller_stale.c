/*
 * Samples that arrive every tick but carry a header time older than the usable age (a long IPC
 * delay) must not keep the sensors "OK" while they are never usable: the verdict becomes FAILED
 * after sensor_silence_ms in a row and the level rises (to 4: every sensor is affected and there is
 * no schedule). Two cases: from the boot level (the lock is not released) and from level 0 after a
 * normal start (the level rises). Found by the tester agent in the Step 7 review.
 */
#include <stdio.h>

#include "lg_controller_logic.h"
#include "lg_test.h"
#include "lg_time.h"

#define STALE_AGE_MS 50 /* above LG_SAMPLE_MAX_AGE_MS, below sensor_silence_ms */

/* One tick: every sensor sends a sample that is age_ms old, west/a and west/b see a train if set. */
static lg_ctl_out_t tick(lg_ctl_t *c, uint32_t t, uint32_t age_ms, int train) {
    int d;
    int i;

    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            uint8_t occ = (uint8_t)(train && d == LG_DIR_WEST && i != LG_SENSOR_ISLAND);

            (void)lg_ctl_on_sample(c, (uint8_t)d, (uint8_t)i, occ, 97, 0, t - age_ms, t);
        }
    }
    (void)lg_ctl_on_gate_reply(c, t > 5000 ? LG_GATE_UP : LG_GATE_DOWN, t);
    return lg_ctl_step(c, t);
}

/* From the very start every sample is stale: the boot lock is never released, no train is detected. */
static void test_stale_from_boot(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t;

    lg_ctl_init(&c, &cfg, 0);
    for (t = 60; t <= 60000; t += 10) {
        o = tick(&c, t, STALE_AGE_MS, t > 20000);
    }
    CHECK(c.activations == 0); /* nothing was usable, so nothing was detected ... */
    CHECK(o.level == LG_LEVEL_FAILSAFE); /* ... but the level says so */
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights);
}

/* Healthy for 10 s (level 0, gate UP), then the samples turn stale: the level must rise. */
static void test_stale_after_normal_start(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;
    lg_ctl_out_t o;
    uint32_t t;

    lg_ctl_init(&c, &cfg, 0);
    for (t = 10; t <= 10000; t += 10) {
        o = tick(&c, t, 0, 0);
    }
    CHECK(o.level == LG_LEVEL_NORMAL);
    for (; t <= 10000 + cfg.sensor_silence_ms - 20; t += 10) { /* not yet long enough */
        o = tick(&c, t, STALE_AGE_MS, 0);
        CHECK(o.level == LG_LEVEL_NORMAL);
    }
    for (; t <= 11000; t += 10) { /* stale for longer than sensor_silence_ms in a row */
        o = tick(&c, t, STALE_AGE_MS, 1);
    }
    CHECK(o.level == LG_LEVEL_FAILSAFE);
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights);
    CHECK(c.mode.boot_lock == 0);
}

int main(void) {
    test_stale_from_boot();
    test_stale_after_normal_start();
    LG_TEST_DONE("test_controller_stale");
}
