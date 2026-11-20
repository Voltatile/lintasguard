#include <string.h>

#include "lg_sensor_model.h"
#include "lg_test.h"

static lg_sensor_model_t m;
static lg_sensor_out_t out;

#define CLEAR LG_SENSOR_CLEAR_LEVEL
#define OCC LG_SENSOR_OCCUPIED_LEVEL

static void fresh(uint32_t seed) {
    lg_sensor_model_init(&m, seed, 0);
}

static int step(uint32_t t, uint8_t occ, uint16_t axles) {
    return lg_sensor_model_step(&m, t, occ, axles, &out);
}

static int near_level(uint16_t raw, int level, int amp) {
    return raw >= level - amp && raw <= level + amp;
}

static void test_healthy(void) {
    uint32_t t;
    int amp = LG_SENSOR_BASE_NOISE * LG_SENSOR_COUNTS_PER_NOISE;

    fresh(1);
    for (t = 1; t <= 2000; t++) {
        CHECK(step(t, 0, 0) == 1);
        CHECK(out.occupied == 0 && near_level(out.raw_level, CLEAR, amp));
        CHECK(out.quality == 100 - LG_SENSOR_BASE_NOISE);
        CHECK(out.axle_count == 0);
    }
    for (t = 2001; t <= 4000; t++) {
        CHECK(step(t, 1, 5) == 1);
        CHECK(out.occupied == 1 && near_level(out.raw_level, OCC, amp));
        CHECK(out.axle_count == 5); /* the axle count follows the truth */
    }
    CHECK(lg_sensor_model_noise(&m, 5000) == LG_SENSOR_BASE_NOISE);
}

static void test_determinism(void) {
    lg_sensor_model_t a;
    lg_sensor_model_t b;
    lg_sensor_model_t c;
    lg_sensor_out_t oa;
    lg_sensor_out_t ob;
    lg_sensor_out_t oc;
    uint32_t t;
    int differs = 0;

    lg_sensor_model_init(&a, 42, 0);
    lg_sensor_model_init(&b, 42, 0);
    lg_sensor_model_init(&c, 43, 0);
    (void)lg_sensor_model_set_fault(&a, LG_SFAULT_NOISE, 50, 0);
    (void)lg_sensor_model_set_fault(&b, LG_SFAULT_NOISE, 50, 0);
    (void)lg_sensor_model_set_fault(&c, LG_SFAULT_NOISE, 50, 0);
    for (t = 1; t <= 3000; t++) {
        (void)lg_sensor_model_step(&a, t, 0, 0, &oa);
        (void)lg_sensor_model_step(&b, t, 0, 0, &ob);
        (void)lg_sensor_model_step(&c, t, 0, 0, &oc);
        CHECK(oa.raw_level == ob.raw_level && oa.occupied == ob.occupied);
        if (oa.raw_level != oc.raw_level) {
            differs = 1;
        }
    }
    CHECK(differs); /* another seed gives another sequence */

    /* Seed 0 is replaced, not left as a stuck all-zero generator. */
    fresh(0);
    (void)lg_sensor_model_set_fault(&m, LG_SFAULT_NOISE, 50, 0);
    {
        uint16_t first = 0;
        int varies = 0;

        for (t = 1; t <= 100; t++) {
            (void)step(t, 0, 0);
            if (t == 1) {
                first = out.raw_level;
            } else if (out.raw_level != first) {
                varies = 1;
            }
        }
        CHECK(varies);
    }
}

static void test_noise(void) {
    uint32_t t;
    uint16_t lo = 1023;
    uint16_t hi = 0;
    int flips = 0;
    int n = 20000;

    /* Noise 0: perfectly clean, quality 100. */
    fresh(7);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NOISE, 0, 0) == 0);
    CHECK(step(1, 0, 0) == 1 && out.raw_level == CLEAR && out.quality == 100);
    CHECK(step(2, 1, 0) == 1 && out.raw_level == OCC && out.occupied == 1);

    /* Noise 50: +-400 counts. Clear 200 plus up to 400 can pass the threshold (500): flips. */
    fresh(7);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NOISE, 50, 0) == 0);
    for (t = 1; t <= (uint32_t)n; t++) {
        (void)step(t, 0, 0);
        if (out.raw_level < lo) {
            lo = out.raw_level;
        }
        if (out.raw_level > hi) {
            hi = out.raw_level;
        }
        flips += out.occupied;
        CHECK(out.quality == 50);
        CHECK(out.raw_level <= LG_SENSOR_RAW_MAX);
    }
    /* uniform on [-400, 400] around 200: it reaches the clamp at 0 and passes 500 about 12.5 percent */
    CHECK(lo == 0);
    CHECK(hi >= 590 && hi <= 600);
    CHECK(flips > n * 9 / 100 && flips < n * 16 / 100);

    /* The same noise with a train: raw spreads around 800 and is clamped at 1023. */
    fresh(7);
    (void)lg_sensor_model_set_fault(&m, LG_SFAULT_NOISE, 50, 0);
    hi = 0;
    lo = 1023;
    flips = 0;
    for (t = 1; t <= (uint32_t)n; t++) {
        (void)step(t, 1, 0);
        if (out.raw_level > hi) {
            hi = out.raw_level;
        }
        if (out.raw_level < lo) {
            lo = out.raw_level;
        }
        flips += out.occupied == 0;
    }
    CHECK(hi == LG_SENSOR_RAW_MAX && lo >= 400 && lo <= 410);
    CHECK(flips > n * 9 / 100 && flips < n * 16 / 100);

    /* Noise 100: quality 0, still inside the ADC range. */
    fresh(7);
    (void)lg_sensor_model_set_fault(&m, LG_SFAULT_NOISE, 100, 0);
    for (t = 1; t <= 5000; t++) {
        (void)step(t, t & 1, 0);
        CHECK(out.quality == 0 && out.raw_level <= LG_SENSOR_RAW_MAX);
    }
}

static void test_dropout(void) {
    uint32_t t;
    int produced = 0;
    int n = 20000;

    fresh(3);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DROPOUT, 0, 0) == 0);
    for (t = 1; t <= 1000; t++) {
        CHECK(step(t, 0, 0) == 1);
    }
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DROPOUT, 100, 1000) == 0);
    for (t = 1001; t <= 2000; t++) {
        CHECK(step(t, 0, 0) == 0);
    }
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DROPOUT, 50, 2000) == 0);
    for (t = 2001; t <= (uint32_t)(2000 + n); t++) {
        produced += step(t, 0, 0);
    }
    CHECK(produced > n * 45 / 100 && produced < n * 55 / 100);

    /* Dropout leaves the quality of the samples that do get through unchanged. */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DROPOUT, 50, 30000) == 0);
    for (t = 30001; t <= 30200; t++) {
        if (step(t, 0, 0)) {
            CHECK(out.quality == 100 - LG_SENSOR_BASE_NOISE);
        }
    }
}

static void test_delay(void) {
    uint32_t t;

    /* The truth turns occupied at t = 1000 (and axles count up); with delay 300 the sensor follows at 1300. */
    fresh(5);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, 300, 0) == 0);
    for (t = 1; t <= 3000; t++) {
        uint8_t occ = t >= 1000 && t < 2000;
        uint16_t axles = t >= 1500 ? 4 : 0;

        CHECK(step(t, occ, axles) == 1);
        CHECK(out.occupied == (t >= 1300 && t < 2300));
        CHECK(out.axle_count == (t >= 1800 ? 4 : 0));
    }

    /* Delay 0 is no delay. */
    fresh(5);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, 0, 0) == 0);
    for (t = 1; t <= 200; t++) {
        (void)step(t, t >= 100, 0);
        CHECK(out.occupied == (t >= 100));
    }

    /* The largest delay is accepted, one more is not. */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, LG_SENSOR_DELAY_MAX_MS, 200) == 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, LG_SENSOR_DELAY_MAX_MS + 1, 200) == -1);
    CHECK(m.param == LG_SENSOR_DELAY_MAX_MS); /* a rejected request changed nothing */

    /* Setting the delay after the truth changed still uses the history: the old state is shown. */
    fresh(5);
    for (t = 1; t <= 1900; t++) {
        (void)step(t, 0, 0);
    }
    (void)step(1900, 1, 0); /* the train arrives */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, 500, 1900) == 0);
    for (t = 1901; t <= 2399; t++) {
        (void)step(t, 1, 0);
        CHECK(out.occupied == 0);
    }
    (void)step(2400, 1, 0);
    CHECK(out.occupied == 1);
    /* Clearing the fault shows the present at once. */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NONE, 0, 2400) == 0);
    (void)step(2401, 0, 0);
    CHECK(out.occupied == 0);

    /* Before any history exists the sensor reports clear, not garbage. */
    fresh(5);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, 1000, 0) == 0);
    (void)step(10, 1, 7);
    CHECK(out.occupied == 0 && out.axle_count == 0);
}

static void test_delay_history_overflow(void) {
    uint32_t t;
    uint32_t k;

    /* A burst of axle changes longer than the history: recent lookups stay right, nothing breaks. */
    fresh(9);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, 100, 0) == 0);
    for (t = 1, k = 0; t <= 3000; t++) {
        k = t;
        (void)step(t, 1, (uint16_t)k); /* a new axle every ms: 3000 changes */
        if (t > 200) {
            CHECK(out.axle_count == t - 100);
        }
    }
}

static void test_delay_across_wrap(void) {
    uint32_t t0 = 0xFFFFFFFFu - 500u;
    uint32_t t;

    lg_sensor_model_init(&m, 11, t0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, 300, t0) == 0);
    for (t = t0 + 1; t != t0 + 1500; t++) { /* crosses the 32-bit wrap */
        uint32_t since = t - t0;

        (void)step(t, since >= 400, 0);
        CHECK(out.occupied == (since >= 700));
    }
}

static void test_drift(void) {
    uint32_t t;

    fresh(2);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 10, 1000) == 0);
    CHECK(lg_sensor_model_noise(&m, 1000) == LG_SENSOR_BASE_NOISE); /* starts at the base noise */
    CHECK(lg_sensor_model_noise(&m, 2000) == LG_SENSOR_BASE_NOISE + 10);
    CHECK(lg_sensor_model_noise(&m, 3500) == LG_SENSOR_BASE_NOISE + 25);
    CHECK(lg_sensor_model_noise(&m, 11000) == 100);
    CHECK(lg_sensor_model_noise(&m, 1000000) == 100); /* capped */
    CHECK(lg_sensor_model_noise(&m, 500) == LG_SENSOR_BASE_NOISE); /* before the start: no drift */

    /* Quality falls as the noise grows. */
    (void)step(2000, 0, 0);
    CHECK(out.quality == 100 - LG_SENSOR_BASE_NOISE - 10);
    (void)step(6000, 0, 0);
    CHECK(out.quality == 100 - LG_SENSOR_BASE_NOISE - 50);
    (void)step(20000, 0, 0);
    CHECK(out.quality == 0);

    /* Sending the same drift again does not restart it; a different rate does. */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 10, 6000) == 0);
    CHECK(lg_sensor_model_noise(&m, 6000) == LG_SENSOR_BASE_NOISE + 50);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 20, 6000) == 0);
    CHECK(lg_sensor_model_noise(&m, 6000) == LG_SENSOR_BASE_NOISE);
    CHECK(lg_sensor_model_noise(&m, 7000) == LG_SENSOR_BASE_NOISE + 20);

    /* Rate bounds. */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 100, 7000) == 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 101, 7000) == -1);

    /* The raw spread really grows with the drift. */
    fresh(2);
    (void)lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 20, 0);
    {
        int spread_early = 0;
        int spread_late = 0;

        for (t = 1; t <= 200; t++) { /* noise stays at 7 units or less: +-56 counts */
            (void)step(t, 0, 0);
            if (out.raw_level > CLEAR + 8 * LG_SENSOR_COUNTS_PER_NOISE) {
                spread_early = 1;
            }
        }
        for (t = 4000; t <= 4500; t++) {
            (void)step(t, 0, 0);
            if (out.raw_level > CLEAR + 8 * LG_SENSOR_COUNTS_PER_NOISE) {
                spread_late = 1;
            }
        }
        CHECK(!spread_early && spread_late);
    }
}

static void test_stuck(void) {
    uint32_t t;
    int amp = LG_SENSOR_BASE_NOISE * LG_SENSOR_COUNTS_PER_NOISE;

    fresh(4);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_STUCK_OCCUPIED, 0, 0) == 0);
    for (t = 1; t <= 500; t++) {
        CHECK(step(t, 0, 0) == 1);
        CHECK(out.occupied == 1 && near_level(out.raw_level, OCC, amp));
        CHECK(out.quality == 100 - LG_SENSOR_BASE_NOISE); /* a stuck sensor looks healthy */
    }
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_STUCK_CLEAR, 0, 500) == 0);
    for (t = 501; t <= 1000; t++) {
        CHECK(step(t, 1, 3) == 1);
        CHECK(out.occupied == 0 && near_level(out.raw_level, CLEAR, amp));
        CHECK(out.axle_count == 3); /* the axle counter is a separate device */
    }
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NONE, 0, 1000) == 0);
    (void)step(1001, 1, 3);
    CHECK(out.occupied == 1);
}

static void test_hang(void) {
    uint32_t t;

    fresh(6);
    CHECK(step(1, 0, 0) == 1);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_HANG, 0, 1) == 0);
    for (t = 2; t <= 1000; t++) {
        CHECK(step(t, t > 500, 2) == 0); /* the truth changes during the hang, nothing is reported */
    }
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NONE, 0, 1000) == 0);
    CHECK(step(1001, 1, 2) == 1);
    CHECK(out.occupied == 1 && out.axle_count == 2); /* after the hang it reports the present */
}

static void test_invalid_faults(void) {
    fresh(8);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NOISE, 30, 0) == 0);

    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_CRASH, 0, 10) == -1);
    CHECK(lg_sensor_model_set_fault(&m, 9, 0, 10) == -1);
    CHECK(lg_sensor_model_set_fault(&m, 200, 0, 10) == -1);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NOISE, 101, 10) == -1);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DROPOUT, 101, 10) == -1);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, LG_SENSOR_DELAY_MAX_MS + 1, 10) == -1);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 101, 10) == -1);
    CHECK(m.fault == LG_SFAULT_NOISE && m.param == 30); /* every rejection changed nothing */

    /* The modes that take no parameter ignore a stray one. */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_STUCK_CLEAR, 99999, 10) == 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_HANG, 99999, 10) == 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NONE, 99999, 10) == 0);
    CHECK(m.fault == LG_SFAULT_NONE);
    CHECK(lg_sensor_model_noise(&m, 10) == LG_SENSOR_BASE_NOISE);
}

static void test_names(void) {
    CHECK(strcmp(lg_sensor_fault_name(LG_SFAULT_NONE), "NONE") == 0);
    CHECK(strcmp(lg_sensor_fault_name(LG_SFAULT_STUCK_OCCUPIED), "STUCK_OCCUPIED") == 0);
    CHECK(strcmp(lg_sensor_fault_name(LG_SFAULT_HANG), "HANG") == 0);
    CHECK(strcmp(lg_sensor_fault_name(77), "UNKNOWN") == 0);
}

int main(void) {
    test_healthy();
    test_determinism();
    test_noise();
    test_dropout();
    test_delay();
    test_delay_history_overflow();
    test_delay_across_wrap();
    test_drift();
    test_stuck();
    test_hang();
    test_invalid_faults();
    test_names();
    LG_TEST_DONE("test_sensor_model");
}
