#include <stdio.h>
#include <string.h>

#include "lg_scenario.h"
#include "lg_test.h"

static lg_json_doc_t doc;
static lg_crossing_cfg_t cfg;
static lg_scenario_t sc;
static char err[LG_ERR_LEN];

static int load_json(const char *s) {
    return lg_json_parse(&doc, s, strlen(s));
}

static int crossing(const char *s) {
    return load_json(s) == LG_JSON_OK ? lg_crossing_from_json(&doc, &cfg, err, sizeof err) : -2;
}

static int scenario(const char *s) {
    return load_json(s) == LG_JSON_OK ? lg_scenario_from_json(&doc, &cfg, &sc, err, sizeof err) : -2;
}

static int load_file(const char *path) {
    static char buf[16384];
    size_t len = 0;

    if (lg_read_file(path, buf, sizeof buf, &len) != 0) {
        return -3;
    }
    return lg_json_parse(&doc, buf, len);
}

static void use_default_crossing(void) {
    cfg.strike_in_mm = 400000;
    cfg.island_mm = 12000;
    cfg.sensor_zone_mm = 2000;
}

/* A one-train scenario with a few fields replaced, to test one rule at a time. */
static int train_with(const char *field_override) {
    char text[512];

    (void)snprintf(text, sizeof text,
                   "{\"name\":\"t\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":5,"
                   "\"speed_mps\":30,\"length_m\":120,\"axles\":8%s}]}",
                   field_override);
    return scenario(text);
}

static void test_repo_files(void) {
    CHECK(load_file("config/crossing.json") == LG_JSON_OK);
    CHECK(lg_crossing_from_json(&doc, &cfg, err, sizeof err) == 0);
    CHECK(cfg.strike_in_mm == 400000 && cfg.island_mm == 12000 && cfg.sensor_zone_mm == 2000);

    CHECK(load_file("scenarios/test_short.json") == LG_JSON_OK);
    CHECK(lg_scenario_from_json(&doc, &cfg, &sc, err, sizeof err) == 0);
    CHECK(strcmp(sc.name, "test_short") == 0 && sc.n_trains == 2);
    CHECK(sc.trains[0].id == 1 && sc.trains[0].dir == LG_DIR_WEST && sc.trains[0].t_strike_ms == 5000);
    CHECK(sc.trains[0].speed_mmps == 30000 && sc.trains[0].length_mm == 120000 && sc.trains[0].axles == 8);
    CHECK(sc.trains[1].id == 2 && sc.trains[1].dir == LG_DIR_EAST && sc.trains[1].axles == 6);

    CHECK(load_file("scenarios/normal_day.json") == LG_JSON_OK);
    CHECK(lg_scenario_from_json(&doc, &cfg, &sc, err, sizeof err) == 0);
    CHECK(strcmp(sc.name, "normal_day") == 0 && sc.n_trains == 6);
    CHECK(sc.trains[5].t_strike_ms == 250000 && sc.trains[5].dir == LG_DIR_EAST);

    CHECK(load_file("scenarios/does_not_exist.json") == -3);
}

static void test_crossing_validation(void) {
    CHECK(crossing("{\"strike_in_m\":400,\"island_m\":12,\"sensor_zone_m\":2}") == 0);
    CHECK(cfg.strike_in_mm == 400000);
    CHECK(crossing("{\"strike_in_m\":400.5,\"island_m\":12.25,\"sensor_zone_m\":2}") == 0);
    CHECK(cfg.strike_in_mm == 400500 && cfg.island_mm == 12250);

    CHECK(crossing("[1]") == -1);
    CHECK(crossing("{\"island_m\":12,\"sensor_zone_m\":2}") == -1); /* missing */
    CHECK(strstr(err, "strike_in_m") != NULL);
    CHECK(crossing("{\"strike_in_m\":\"400\",\"island_m\":12,\"sensor_zone_m\":2}") == -1); /* a string */
    CHECK(crossing("{\"strike_in_m\":49,\"island_m\":12,\"sensor_zone_m\":2}") == -1);
    CHECK(crossing("{\"strike_in_m\":5001,\"island_m\":12,\"sensor_zone_m\":2}") == -1);
    CHECK(crossing("{\"strike_in_m\":400,\"island_m\":0,\"sensor_zone_m\":2}") == -1);
    CHECK(crossing("{\"strike_in_m\":400,\"island_m\":201,\"sensor_zone_m\":2}") == -1);
    CHECK(crossing("{\"strike_in_m\":400,\"island_m\":12,\"sensor_zone_m\":0}") == -1);
    CHECK(crossing("{\"strike_in_m\":400,\"island_m\":12,\"sensor_zone_m\":21}") == -1);
    CHECK(crossing("{\"strike_in_m\":50,\"island_m\":12,\"sensor_zone_m\":20}") == 0);
    CHECK(crossing("{\"strike_in_m\":10,\"island_m\":12,\"sensor_zone_m\":2}") == -1);
    CHECK(crossing("not json") == -2);
}

static void test_scenario_validation(void) {
    use_default_crossing();

    CHECK(train_with("") == 0 && sc.n_trains == 1);

    /* Field by field: replace one by an invalid value (a later key overrides? no: duplicates are refused). */
    CHECK(scenario("{\"name\":\"t\",\"trains\":[]}") == 0 && sc.n_trains == 0);
    CHECK(scenario("{\"trains\":[]}") == -1);                       /* no name */
    CHECK(scenario("{\"name\":\"\",\"trains\":[]}") == -1);         /* empty name */
    CHECK(scenario("{\"name\":\"0123456789012345678901234567890123\",\"trains\":[]}") == -1);
    CHECK(scenario("{\"name\":\"t\"}") == -1);                      /* no trains */
    CHECK(scenario("{\"name\":\"t\",\"trains\":{}}") == -1);        /* not an array */
    CHECK(scenario("{\"name\":\"t\",\"trains\":[1]}") == -1);       /* not objects */
    CHECK(scenario("[]") == -1);

    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1,\"dir\":\"north\",\"t_strike_s\":5,"
                   "\"speed_mps\":30,\"length_m\":120,\"axles\":8}]}") == -1);
    CHECK(strstr(err, "west or east") != NULL);
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1,\"t_strike_s\":5,\"speed_mps\":30,"
                   "\"length_m\":120,\"axles\":8}]}") == -1); /* no dir */
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":0,\"dir\":\"west\",\"t_strike_s\":5,"
                   "\"speed_mps\":30,\"length_m\":120,\"axles\":8}]}") == -1);
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1.5,\"dir\":\"west\",\"t_strike_s\":5,"
                   "\"speed_mps\":30,\"length_m\":120,\"axles\":8}]}") == -1);
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":-1,"
                   "\"speed_mps\":30,\"length_m\":120,\"axles\":8}]}") == -1);
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":86401,"
                   "\"speed_mps\":30,\"length_m\":120,\"axles\":8}]}") == -1);
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":5,"
                   "\"speed_mps\":0,\"length_m\":120,\"axles\":8}]}") == -1);
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":5,"
                   "\"speed_mps\":101,\"length_m\":120,\"axles\":8}]}") == -1);
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":5,"
                   "\"speed_mps\":30,\"length_m\":4,\"axles\":8}]}") == -1);
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":5,"
                   "\"speed_mps\":30,\"length_m\":1001,\"axles\":8}]}") == -1);
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":5,"
                   "\"speed_mps\":30,\"length_m\":120,\"axles\":0}]}") == -1);
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":5,"
                   "\"speed_mps\":30,\"length_m\":120,\"axles\":201}]}") == -1);
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":5,"
                   "\"speed_mps\":30,\"length_m\":120,\"axles\":8.5}]}") == -1);
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":5,"
                   "\"speed_mps\":30,\"length_m\":120,\"axles\":\"8\"}]}") == -1);

    /* Limits that are valid. */
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":65535,\"dir\":\"east\",\"t_strike_s\":86400,"
                   "\"speed_mps\":100,\"length_m\":1000,\"axles\":200}]}") == 0);
    CHECK(sc.trains[0].id == 65535 && sc.trains[0].axles == 200 && sc.trains[0].t_strike_ms == 86400000u);
    CHECK(scenario("{\"name\":\"t\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":0.5,"
                   "\"speed_mps\":1,\"length_m\":5,\"axles\":1}]}") == 0);
    CHECK(sc.trains[0].t_strike_ms == 500 && sc.trains[0].speed_mmps == 1000 && sc.trains[0].length_mm == 5000);

    /* Duplicate id. */
    CHECK(scenario("{\"name\":\"t\",\"trains\":["
                   "{\"id\":1,\"dir\":\"west\",\"t_strike_s\":5,\"speed_mps\":30,\"length_m\":120,\"axles\":8},"
                   "{\"id\":1,\"dir\":\"east\",\"t_strike_s\":50,\"speed_mps\":30,\"length_m\":120,\"axles\":8}]}") == -1);
    CHECK(strstr(err, "duplicate") != NULL);
}

static void test_too_many_trains(void) {
    static char text[8192];
    size_t n = 0;
    uint32_t i;

    use_default_crossing();
    /* Exactly the limit: 64 trains, 200 s apart (far apart, no overlap). */
    n += (size_t)snprintf(text + n, sizeof text - n, "{\"name\":\"many\",\"trains\":[");
    for (i = 0; i < LG_SCENARIO_MAX_TRAINS; i++) {
        n += (size_t)snprintf(text + n, sizeof text - n,
                              "%s{\"id\":%u,\"dir\":\"west\",\"t_strike_s\":%u,\"speed_mps\":30,"
                              "\"length_m\":100,\"axles\":4}",
                              i ? "," : "", (unsigned)(i + 1), (unsigned)(i * 200));
    }
    n += (size_t)snprintf(text + n, sizeof text - n, "]}");
    CHECK(load_json(text) == LG_JSON_OK);
    CHECK(lg_scenario_from_json(&doc, &cfg, &sc, err, sizeof err) == 0 && sc.n_trains == 64);

    n = 0;
    n += (size_t)snprintf(text + n, sizeof text - n, "{\"name\":\"many\",\"trains\":[");
    for (i = 0; i < LG_SCENARIO_MAX_TRAINS + 1; i++) {
        n += (size_t)snprintf(text + n, sizeof text - n,
                              "%s{\"id\":%u,\"dir\":\"west\",\"t_strike_s\":%u,\"speed_mps\":30,"
                              "\"length_m\":100,\"axles\":4}",
                              i ? "," : "", (unsigned)(i + 1), (unsigned)(i * 200));
    }
    n += (size_t)snprintf(text + n, sizeof text - n, "]}");
    CHECK(load_json(text) == LG_JSON_OK);
    CHECK(lg_scenario_from_json(&doc, &cfg, &sc, err, sizeof err) == -1);
    CHECK(strstr(err, "too many") != NULL);
}

static const char *two(const char *dir2, int t2, int v2, int len2) {
    static char text[640];

    (void)snprintf(text, sizeof text,
                   "{\"name\":\"t\",\"trains\":["
                   "{\"id\":1,\"dir\":\"west\",\"t_strike_s\":10,\"speed_mps\":20,\"length_m\":100,\"axles\":4},"
                   "{\"id\":2,\"dir\":\"%s\",\"t_strike_s\":%d,\"speed_mps\":%d,\"length_m\":%d,\"axles\":4}]}",
                   dir2, t2, v2, len2);
    return text;
}

static void test_overlap(void) {
    /*
     * Train 1: west, strikes in at 10 s, 20 m/s, 100 m long. Its rear passes the strike-in point
     * at 15 s and the island exit (412 m) when its front is at 512 m: 10 + 25.6 = 35.6 s.
     */
    use_default_crossing();

    /* A follower appearing while the leader still covers the strike-in point overlaps. */
    CHECK(scenario(two("west", 12, 20, 100)) == -1);
    CHECK(strstr(err, "overlap") != NULL);
    CHECK(scenario(two("west", 14, 20, 100)) == -1);
    /* Exactly when the leader's rear has passed the point: no overlap (gap 0, same speed). */
    CHECK(scenario(two("west", 15, 20, 100)) == 0);
    CHECK(scenario(two("west", 16, 20, 100)) == 0);
    /* A faster follower that would catch the leader before it leaves the island overlaps. */
    CHECK(scenario(two("west", 16, 60, 100)) == -1);
    /* Long after the leader is gone: fine, even if fast. */
    CHECK(scenario(two("west", 40, 100, 100)) == 0);
    /* The same train ids and times in the other direction never conflict (one track each). */
    CHECK(scenario(two("east", 12, 20, 100)) == 0);
    CHECK(scenario(two("east", 10, 20, 100)) == 0);
    /* Equal times, same direction: the lower index leads, and they overlap. */
    CHECK(scenario(two("west", 10, 20, 100)) == -1);
}

int main(void) {
    use_default_crossing();
    test_repo_files();
    test_crossing_validation();
    test_scenario_validation();
    test_too_many_trains();
    test_overlap();
    LG_TEST_DONE("test_scenario");
}
