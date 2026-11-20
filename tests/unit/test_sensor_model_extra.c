#include <stdio.h>
#include <string.h>

#include "lg_sensor_model.h"
#include "lg_test.h"

/*
 * Extra tests for core/sensor_model.c: a seeded random walk over fault changes and truth changes
 * checked against an independent oracle after every step, reproducibility, DELAY at the 32-bit
 * wrap and with random delays, history use, dropout statistics, rejected set_fault leaving the
 * state untouched, and fault transitions.
 */

static lg_sensor_model_t m;
static lg_sensor_out_t out;

static uint32_t script_state = 1;

static uint32_t rnd(void) {
    uint32_t x = script_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    script_state = x;
    return x;
}

static int valid_fault(uint8_t mode, uint32_t param) {
    switch (mode) {
    case LG_SFAULT_NONE:
    case LG_SFAULT_STUCK_OCCUPIED:
    case LG_SFAULT_STUCK_CLEAR:
    case LG_SFAULT_HANG:
        return 1;
    case LG_SFAULT_NOISE:
    case LG_SFAULT_DROPOUT:
    case LG_SFAULT_DRIFT:
        return param <= 100;
    case LG_SFAULT_DELAY:
        return param <= LG_SENSOR_DELAY_MAX_MS;
    default:
        return 0;
    }
}

#define MAX_STEPS 40000

typedef struct {
    uint32_t t;
    uint8_t occ;
    uint16_t ax;
} rec_t;

static rec_t recs[MAX_STEPS + 1];

/* Truth at target_ms as the oracle sees it: the newest record at or before it. */
static void oracle_at(size_t n, uint32_t target, uint8_t *occ, uint16_t *ax) {
    size_t i;

    *occ = 0;
    *ax = 0;
    for (i = n; i > 0; i--) {
        if ((int32_t)(uint32_t)(target - recs[i - 1].t) >= 0) {
            *occ = recs[i - 1].occ;
            *ax = recs[i - 1].ax;
            return;
        }
    }
}

static uint32_t oracle_noise(uint8_t mode, uint32_t param, uint32_t since, uint32_t now) {
    if (mode == LG_SFAULT_NOISE) {
        return param;
    }
    if (mode == LG_SFAULT_DRIFT) {
        uint64_t n = LG_SENSOR_BASE_NOISE + (uint64_t)param * (uint64_t)(now - since) / 1000u;

        return n > 100 ? 100 : (uint32_t)n;
    }
    return LG_SENSOR_BASE_NOISE;
}

static uint32_t fnv(uint32_t h, uint32_t v) {
    return (h ^ v) * 16777619u;
}

/*
 * One random walk. Returns the number of violated expectations; *hash summarises all outputs, so
 * two runs with the same arguments must give the same hash.
 */
static unsigned walk(uint32_t script_seed, uint32_t model_seed, uint32_t t0, uint32_t dt, unsigned steps,
                     uint32_t *hash) {
    unsigned bad = 0;
    unsigned s;
    size_t n_recs = 0;
    uint8_t e_mode = LG_SFAULT_NONE;
    uint32_t e_param = 0;
    uint32_t e_since = t0;
    uint8_t truth_occ = 0;
    uint16_t truth_ax = 0;
    uint32_t t = t0;
    uint32_t h = 2166136261u;

    script_state = script_seed;
    lg_sensor_model_init(&m, model_seed, t0);
    recs[n_recs].t = t0;
    recs[n_recs].occ = 0;
    recs[n_recs].ax = 0;
    n_recs++;

    for (s = 0; s < steps; s++) {
        int ret;
        int produced;
        uint8_t e_occ;
        uint16_t e_ax;
        uint32_t noise;
        int32_t amp;
        int level;

        t += dt;
        if (rnd() % 40 == 0) {
            static const uint32_t params[] = {0, 1, 2, 5, 10, 37, 38, 50, 99, 100, 101, 500, 1999, 2000, 2001, 0xFFFFFFFFu};
            uint8_t mode = (uint8_t)(rnd() % 11u);
            uint32_t param = params[rnd() % (sizeof params / sizeof params[0])];
            lg_sensor_model_t before = m;
            int want = valid_fault(mode, param);

            if (mode == 10) {
                mode = 255;
            }
            want = valid_fault(mode, param);
            ret = lg_sensor_model_set_fault(&m, mode, param, t);
            if ((ret == 0) != (want != 0) || (ret != 0 && ret != -1)) {
                bad++;
            }
            if (!want) {
                if (memcmp(&before, &m, sizeof m) != 0) {
                    bad++; /* a rejected call changed something */
                }
            } else {
                uint32_t p = (mode == LG_SFAULT_NONE || mode == LG_SFAULT_STUCK_OCCUPIED ||
                              mode == LG_SFAULT_STUCK_CLEAR || mode == LG_SFAULT_HANG)
                                 ? 0
                                 : param;

                if (mode != e_mode || p != e_param) {
                    e_since = t;
                }
                e_mode = mode;
                e_param = p;
                if (m.fault != e_mode || m.param != e_param || m.fault_since_ms != e_since) {
                    bad++;
                }
            }
        }
        if (rnd() % 30 == 0) {
            if (rnd() & 1u) {
                truth_occ = (uint8_t)!truth_occ;
            } else {
                truth_ax = (uint16_t)(truth_ax + 1 + rnd() % 3);
            }
        }
        recs[n_recs].t = t;
        recs[n_recs].occ = truth_occ;
        recs[n_recs].ax = truth_ax;
        n_recs++;

        memset(&out, 0xEE, sizeof out);
        produced = lg_sensor_model_step(&m, t, truth_occ, truth_ax, &out);
        h = fnv(h, (uint32_t)produced);

        if (e_mode == LG_SFAULT_HANG) {
            if (produced != 0) {
                bad++;
            }
            continue;
        }
        if (e_mode == LG_SFAULT_DROPOUT && e_param == 0 && !produced) {
            bad++;
        }
        if (e_mode == LG_SFAULT_DROPOUT && e_param == 100 && produced) {
            bad++;
        }
        if (e_mode != LG_SFAULT_DROPOUT && !produced) {
            bad++;
        }
        if (!produced) {
            continue;
        }
        if (e_mode == LG_SFAULT_STUCK_OCCUPIED) {
            e_occ = 1;
            e_ax = truth_ax;
        } else if (e_mode == LG_SFAULT_STUCK_CLEAR) {
            e_occ = 0;
            e_ax = truth_ax;
        } else if (e_mode == LG_SFAULT_DELAY) {
            oracle_at(n_recs, t - e_param, &e_occ, &e_ax);
        } else {
            e_occ = truth_occ;
            e_ax = truth_ax;
        }
        noise = oracle_noise(e_mode, e_param, e_since, t);
        amp = (int32_t)(noise * 8);
        level = e_occ ? 800 : 200;

        if (out.raw_level > 1023) {
            bad++;
        }
        if ((int)out.raw_level < (level - amp > 0 ? level - amp : 0) ||
            (int)out.raw_level > (level + amp < 1023 ? level + amp : 1023)) {
            bad++;
        }
        if (out.quality != 100u - noise) {
            bad++;
        }
        if (out.occupied != (out.raw_level > 500 ? 1 : 0)) {
            bad++;
        }
        if (noise <= 37 && out.occupied != e_occ) {
            bad++;
        }
        if (out.axle_count != e_ax) {
            bad++;
        }
        if (lg_sensor_model_noise(&m, t) != noise) {
            bad++;
        }
        h = fnv(h, out.raw_level);
        h = fnv(h, out.quality);
        h = fnv(h, out.occupied);
        h = fnv(h, out.axle_count);
    }
    *hash = h;
    return bad;
}

static void test_random_walks(void) {
    unsigned bad = 0;
    unsigned w;

    printf("sensor extra random walks\n");
    for (w = 0; w < 60; w++) {
        uint32_t h1;
        uint32_t h2;
        uint32_t t0 = (w % 3 == 0) ? 0xFFFFFFFFu - 3000u - w : (w % 3 == 1 ? 1000u * w : 0x7FFFFFF0u);
        uint32_t dt = (w & 1u) ? 1u : 10u;
        unsigned steps = (w & 1u) ? 6000u : 2500u;

        bad += walk(0x1000u + w, 0x77u + w * 31u, t0, dt, steps, &h1);
        /* The same script and seed again: identical outputs. */
        bad += walk(0x1000u + w, 0x77u + w * 31u, t0, dt, steps, &h2);
        if (h1 != h2) {
            bad++;
            printf("walk %u not reproducible\n", w);
        }
        /* A different model seed changes the noise but not the verdicts. */
        bad += walk(0x1000u + w, 0x99u + w * 17u, t0, dt, steps, &h2);
    }
    CHECK(bad == 0);
}

static void test_seed_reproducibility(void) {
    lg_sensor_model_t a;
    lg_sensor_model_t b;
    lg_sensor_model_t z;
    lg_sensor_model_t d;
    lg_sensor_out_t oa;
    lg_sensor_out_t ob;
    lg_sensor_out_t oz;
    lg_sensor_out_t od;
    int t;
    int same_ab = 0;
    int same_zd = 0;
    int differ = 0;

    lg_sensor_model_init(&a, 12345, 0);
    lg_sensor_model_init(&b, 12345, 0);
    lg_sensor_model_init(&z, 0, 0);
    lg_sensor_model_init(&d, 0x9E3779B9u, 0);
    CHECK(lg_sensor_model_set_fault(&a, LG_SFAULT_NOISE, 60, 0) == 0);
    CHECK(lg_sensor_model_set_fault(&b, LG_SFAULT_NOISE, 60, 0) == 0);
    CHECK(lg_sensor_model_set_fault(&z, LG_SFAULT_NOISE, 60, 0) == 0);
    CHECK(lg_sensor_model_set_fault(&d, LG_SFAULT_NOISE, 60, 0) == 0);
    for (t = 1; t <= 500; t++) {
        CHECK(lg_sensor_model_step(&a, (uint32_t)t, 1, 0, &oa) == 1);
        CHECK(lg_sensor_model_step(&b, (uint32_t)t, 1, 0, &ob) == 1);
        CHECK(lg_sensor_model_step(&z, (uint32_t)t, 1, 0, &oz) == 1);
        CHECK(lg_sensor_model_step(&d, (uint32_t)t, 1, 0, &od) == 1);
        same_ab += oa.raw_level == ob.raw_level;
        same_zd += oz.raw_level == od.raw_level;
        differ += oa.raw_level != oz.raw_level;
    }
    CHECK(same_ab == 500);
    CHECK(same_zd == 500); /* seed 0 is the documented fixed constant */
    CHECK(differ > 300);
    /* Poor seeds 1, 2, 3 give different sequences and never a constant output. */
    {
        uint16_t first[4][16];
        int s;
        int i;

        for (s = 1; s <= 3; s++) {
            lg_sensor_model_t p;
            lg_sensor_out_t o;

            lg_sensor_model_init(&p, (uint32_t)s, 0);
            CHECK(lg_sensor_model_set_fault(&p, LG_SFAULT_NOISE, 60, 0) == 0);
            for (i = 0; i < 16; i++) {
                (void)lg_sensor_model_step(&p, (uint32_t)i + 1, 0, 0, &o);
                first[s][i] = o.raw_level;
            }
        }
        CHECK(memcmp(first[1], first[2], sizeof first[1]) != 0);
        CHECK(memcmp(first[2], first[3], sizeof first[1]) != 0);
        CHECK(memcmp(first[1], first[3], sizeof first[1]) != 0);
        for (i = 1; i < 16; i++) {
            if (first[1][i] != first[1][0]) {
                break;
            }
        }
        CHECK(i < 16);
    }
}

static void test_set_fault_ranges(void) {
    uint32_t mode;
    uint32_t p;
    static const uint32_t probes[] = {0, 1, 99, 100, 101, 1000, 1999, 2000, 2001, 4294967295u};
    size_t i;

    for (mode = 0; mode < 256; mode++) {
        for (i = 0; i < sizeof probes / sizeof probes[0]; i++) {
            lg_sensor_model_t before;
            int rc;

            p = probes[i];
            lg_sensor_model_init(&m, 5, 100);
            (void)lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 7, 150);
            before = m;
            rc = lg_sensor_model_set_fault(&m, (uint8_t)mode, p, 900);
            CHECK((rc == 0) == (valid_fault((uint8_t)mode, p) != 0));
            if (rc != 0) {
                CHECK(rc == -1);
                CHECK(memcmp(&before, &m, sizeof m) == 0);
            } else {
                CHECK(m.fault == mode);
            }
        }
    }
    /* Explicit boundaries. */
    lg_sensor_model_init(&m, 1, 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NOISE, 100, 0) == 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NOISE, 101, 0) == -1 && m.param == 100);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DROPOUT, 100, 0) == 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DROPOUT, 101, 0) == -1 && m.fault == LG_SFAULT_DROPOUT);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 100, 0) == 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 101, 0) == -1);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, LG_SENSOR_DELAY_MAX_MS, 0) == 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, LG_SENSOR_DELAY_MAX_MS + 1, 0) == -1);
    CHECK(m.fault == LG_SFAULT_DELAY && m.param == LG_SENSOR_DELAY_MAX_MS);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_CRASH, 0, 0) == -1);
    CHECK(lg_sensor_model_set_fault(&m, 9, 0, 0) == -1);
    CHECK(lg_sensor_model_set_fault(&m, 255, 0, 0) == -1);
    /* Parameter-less modes ignore any parameter, even a huge one. */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_STUCK_CLEAR, 0xFFFFFFFFu, 0) == 0 && m.param == 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_HANG, 12345, 0) == 0 && m.param == 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NONE, 77, 0) == 0 && m.param == 0 && m.fault == LG_SFAULT_NONE);
}

static void test_delay_random(void) {
    int round;
    unsigned bad = 0;

    for (round = 0; round < 60; round++) {
        uint32_t t0 = (round & 1) ? 0xFFFFFFFFu - 2500u : 5000u;
        uint32_t delay = rnd() % 2001;
        uint32_t t;
        uint32_t k;
        uint8_t occ = 0;
        uint16_t ax = 0;
        static uint8_t occ_at[6001];
        static uint16_t ax_at[6001];

        script_state = 0xABC00u + (uint32_t)round;
        lg_sensor_model_init(&m, 3 + (uint32_t)round, t0);
        CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, delay, t0) == 0);
        occ_at[0] = 0;
        ax_at[0] = 0;
        for (k = 1; k <= 6000; k++) {
            t = t0 + k;
            if (rnd() % 25 == 0) {
                if (rnd() & 1u) {
                    occ = (uint8_t)!occ;
                } else {
                    ax = (uint16_t)(ax + 1);
                }
            }
            occ_at[k] = occ;
            ax_at[k] = ax;
            (void)lg_sensor_model_step(&m, t, occ, ax, &out);
            {
                uint8_t eo = 0;
                uint16_t ea = 0;

                if (k >= delay) {
                    eo = occ_at[k - delay];
                    ea = ax_at[k - delay];
                }
                if (out.axle_count != ea || out.occupied != eo) {
                    bad++;
                }
            }
        }
    }
    CHECK(bad == 0);

    /* DELAY(0) is the live truth, also across the wrap. */
    lg_sensor_model_init(&m, 1, 0xFFFFFFFEu);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, 0, 0xFFFFFFFEu) == 0);
    (void)lg_sensor_model_step(&m, 0xFFFFFFFFu, 1, 4, &out);
    CHECK(out.occupied == 1 && out.axle_count == 4);
    (void)lg_sensor_model_step(&m, 0u, 0, 5, &out);
    CHECK(out.occupied == 0 && out.axle_count == 5);
    (void)lg_sensor_model_step(&m, 1u, 1, 6, &out);
    CHECK(out.occupied == 1 && out.axle_count == 6);
}

static void test_history_use(void) {
    /* A change on every ms (axle count and occupancy alternate). Delays that the ring covers
     * (a change per ms means 512 entries cover 511 ms) must be exact for the whole run. */
    static const uint32_t delays[] = {0, 1, 2, 100, 255, 256, 400, 500, 510, 511};
    size_t d;

    for (d = 0; d < sizeof delays / sizeof delays[0]; d++) {
        uint32_t t;
        unsigned bad = 0;

        lg_sensor_model_init(&m, 7, 0);
        CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, delays[d], 0) == 0);
        for (t = 1; t <= 5000; t++) {
            (void)lg_sensor_model_step(&m, t, (uint8_t)(t & 1u), (uint16_t)t, &out);
            if (t >= delays[d] + 1) {
                uint32_t src = t - delays[d];

                if (out.axle_count != (uint16_t)src || out.occupied != (src & 1u)) {
                    bad++;
                }
            }
        }
        if (bad != 0) {
            printf("history: delay %u wrong in %u steps\n", (unsigned)delays[d], bad);
        }
        CHECK(bad == 0);
    }
    /* A burst of changes shorter than the ring, then quiet: a long delay still finds the old
     * values (the ring keeps the last 512 changes, not the last 512 ms). */
    lg_sensor_model_init(&m, 8, 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, 2000, 0) == 0);
    {
        uint32_t t;

        for (t = 1; t <= 6000; t++) {
            uint8_t occ = (t >= 100 && t < 300) ? 1 : 0;
            uint16_t ax = (t >= 1000) ? 9 : 0;

            (void)lg_sensor_model_step(&m, t, occ, ax, &out);
            if (t == 2150) {
                CHECK(out.occupied == 1 && out.axle_count == 0); /* truth at 150 */
            }
            if (t == 2400) {
                CHECK(out.occupied == 0 && out.axle_count == 0); /* truth at 400 */
            }
            if (t == 3500) {
                CHECK(out.axle_count == 9); /* truth at 1500 */
            }
            if (t == 2900) {
                CHECK(out.axle_count == 0); /* truth at 900 */
            }
            if (t == 3000) {
                CHECK(out.axle_count == 9); /* truth at 1000 */
            }
        }
    }
}

static void test_dropout_statistics(void) {
    static const uint32_t pcts[] = {1, 10, 25, 50, 75, 90, 99};
    uint32_t seed;
    size_t i;

    for (i = 0; i < sizeof pcts / sizeof pcts[0]; i++) {
        for (seed = 1; seed <= 6; seed++) {
            uint32_t t;
            unsigned dropped = 0;
            const unsigned n = 20000;

            lg_sensor_model_init(&m, seed * 2654435761u, 0);
            CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DROPOUT, pcts[i], 0) == 0);
            for (t = 1; t <= n; t++) {
                if (!lg_sensor_model_step(&m, t, 0, 0, &out)) {
                    dropped++;
                } else {
                    CHECK(out.occupied == 0 && out.quality == 100 - LG_SENSOR_BASE_NOISE);
                }
            }
            {
                double rate = 100.0 * (double)dropped / (double)n;
                double want = (double)pcts[i];
                double tol = 2.5;

                if (rate < want - tol || rate > want + tol) {
                    printf("dropout %u%% seed %u: measured %.2f%%\n", (unsigned)pcts[i], (unsigned)seed, rate);
                }
                CHECK(rate >= want - tol && rate <= want + tol);
            }
        }
    }
    lg_sensor_model_init(&m, 4, 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DROPOUT, 0, 0) == 0);
    {
        uint32_t t;
        unsigned dropped = 0;

        for (t = 1; t <= 5000; t++) {
            dropped += lg_sensor_model_step(&m, t, 1, 0, &out) == 0;
        }
        CHECK(dropped == 0);
        CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DROPOUT, 100, 5000) == 0);
        for (t = 5001; t <= 10000; t++) {
            dropped += lg_sensor_model_step(&m, t, 1, 0, &out) == 0;
        }
        CHECK(dropped == 5000);
    }
}

static void test_transitions(void) {
    uint32_t t;

    /* HANG: no samples, the history goes on; DELAY afterwards reports what happened while hung. */
    lg_sensor_model_init(&m, 21, 0);
    for (t = 1; t <= 100; t++) {
        CHECK(lg_sensor_model_step(&m, t, 0, 0, &out) == 1);
    }
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_HANG, 0, 100) == 0);
    for (t = 101; t <= 400; t++) {
        CHECK(lg_sensor_model_step(&m, t, t >= 200 && t < 300, (uint16_t)(t >= 250 ? 3 : 0), &out) == 0);
    }
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DELAY, 200, 400) == 0);
    CHECK(lg_sensor_model_step(&m, 401, 0, 3, &out) == 1); /* truth at 201: occupied */
    CHECK(out.occupied == 1 && out.axle_count == 0);
    CHECK(lg_sensor_model_step(&m, 460, 0, 3, &out) == 1); /* truth at 260: occupied, 3 axles */
    CHECK(out.occupied == 1 && out.axle_count == 3);
    CHECK(lg_sensor_model_step(&m, 500, 0, 3, &out) == 1); /* truth at 300: clear */
    CHECK(out.occupied == 0 && out.axle_count == 3);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NONE, 0, 500) == 0);
    CHECK(lg_sensor_model_step(&m, 501, 1, 4, &out) == 1 && out.occupied == 1 && out.axle_count == 4);

    /* STUCK then DRIFT: the drift starts when it is set, the stuck state is gone. */
    lg_sensor_model_init(&m, 22, 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_STUCK_OCCUPIED, 0, 0) == 0);
    CHECK(lg_sensor_model_step(&m, 10, 0, 0, &out) == 1 && out.occupied == 1);
    CHECK(out.quality == 100 - LG_SENSOR_BASE_NOISE); /* a stuck sensor looks healthy */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 10, 5000) == 0);
    CHECK(lg_sensor_model_noise(&m, 5000) == LG_SENSOR_BASE_NOISE);
    CHECK(lg_sensor_model_noise(&m, 5999) == LG_SENSOR_BASE_NOISE + 9);
    CHECK(lg_sensor_model_noise(&m, 6000) == LG_SENSOR_BASE_NOISE + 10);
    CHECK(lg_sensor_model_step(&m, 5000, 0, 0, &out) == 1 && out.occupied == 0 &&
          out.quality == 100 - LG_SENSOR_BASE_NOISE); /* the drift starts at the healthy quality */
    CHECK(lg_sensor_model_noise(&m, 15000) == 100);
    CHECK(lg_sensor_model_noise(&m, 5000u + 1000000u) == 100);
    /* Repeating the same drift keeps the start; a changed rate starts again. */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 10, 8000) == 0);
    CHECK(lg_sensor_model_noise(&m, 8000) == LG_SENSOR_BASE_NOISE + 30);
    CHECK(m.fault_since_ms == 5000);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 11, 8000) == 0);
    CHECK(lg_sensor_model_noise(&m, 8000) == LG_SENSOR_BASE_NOISE);
    CHECK(m.fault_since_ms == 8000);
    /* A rejected set does not restart it either. */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 101, 9000) == -1);
    CHECK(m.fault_since_ms == 8000);
    /* A fault other than drift in between does restart it. */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NONE, 0, 9000) == 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 11, 9500) == 0);
    CHECK(m.fault_since_ms == 9500);
    /* Parameter-less modes repeated with another parameter: no restart. */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_STUCK_CLEAR, 3, 10000) == 0);
    CHECK(m.fault_since_ms == 10000);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_STUCK_CLEAR, 9, 12000) == 0);
    CHECK(m.fault_since_ms == 10000);
    /* Drift rate 0 never adds noise, rate 100 reaches the cap after 970 ms (base noise 3 plus 97). */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 0, 20000) == 0);
    CHECK(lg_sensor_model_noise(&m, 20000 + 100000) == LG_SENSOR_BASE_NOISE);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 100, 30000) == 0);
    CHECK(lg_sensor_model_noise(&m, 30000 + 960) == 99);
    CHECK(lg_sensor_model_noise(&m, 30000 + 970) == 100);
    /* Drift across the 32-bit wrap. */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_DRIFT, 10, 0xFFFFFC00u) == 0);
    CHECK(lg_sensor_model_noise(&m, 0xFFFFFC00u) == LG_SENSOR_BASE_NOISE);
    CHECK(lg_sensor_model_noise(&m, 0x00000000u) == LG_SENSOR_BASE_NOISE + 10); /* 1024 ms later */
    CHECK(lg_sensor_model_noise(&m, 0x000003FFu) == LG_SENSOR_BASE_NOISE + 20);
    /* Before the drift started (time went back): only the base noise, never a huge value. */
    CHECK(lg_sensor_model_noise(&m, 0xFFFFFB00u) == LG_SENSOR_BASE_NOISE);

    /* NOISE then NONE returns to base noise and quality. */
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NOISE, 100, 40000) == 0);
    CHECK(lg_sensor_model_step(&m, 40001, 1, 0, &out) == 1 && out.quality == 0 && out.raw_level <= 1023);
    CHECK(lg_sensor_model_noise(&m, 1) == 100);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NONE, 0, 40002) == 0);
    CHECK(lg_sensor_model_step(&m, 40003, 1, 0, &out) == 1 && out.quality == 97 && out.occupied == 1);
}

static void test_raw_range_all_noise(void) {
    uint32_t noise;
    uint32_t seed;

    for (noise = 0; noise <= 100; noise++) {
        for (seed = 1; seed <= 3; seed++) {
            uint32_t t;
            int occ;

            lg_sensor_model_init(&m, seed + noise * 1000u, 0);
            CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NOISE, noise, 0) == 0);
            for (t = 1; t <= 300; t++) {
                occ = (int)(t & 1u);
                CHECK(lg_sensor_model_step(&m, t, (uint8_t)occ, 0, &out) == 1);
                CHECK(out.raw_level <= 1023);
                CHECK(out.quality == 100 - noise);
                CHECK(out.occupied == (out.raw_level > 500));
                if (noise <= 37) {
                    CHECK(out.occupied == occ);
                }
            }
        }
    }
    /* Truth values other than 0 and 1 count as occupied. */
    lg_sensor_model_init(&m, 3, 0);
    CHECK(lg_sensor_model_step(&m, 1, 200, 0, &out) == 1 && out.occupied == 1);
    CHECK(lg_sensor_model_step(&m, 2, 255, 0, &out) == 1 && out.occupied == 1);
    CHECK(lg_sensor_model_step(&m, 3, 0, 0xFFFF, &out) == 1 && out.axle_count == 0xFFFF);
}

/* The error is uniform over the full +-amp range: both ends are reached and never exceeded. */
static void test_noise_reaches_full_range(void) {
    uint32_t t;
    int lo = 5000;
    int hi = -1;

    lg_sensor_model_init(&m, 99, 0);
    CHECK(lg_sensor_model_set_fault(&m, LG_SFAULT_NOISE, 10, 0) == 0); /* amp 80 around 200 */
    for (t = 1; t <= 20000; t++) {
        CHECK(lg_sensor_model_step(&m, t, 0, 0, &out) == 1);
        if (out.raw_level < lo) {
            lo = out.raw_level;
        }
        if (out.raw_level > hi) {
            hi = out.raw_level;
        }
    }
    CHECK(lo == 200 - 80);
    CHECK(hi == 200 + 80);
}

int main(void) {
    test_noise_reaches_full_range();
    test_random_walks();
    test_seed_reproducibility();
    test_set_fault_ranges();
    test_delay_random();
    test_history_use();
    test_dropout_statistics();
    test_transitions();
    test_raw_range_all_noise();
    LG_TEST_DONE("test_sensor_model_extra");
}
