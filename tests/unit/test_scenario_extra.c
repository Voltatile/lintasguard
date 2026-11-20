#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lg_scenario.h"
#include "lg_test.h"

/*
 * Extra tests for core/scenario.c: every range boundary (at the edge and one milli unit beyond),
 * whole number checks, train count limits, same-direction overlap at zero gap and one mm either
 * side, overflow shapes, an independent brute force oracle for the overlap rule, and lg_read_file
 * at the exact capacity boundaries.
 */

static lg_json_doc_t doc;
static lg_crossing_cfg_t cfg;
static lg_scenario_t sc;
static char err[LG_ERR_LEN];
static char big[40000];

static int load_json(const char *s) {
    return lg_json_parse(&doc, s, strlen(s));
}

static int crossing_text(const char *s) {
    memset(&cfg, 0, sizeof cfg);
    err[0] = '\0';
    if (load_json(s) != LG_JSON_OK) {
        return -2;
    }
    return lg_crossing_from_json(&doc, &cfg, err, sizeof err);
}

static int crossing_vals(const char *strike, const char *island, const char *zone) {
    char buf[200];

    (void)snprintf(buf, sizeof buf, "{\"strike_in_m\":%s,\"island_m\":%s,\"sensor_zone_m\":%s}", strike,
                   island, zone);
    return crossing_text(buf);
}

static int scenario_text(const char *s) {
    err[0] = '\0';
    if (load_json(s) != LG_JSON_OK) {
        return -2;
    }
    return lg_scenario_from_json(&doc, &cfg, &sc, err, sizeof err);
}

static void default_cfg(void) {
    cfg.strike_in_mm = 400000;
    cfg.island_mm = 12000;
    cfg.sensor_zone_mm = 2000;
}

/* A one-train scenario where one field is replaced. */
static int one_train(const char *id, const char *dir, const char *t, const char *speed, const char *len,
                     const char *axles) {
    char buf[400];

    (void)snprintf(buf, sizeof buf,
                   "{\"name\":\"t\",\"trains\":[{\"id\":%s,\"dir\":%s,\"t_strike_s\":%s,\"speed_mps\":%s,"
                   "\"length_m\":%s,\"axles\":%s}]}",
                   id, dir, t, speed, len, axles);
    return scenario_text(buf);
}

#define OK_TRAIN "1", "\"west\"", "10", "20", "100", "8"

static void test_crossing_boundaries(void) {
    CHECK(crossing_vals("50", "12", "2") == 0 && cfg.strike_in_mm == 50000);
    CHECK(crossing_vals("49.999", "12", "2") == -1);
    CHECK(crossing_vals("5000", "12", "2") == 0 && cfg.strike_in_mm == 5000000);
    CHECK(crossing_vals("5000.001", "12", "2") == -1);
    CHECK(crossing_vals("49", "12", "2") == -1);
    CHECK(crossing_vals("0", "12", "2") == -1);
    CHECK(crossing_vals("-400", "12", "2") == -1);

    CHECK(crossing_vals("400", "1", "2") == 0 && cfg.island_mm == 1000);
    CHECK(crossing_vals("400", "0.999", "2") == -1);
    CHECK(crossing_vals("400", "200", "2") == 0 && cfg.island_mm == 200000);
    CHECK(crossing_vals("400", "200.001", "2") == -1);
    CHECK(crossing_vals("400", "0", "2") == -1);
    CHECK(crossing_vals("400", "-1", "2") == -1);

    CHECK(crossing_vals("400", "12", "1") == 0 && cfg.sensor_zone_mm == 1000);
    CHECK(crossing_vals("400", "12", "0.999") == -1);
    CHECK(crossing_vals("400", "12", "20") == 0 && cfg.sensor_zone_mm == 20000);
    CHECK(crossing_vals("400", "12", "20.001") == -1);
    CHECK(crossing_vals("400", "12", "0") == -1);
    CHECK(crossing_vals("50", "12", "20") == 0); /* the closest legal strike-in and widest zone */

    /* Fractions are kept in mm. */
    CHECK(crossing_vals("400.125", "12.5", "2.25") == 0);
    CHECK(cfg.strike_in_mm == 400125 && cfg.island_mm == 12500 && cfg.sensor_zone_mm == 2250);

    /* Wrong shapes and types. */
    CHECK(crossing_text("[]") == -1);
    CHECK(crossing_text("null") == -1);
    CHECK(crossing_text("7") == -1);
    CHECK(crossing_text("{}") == -1);
    CHECK(crossing_text("{\"strike_in_m\":\"400\",\"island_m\":12,\"sensor_zone_m\":2}") == -1);
    CHECK(crossing_text("{\"strike_in_m\":400,\"island_m\":true,\"sensor_zone_m\":2}") == -1);
    CHECK(crossing_text("{\"strike_in_m\":400,\"island_m\":12}") == -1);
    CHECK(strstr(err, "sensor_zone_m") != NULL);
    CHECK(crossing_text("{\"strike_in_m\":null,\"island_m\":12,\"sensor_zone_m\":2}") == -1);
    CHECK(crossing_text("{\"strike_in_m\":[400],\"island_m\":12,\"sensor_zone_m\":2}") == -1);
    /* Unknown members are ignored, not an error. */
    CHECK(crossing_text("{\"strike_in_m\":400,\"island_m\":12,\"sensor_zone_m\":2,\"extra\":1}") == 0);

    /* No error buffer: still returns -1 and does not crash. */
    CHECK(load_json("{}") == 0);
    CHECK(lg_crossing_from_json(&doc, &cfg, NULL, 0) == -1);
    err[0] = 'Z';
    CHECK(lg_crossing_from_json(&doc, &cfg, err, 0) == -1 && err[0] == 'Z');
    /* A tiny buffer is always terminated. */
    {
        char tiny[4] = {'x', 'x', 'x', 'x'};

        CHECK(lg_crossing_from_json(&doc, &cfg, tiny, sizeof tiny) == -1);
        CHECK(memchr(tiny, '\0', sizeof tiny) != NULL);
    }
    /* Nothing parsed (a failed parse): the root is NULL and the loader refuses. */
    CHECK(load_json("{") != LG_JSON_OK);
    CHECK(lg_crossing_from_json(&doc, &cfg, err, sizeof err) == -1);
}

static void test_train_boundaries(void) {
    default_cfg();
    CHECK(one_train(OK_TRAIN) == 0);

    /* id: 1..65535, whole */
    CHECK(one_train("0", "\"west\"", "10", "20", "100", "8") == -1);
    CHECK(one_train("0.999", "\"west\"", "10", "20", "100", "8") == -1);
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "8") == 0 && sc.trains[0].id == 1);
    CHECK(one_train("1.000", "\"west\"", "10", "20", "100", "8") == 0 && sc.trains[0].id == 1);
    CHECK(one_train("1.5", "\"west\"", "10", "20", "100", "8") == -1);
    CHECK(strstr(err, "whole") != NULL);
    CHECK(one_train("1.001", "\"west\"", "10", "20", "100", "8") == -1);
    CHECK(one_train("65535", "\"west\"", "10", "20", "100", "8") == 0 && sc.trains[0].id == 65535);
    CHECK(one_train("65535.001", "\"west\"", "10", "20", "100", "8") == -1);
    CHECK(one_train("65536", "\"west\"", "10", "20", "100", "8") == -1);
    CHECK(one_train("-1", "\"west\"", "10", "20", "100", "8") == -1);
    CHECK(one_train("\"1\"", "\"west\"", "10", "20", "100", "8") == -1);

    /* t_strike_s: 0..86400 */
    CHECK(one_train("1", "\"west\"", "0", "20", "100", "8") == 0 && sc.trains[0].t_strike_ms == 0);
    CHECK(one_train("1", "\"west\"", "-0.001", "20", "100", "8") == -1);
    CHECK(one_train("1", "\"west\"", "0.001", "20", "100", "8") == 0 && sc.trains[0].t_strike_ms == 1);
    CHECK(one_train("1", "\"west\"", "86400", "20", "100", "8") == 0 &&
          sc.trains[0].t_strike_ms == 86400000u);
    CHECK(one_train("1", "\"west\"", "86400.001", "20", "100", "8") == -1);
    CHECK(one_train("1", "\"west\"", "1.5", "20", "100", "8") == 0 && sc.trains[0].t_strike_ms == 1500);

    /* speed_mps: 1..100 */
    CHECK(one_train("1", "\"west\"", "10", "0.999", "100", "8") == -1);
    CHECK(one_train("1", "\"west\"", "10", "1", "100", "8") == 0 && sc.trains[0].speed_mmps == 1000);
    CHECK(one_train("1", "\"west\"", "10", "100", "100", "8") == 0 && sc.trains[0].speed_mmps == 100000);
    CHECK(one_train("1", "\"west\"", "10", "100.001", "100", "8") == -1);
    CHECK(one_train("1", "\"west\"", "10", "0", "100", "8") == -1);
    CHECK(one_train("1", "\"west\"", "10", "-20", "100", "8") == -1);
    CHECK(one_train("1", "\"west\"", "10", "20.5", "100", "8") == 0 && sc.trains[0].speed_mmps == 20500);

    /* length_m: 5..1000 */
    CHECK(one_train("1", "\"west\"", "10", "20", "4.999", "8") == -1);
    CHECK(one_train("1", "\"west\"", "10", "20", "5", "8") == 0 && sc.trains[0].length_mm == 5000);
    CHECK(one_train("1", "\"west\"", "10", "20", "1000", "8") == 0 && sc.trains[0].length_mm == 1000000);
    CHECK(one_train("1", "\"west\"", "10", "20", "1000.001", "8") == -1);
    CHECK(one_train("1", "\"west\"", "10", "20", "0", "8") == -1);
    CHECK(one_train("1", "\"west\"", "10", "20", "5.5", "8") == 0 && sc.trains[0].length_mm == 5500);

    /* axles: 1..200, whole */
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "0") == -1);
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "0.999") == -1);
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "1") == 0 && sc.trains[0].axles == 1);
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "8.000") == 0 && sc.trains[0].axles == 8);
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "8.5") == -1);
    CHECK(strstr(err, "whole") != NULL);
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "8.001") == -1);
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "200") == 0 && sc.trains[0].axles == 200);
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "200.001") == -1);
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "201") == -1);
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "256") == -1);
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "257") == -1); /* would wrap to 1 in uint8 */
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "-8") == -1);

    /* dir */
    CHECK(one_train("1", "\"east\"", "10", "20", "100", "8") == 0 && sc.trains[0].dir == LG_DIR_EAST);
    CHECK(one_train("1", "\"west\"", "10", "20", "100", "8") == 0 && sc.trains[0].dir == LG_DIR_WEST);
    CHECK(one_train("1", "\"West\"", "10", "20", "100", "8") == -1);
    CHECK(one_train("1", "\"west \"", "10", "20", "100", "8") == -1);
    CHECK(one_train("1", "\"\"", "10", "20", "100", "8") == -1);
    CHECK(one_train("1", "\"north\"", "10", "20", "100", "8") == -1);
    CHECK(one_train("1", "0", "10", "20", "100", "8") == -1);
    CHECK(one_train("1", "null", "10", "20", "100", "8") == -1);
}

static void test_scenario_shape(void) {
    char name[80];
    char buf[300];
    int i;

    default_cfg();
    CHECK(scenario_text("{\"name\":\"x\",\"trains\":[]}") == 0 && sc.n_trains == 0);
    CHECK(scenario_text("[]") == -1);
    CHECK(scenario_text("{\"trains\":[]}") == -1);
    CHECK(scenario_text("{\"name\":\"\",\"trains\":[]}") == -1);
    CHECK(scenario_text("{\"name\":5,\"trains\":[]}") == -1);
    CHECK(scenario_text("{\"name\":\"x\"}") == -1);
    CHECK(scenario_text("{\"name\":\"x\",\"trains\":{}}") == -1);
    CHECK(scenario_text("{\"name\":\"x\",\"trains\":null}") == -1);
    CHECK(scenario_text("{\"name\":\"x\",\"trains\":[1]}") == -1);
    CHECK(scenario_text("{\"name\":\"x\",\"trains\":[[]]}") == -1);
    CHECK(scenario_text("{\"name\":\"x\",\"trains\":[{}]}") == -1);
    CHECK(sc.n_trains == 0); /* a failed load never leaves trains behind */

    /* Name of 31 bytes fits (32 with the NUL), 32 does not. */
    for (i = 0; i < 2; i++) {
        int len = 31 + i;

        memset(name, 'n', (size_t)len);
        name[len] = '\0';
        (void)snprintf(buf, sizeof buf, "{\"name\":\"%s\",\"trains\":[]}", name);
        CHECK(scenario_text(buf) == (i == 0 ? 0 : -1));
        if (i == 0) {
            CHECK(strlen(sc.name) == 31 && sc.name[31] == '\0');
        }
    }
    CHECK(scenario_text("{\"name\":\"a\\nb\",\"trains\":[]}") == 0 && strcmp(sc.name, "a\nb") == 0);

    /* Failure in the middle: the whole scenario is wiped (nothing half loaded). */
    CHECK(scenario_text("{\"name\":\"x\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":10,"
                        "\"speed_mps\":20,\"length_m\":100,\"axles\":8},{\"id\":2}]}") == -1);
    CHECK(sc.n_trains == 0);

    /* Duplicate ids, also across directions. */
    CHECK(scenario_text("{\"name\":\"x\",\"trains\":["
                        "{\"id\":7,\"dir\":\"west\",\"t_strike_s\":10,\"speed_mps\":20,\"length_m\":100,\"axles\":8},"
                        "{\"id\":7,\"dir\":\"east\",\"t_strike_s\":10,\"speed_mps\":20,\"length_m\":100,\"axles\":8}]}") == -1);
    CHECK(strstr(err, "duplicate") != NULL);
    CHECK(scenario_text("{\"name\":\"x\",\"trains\":["
                        "{\"id\":7.000,\"dir\":\"west\",\"t_strike_s\":10,\"speed_mps\":20,\"length_m\":100,\"axles\":8},"
                        "{\"id\":7,\"dir\":\"east\",\"t_strike_s\":10,\"speed_mps\":20,\"length_m\":100,\"axles\":8}]}") == -1);
}

static size_t put_train(char *dst, int id, const char *dir, int t_s) {
    return (size_t)sprintf(dst, "{\"id\":%d,\"dir\":\"%s\",\"t_strike_s\":%d,\"speed_mps\":20,"
                                "\"length_m\":100,\"axles\":8}",
                           id, dir, t_s);
}

static int many_trains(int count, int spacing_s) {
    size_t n = 0;
    int i;

    n += (size_t)sprintf(big + n, "{\"name\":\"m\",\"trains\":[");
    for (i = 0; i < count; i++) {
        if (i > 0) {
            big[n++] = ',';
        }
        n += put_train(big + n, i + 1, "west", 10 + i * spacing_s);
    }
    n += (size_t)sprintf(big + n, "]}");
    return scenario_text(big);
}

static void test_train_counts(void) {
    default_cfg();
    /* 64 trains fit (spaced out so they do not overlap): 8 nodes per train plus 3 = 515 nodes. */
    CHECK(many_trains(63, 100) == 0 && sc.n_trains == 63);
    CHECK(many_trains(64, 100) == 0 && sc.n_trains == 64);
    CHECK(sc.trains[63].id == 64 && sc.trains[63].t_strike_ms == (10u + 63u * 100u) * 1000u);
    CHECK(many_trains(65, 100) == -1);
    CHECK(strstr(err, "too many") != NULL);
    CHECK(sc.n_trains == 0);
    /* 64 trains 1 s apart overlap: refused as overlap, not as a count problem. */
    CHECK(many_trains(64, 1) == -1);
    CHECK(strstr(err, "overlap") != NULL);
}

/* ---- overlap boundaries --------------------------------------------------------------------- */

static int two_trains(const char *dir1, long t1_ms, long v1_mmps, long len1_mm, const char *dir2,
                      long t2_ms, long v2_mmps, long len2_mm) {
    char buf[600];

    /* Values in milli units go in as decimals: ms -> s with 3 decimals, mm/s -> m/s, mm -> m. */
    (void)snprintf(buf, sizeof buf,
                   "{\"name\":\"o\",\"trains\":["
                   "{\"id\":1,\"dir\":\"%s\",\"t_strike_s\":%ld.%03ld,\"speed_mps\":%ld.%03ld,\"length_m\":%ld.%03ld,\"axles\":4},"
                   "{\"id\":2,\"dir\":\"%s\",\"t_strike_s\":%ld.%03ld,\"speed_mps\":%ld.%03ld,\"length_m\":%ld.%03ld,\"axles\":4}]}",
                   dir1, t1_ms / 1000, t1_ms % 1000, v1_mmps / 1000, v1_mmps % 1000, len1_mm / 1000,
                   len1_mm % 1000, dir2, t2_ms / 1000, t2_ms % 1000, v2_mmps / 1000, v2_mmps % 1000,
                   len2_mm / 1000, len2_mm % 1000);
    return scenario_text(buf);
}

static void test_overlap_boundaries(void) {
    default_cfg(); /* strike-in 400 m, island 12 m: the island exit is at 412 m */

    /* Same speed 1 m/s (= 1 mm per ms), leader 5 m long: the gap is dt - 5000 mm. */
    CHECK(two_trains("west", 10000, 1000, 5000, "west", 15000, 1000, 5000) == 0);  /* gap 0 */
    CHECK(two_trains("west", 10000, 1000, 5000, "west", 14999, 1000, 5000) == -1); /* gap -1 mm */
    CHECK(two_trains("west", 10000, 1000, 5000, "west", 15001, 1000, 5000) == 0);  /* gap +1 mm */
    CHECK(two_trains("west", 10000, 1000, 5000, "west", 10000, 1000, 5000) == -1);
    /* Same numbers, the other direction and mixed directions never overlap. */
    CHECK(two_trains("east", 10000, 1000, 5000, "east", 14999, 1000, 5000) == -1);
    CHECK(two_trains("east", 10000, 1000, 5000, "west", 10000, 1000, 5000) == 0);
    CHECK(two_trains("west", 10000, 1000, 5000, "east", 14999, 1000, 5000) == 0);
    /* The order in the file does not matter: the earlier strike-in leads. */
    CHECK(two_trains("west", 14999, 1000, 5000, "west", 10000, 1000, 5000) == -1);
    CHECK(two_trains("west", 15000, 1000, 5000, "west", 10000, 1000, 5000) == 0);
    CHECK(two_trains("west", 15001, 1000, 5000, "west", 10000, 1000, 5000) == 0);

    /* Follower faster: it must not reach the leader's rear before the leader leaves the island.
     * Leader 1 m/s, 5 m long: gone at t + (412000 + 5000) ms. Follower 2 m/s striking dt later has
     * rear gap at that moment 412000 - 2 * (417000 - dt) mm; zero for dt = 211000 ms. */
    CHECK(two_trains("west", 0, 1000, 5000, "west", 211000, 2000, 5000) == 0);
    CHECK(two_trains("west", 0, 1000, 5000, "west", 210999, 2000, 5000) == -1);
    CHECK(two_trains("west", 0, 1000, 5000, "west", 211001, 2000, 5000) == 0);
    /* Follower slower: only the moment it appears matters. */
    CHECK(two_trains("west", 0, 100000, 5000, "west", 50, 1000, 5000) == 0);  /* 5 m/50 ms: 5000 mm */
    CHECK(two_trains("west", 0, 100000, 5000, "west", 49, 1000, 5000) == -1); /* gap -100 mm */
    /* Follower appears exactly when the leader is gone: 417000 ms for the 1 m/s leader. */
    CHECK(two_trains("west", 0, 1000, 5000, "west", 417000, 100000, 5000) == 0);
    CHECK(two_trains("west", 0, 1000, 5000, "west", 416999, 100000, 5000) == 0); /* 100 mm in */
    CHECK(two_trains("west", 0, 1000, 5000, "west", 417001, 100000, 5000) == 0);
    /* A 100 m/s follower (100 mm per ms) reaches the leader's rear at the exit for dt = 412880. */
    CHECK(two_trains("west", 0, 1000, 5000, "west", 412880, 100000, 5000) == 0);
    CHECK(two_trains("west", 0, 1000, 5000, "west", 412879, 100000, 5000) == -1);
    CHECK(two_trains("west", 0, 1000, 5000, "west", 412881, 100000, 5000) == 0);
}

static void test_overlap_overflow_shapes(void) {
    cfg.strike_in_mm = 5000000; /* 5 km */
    cfg.island_mm = 200000;
    cfg.sensor_zone_mm = 20000;
    /* Slowest 1 km train, latest strike time, fastest follower right behind. */
    CHECK(two_trains("west", 86300000, 1000, 1000000, "west", 86400000, 100000, 5000) == -1);
    CHECK(two_trains("west", 0, 1000, 1000000, "west", 86400000, 100000, 5000) == 0);
    CHECK(two_trains("west", 0, 1000, 1000000, "west", 86400000, 100000, 1000000) == 0);
    CHECK(two_trains("west", 0, 100000, 1000000, "west", 86400000, 1000, 5000) == 0);
    /* Slow leader, fast follower that catches it inside the island long after striking in. */
    CHECK(two_trains("west", 0, 1000, 5000, "west", 2000000, 100000, 5000) == -1);
    CHECK(strstr(err, "overlap") != NULL);
    /* Both at the extremes in the same direction at the same time. */
    CHECK(two_trains("west", 86400000, 100000, 1000000, "west", 86400000, 100000, 1000000) == -1);
    CHECK(two_trains("west", 86400000, 1000, 1000000, "west", 86400000, 100000, 5000) == -1);
    /* A leader of 1 km at 100 m/s is clear of the strike-in point after 10 s. */
    CHECK(two_trains("west", 0, 100000, 1000000, "west", 10000, 100000, 1000000) == 0);
    CHECK(two_trains("west", 0, 100000, 1000000, "west", 9999, 100000, 1000000) == -1);
    default_cfg();
}

/* ---- brute force oracle for the overlap rule ------------------------------------------------- */

static uint32_t rng_state = 0x12345679u;

static uint32_t rnd(void) {
    uint32_t x = rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/*
 * Smallest gap in mm (rear of the leader minus front of the follower, in the path coordinate of
 * the shared track) over the time both are on the track, found by scanning. A negative value means
 * the follower would run into the leader. Time steps of 20 ms.
 */
static double min_gap(const lg_crossing_cfg_t *c, long tl, long vl, long ll, long tf, long vf) {
    double exit_mm = (double)c->strike_in_mm + (double)c->island_mm;
    double t_gone = (double)tl + (exit_mm + (double)ll) * 1000.0 / (double)vl;
    double best = 1e18;
    double t;

    if ((double)tf >= t_gone) {
        return 1e18;
    }
    for (t = (double)tf; t <= t_gone + 1.0; t += 20.0) {
        double tt = t > t_gone ? t_gone : t;
        double rear_l = (double)vl * (tt - (double)tl) / 1000.0 - (double)ll;
        double front_f = (double)vf * (tt - (double)tf) / 1000.0;

        if (rear_l - front_f < best) {
            best = rear_l - front_f;
        }
        if (tt >= t_gone) {
            break;
        }
    }
    return best;
}

static void test_overlap_oracle(void) {
    int i;
    int disagree = 0;
    int refused = 0;
    int accepted = 0;

    default_cfg();
    for (i = 0; i < 6000; i++) {
        long vl = 1000 + (long)(rnd() % 40) * 1000;
        long vf = 1000 + (long)(rnd() % 40) * 1000;
        long ll = 5000 + (long)(rnd() % 300) * 1000;
        long lf = 5000 + (long)(rnd() % 300) * 1000;
        long tl = (long)(rnd() % 20) * 1000;
        long tf = tl + (long)(rnd() % 250000);
        int swap = (int)(rnd() & 1u);
        double gap = min_gap(&cfg, tl, vl, ll, tf, vf);
        int rc;

        if (swap) {
            rc = two_trains("west", tf, vf, lf, "west", tl, vl, ll);
        } else {
            rc = two_trains("west", tl, vl, ll, "west", tf, vf, lf);
        }
        if (rc == 0) {
            accepted++;
        } else {
            refused++;
        }
        /* Ignore the cases within 120 mm of touching (scan step and mm truncation). */
        if (gap > 120.0 && rc != 0) {
            disagree++;
            printf("refused but clear (gap %.0f mm): lead t=%ld v=%ld l=%ld, follow t=%ld v=%ld\n", gap, tl,
                   vl, ll, tf, vf);
        }
        if (gap < -120.0 && rc == 0) {
            disagree++;
            printf("accepted but overlapping (gap %.0f mm): lead t=%ld v=%ld l=%ld, follow t=%ld v=%ld\n",
                   gap, tl, vl, ll, tf, vf);
        }
    }
    CHECK(disagree == 0);
    CHECK(accepted > 300 && refused > 300);
}

/* ---- lg_read_file --------------------------------------------------------------------------- */

static int write_tmp(const char *path, const char *data, size_t n) {
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        return -1;
    }
    if (n > 0 && fwrite(data, 1, n, f) != n) {
        (void)fclose(f);
        return -1;
    }
    return fclose(f);
}

static void test_read_file(void) {
    static const char *const p = "tmp_test_scenario_a.bin";
    char buf[16];
    size_t len = 99;
    size_t i;

    /* Empty file. */
    CHECK(write_tmp(p, "", 0) == 0);
    memset(buf, 'x', sizeof buf);
    CHECK(lg_read_file(p, buf, 10, &len) == 0 && len == 0 && buf[0] == '\0');
    memset(buf, 'x', sizeof buf);
    CHECK(lg_read_file(p, buf, 1, &len) == 0 && len == 0 && buf[0] == '\0');

    /* cap - 1 bytes fit, cap bytes do not. */
    CHECK(write_tmp(p, "123456789", 9) == 0);
    memset(buf, 'x', sizeof buf);
    len = 99;
    CHECK(lg_read_file(p, buf, 10, &len) == 0 && len == 9 && strcmp(buf, "123456789") == 0);
    CHECK(buf[10] == 'x'); /* nothing written past cap */
    CHECK(write_tmp(p, "1234567890", 10) == 0);
    memset(buf, 'x', sizeof buf);
    len = 99;
    CHECK(lg_read_file(p, buf, 10, &len) == -1 && len == 99);
    CHECK(buf[10] == 'x' && buf[11] == 'x');
    CHECK(lg_read_file(p, buf, 11, &len) == 0 && len == 10 && buf[10] == '\0');
    CHECK(write_tmp(p, "12345678901", 11) == 0);
    len = 99;
    CHECK(lg_read_file(p, buf, 10, &len) == -1 && len == 99);
    CHECK(lg_read_file(p, buf, 11, &len) == -1);
    CHECK(lg_read_file(p, buf, 12, &len) == 0 && len == 11);

    /* cap 1 holds only the empty file. */
    CHECK(write_tmp(p, "a", 1) == 0);
    CHECK(lg_read_file(p, buf, 1, &len) == -1);
    CHECK(lg_read_file(p, buf, 2, &len) == 0 && len == 1 && buf[0] == 'a' && buf[1] == '\0');

    /* Binary content (NUL bytes, CR LF, high bytes) is returned untouched. */
    {
        static const char bin[] = {'a', '\0', 'b', '\r', '\n', (char)0xFF, '\x1A', 'z'};

        CHECK(write_tmp(p, bin, sizeof bin) == 0);
        memset(buf, 'x', sizeof buf);
        CHECK(lg_read_file(p, buf, sizeof buf, &len) == 0 && len == sizeof bin);
        CHECK(memcmp(buf, bin, sizeof bin) == 0 && buf[sizeof bin] == '\0');
    }

    /* Bad arguments and a missing file. */
    CHECK(lg_read_file(p, buf, 0, &len) == -1);
    CHECK(lg_read_file(NULL, buf, 10, &len) == -1);
    CHECK(lg_read_file(p, NULL, 10, &len) == -1);
    CHECK(lg_read_file(p, buf, 10, NULL) == -1);
    CHECK(lg_read_file("tmp_test_scenario_missing.bin", buf, 10, &len) == -1);
    CHECK(lg_read_file("", buf, 10, &len) == -1);
    CHECK(lg_read_file(".", buf, 10, &len) == -1 || len == 0); /* a directory is no file */

    /* A larger file at the exact cap, read twice (the file is closed each time). */
    {
        static char content[5000];
        static char out[5001];

        for (i = 0; i < sizeof content; i++) {
            content[i] = (char)('a' + (i % 26));
        }
        CHECK(write_tmp(p, content, sizeof content) == 0);
        CHECK(lg_read_file(p, out, sizeof out, &len) == 0 && len == sizeof content);
        CHECK(memcmp(out, content, len) == 0);
        CHECK(lg_read_file(p, out, sizeof out - 1, &len) == -1);
        CHECK(lg_read_file(p, out, sizeof out, &len) == 0);
    }
    (void)remove(p);
}

int main(void) {
    test_crossing_boundaries();
    test_train_boundaries();
    test_scenario_shape();
    test_train_counts();
    test_overlap_boundaries();
    test_overlap_overflow_shapes();
    test_overlap_oracle();
    test_read_file();
    LG_TEST_DONE("test_scenario_extra");
}
