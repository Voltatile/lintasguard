/* Unit tests for core/timetable.c (Step 12): timetable file, station rule, listing, window tracker. */

#include <string.h>

#include "lg_json_mini.h"
#include "lg_test.h"
#include "lg_timetable.h"

static lg_json_doc_t g_doc;

static int load(lg_tt_t *tt, const char *text) {
    char err[128];

    if (lg_json_parse(&g_doc, text, strlen(text)) != LG_JSON_OK) {
        lg_tt_init(tt);
        return -2;
    }
    return lg_tt_from_json(&g_doc, tt, err, sizeof err);
}

static const char *k_basic = "{\"name\":\"t\",\"lead_s\":60,\"tolerance_s\":180,\"trains\":["
                             "{\"id\":1,\"dir\":\"west\",\"eta_s\":100},"
                             "{\"id\":2,\"dir\":\"east\",\"eta_s\":150},"
                             "{\"id\":3,\"dir\":\"west\",\"eta_s\":400.5},"
                             "{\"id\":4,\"dir\":\"west\",\"eta_s\":250}]}";

static void test_load_valid(void) {
    lg_tt_t tt;
    lg_tt_entry_t e[LG_SCHED_ENTRIES];
    uint32_t n;

    CHECK(load(&tt, k_basic) == 0);
    CHECK(tt.count == 4 && tt.lead_ms == 60000 && tt.tol_ms == 180000);
    CHECK(lg_tt_info(&tt, LG_DIR_WEST) && lg_tt_info(&tt, LG_DIR_EAST));
    n = lg_tt_next(&tt, LG_DIR_WEST, 0, e, LG_SCHED_ENTRIES);
    CHECK(n == 3 && e[0].id == 1 && e[1].id == 4 && e[2].id == 3);
    CHECK(e[2].eta_ms == 400500 && e[0].source == LG_SRC_TIMETABLE);
    /* Defaults when lead_s and tolerance_s are missing; a direction without trains has no information. */
    CHECK(load(&tt, "{\"trains\":[{\"id\":7,\"dir\":\"east\",\"eta_s\":0}]}") == 0);
    CHECK(tt.lead_ms == LG_WINDOW_LEAD_MS && tt.tol_ms == LG_WINDOW_TOLERANCE_MS);
    CHECK(!lg_tt_info(&tt, LG_DIR_WEST) && lg_tt_info(&tt, LG_DIR_EAST));
    CHECK(load(&tt, "{\"trains\":[]}") == 0 && !lg_tt_info(&tt, LG_DIR_WEST) && !lg_tt_info(&tt, LG_DIR_EAST));
}

static void test_load_invalid(void) {
    static const char *const bad[] = {
        "[]",
        "{}",
        "{\"trains\":{}}",
        "{\"trains\":[],\"extra\":1}",
        "{\"trains\":[{\"id\":1,\"dir\":\"west\"}]}",
        "{\"trains\":[{\"id\":1,\"dir\":\"north\",\"eta_s\":5}]}",
        "{\"trains\":[{\"id\":0,\"dir\":\"west\",\"eta_s\":5}]}",
        "{\"trains\":[{\"id\":1.5,\"dir\":\"west\",\"eta_s\":5}]}",
        "{\"trains\":[{\"id\":32768,\"dir\":\"west\",\"eta_s\":5}]}",
        "{\"trains\":[{\"id\":1,\"dir\":\"west\",\"eta_s\":-1}]}",
        "{\"trains\":[{\"id\":1,\"dir\":\"west\",\"eta_s\":86401}]}",
        "{\"trains\":[{\"id\":1,\"dir\":\"west\",\"eta_s\":5,\"axles\":8}]}",
        "{\"trains\":[{\"id\":1,\"dir\":\"west\",\"eta_s\":5},{\"id\":1,\"dir\":\"east\",\"eta_s\":9}]}",
        "{\"lead_s\":0,\"trains\":[]}",
        "{\"lead_s\":601,\"trains\":[]}",
        "{\"tolerance_s\":1801,\"trains\":[]}",
        "{\"name\":5,\"trains\":[]}",
        "{\"trains\":[5]}",
    };
    lg_tt_t tt;
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = load(&tt, bad[i]);

        CHECK(rc != 0);
        if (rc == 0) {
            printf("  accepted: %s\n", bad[i]);
        }
        /* A refused file leaves no information behind. */
        CHECK(tt.count == 0 && !lg_tt_info(&tt, LG_DIR_WEST) && !lg_tt_info(&tt, LG_DIR_EAST));
    }
}

static void test_too_many(void) {
    static char text[8192];
    lg_tt_t tt;
    size_t used;
    int i;

    used = (size_t)snprintf(text, sizeof text, "{\"trains\":[");
    for (i = 1; i <= LG_TT_MAX + 1; i++) {
        used += (size_t)snprintf(text + used, sizeof text - used, "%s{\"id\":%d,\"dir\":\"west\",\"eta_s\":%d}",
                                 i > 1 ? "," : "", i, i * 10);
    }
    (void)snprintf(text + used, sizeof text - used, "]}");
    CHECK(load(&tt, text) != 0);
}

static void test_listing_and_expiry(void) {
    lg_tt_t tt;
    lg_tt_entry_t e[LG_SCHED_ENTRIES];

    CHECK(load(&tt, k_basic) == 0);
    /* Entry 1 (eta 100 s) is listed until eta + tolerance = 280 s. */
    CHECK(lg_tt_next(&tt, LG_DIR_WEST, 280000, e, LG_SCHED_ENTRIES) == 3 && e[0].id == 1);
    CHECK(lg_tt_next(&tt, LG_DIR_WEST, 280001, e, LG_SCHED_ENTRIES) == 2 && e[0].id == 4);
    CHECK(lg_tt_next(&tt, LG_DIR_WEST, 0, e, 1) == 1 && e[0].id == 1);
    CHECK(lg_tt_next(&tt, LG_DIR_WEST, 600000, e, LG_SCHED_ENTRIES) == 0);
    CHECK(lg_tt_next(&tt, 5, 0, e, LG_SCHED_ENTRIES) == 0);
    CHECK(lg_tt_next(&tt, LG_DIR_WEST, 0, NULL, LG_SCHED_ENTRIES) == 0);
}

static void test_outage(void) {
    lg_tt_t tt;

    CHECK(load(&tt, k_basic) == 0);
    lg_tt_set_outage(&tt, 1u << LG_DIR_WEST);
    CHECK(!lg_tt_info(&tt, LG_DIR_WEST) && lg_tt_info(&tt, LG_DIR_EAST));
    lg_tt_set_outage(&tt, 3u);
    CHECK(!lg_tt_info(&tt, LG_DIR_WEST) && !lg_tt_info(&tt, LG_DIR_EAST));
    lg_tt_set_outage(&tt, 0);
    CHECK(lg_tt_info(&tt, LG_DIR_WEST) && lg_tt_info(&tt, LG_DIR_EAST));
    CHECK(!lg_tt_info(&tt, 2));
}

static void test_station_rule(void) {
    lg_tt_t tt;
    lg_tt_entry_t e[LG_SCHED_ENTRIES];
    uint32_t n;

    CHECK(load(&tt, k_basic) == 0);
    /* Train 1 (planned 100 s) is reported 30 s early: moved earlier, source STATION. */
    CHECK(lg_tt_station(&tt, LG_DIR_WEST, 70000, 10000) == 1);
    n = lg_tt_next(&tt, LG_DIR_WEST, 10000, e, LG_SCHED_ENTRIES);
    CHECK(n == 3 && e[0].id == 1 && e[0].eta_ms == 70000 && e[0].tt_eta_ms == 100000 &&
          e[0].source == LG_SRC_STATION);
    /* A later eta (90 s) never delays entry 1 (70 s). It lies 160 s (<= 15 min) before entry 4 (250 s),
     * so entry 4 moves earlier: more closure, never less protection. */
    CHECK(lg_tt_station(&tt, LG_DIR_WEST, 90000, 20000) == 1);
    n = lg_tt_next(&tt, LG_DIR_WEST, 20000, e, LG_SCHED_ENTRIES);
    CHECK(n == 3 && e[0].id == 1 && e[0].eta_ms == 70000 && e[1].id == 4 && e[1].eta_ms == 90000);
    CHECK(tt.station_moved == 2);
}

static void test_station_late_and_added(void) {
    lg_tt_t tt;
    lg_tt_entry_t e[LG_SCHED_ENTRIES];
    uint32_t n;

    CHECK(load(&tt, "{\"trains\":[{\"id\":1,\"dir\":\"west\",\"eta_s\":100}]}") == 0);
    /* Reported 60 s late: inside the window of entry 1, which keeps the gate down: ignored, not delayed. */
    CHECK(lg_tt_station(&tt, LG_DIR_WEST, 160000, 50000) == 0);
    n = lg_tt_next(&tt, LG_DIR_WEST, 50000, e, LG_SCHED_ENTRIES);
    CHECK(n == 1 && e[0].eta_ms == 100000 && e[0].source == LG_SRC_TIMETABLE && tt.station_ignored == 1);
    /* Far later than any window and no entry after it: a new STATION entry. */
    CHECK(lg_tt_station(&tt, LG_DIR_WEST, 2000000, 50000) == 2);
    n = lg_tt_next(&tt, LG_DIR_WEST, 50000, e, LG_SCHED_ENTRIES);
    CHECK(n == 2 && e[1].id >= LG_SCHED_STATION_ID_BASE && e[1].source == LG_SRC_STATION &&
          e[1].eta_ms == 2000000);
    /* An eta more than 15 min before the next entry is another train: added. */
    CHECK(load(&tt, "{\"trains\":[{\"id\":1,\"dir\":\"west\",\"eta_s\":2000}]}") == 0);
    CHECK(lg_tt_station(&tt, LG_DIR_WEST, 1000000, 0) == 2);
    /* A direction without timetable entries: added, but information is still missing (network-only). */
    CHECK(lg_tt_station(&tt, LG_DIR_EAST, 100000, 0) == 2);
    CHECK(!lg_tt_info(&tt, LG_DIR_EAST));
    CHECK(lg_tt_next(&tt, LG_DIR_EAST, 0, e, LG_SCHED_ENTRIES) == 1);
}

static void test_station_rejected(void) {
    lg_tt_t tt;

    CHECK(load(&tt, k_basic) == 0);
    CHECK(lg_tt_station(&tt, 2, 100000, 0) == -1);
    CHECK(lg_tt_station(&tt, LG_DIR_WEST, 1000, 2000) == -1);                        /* in the past */
    CHECK(lg_tt_station(&tt, LG_DIR_WEST, LG_STATION_ETA_MAX_MS + 1001u, 1000) == -1); /* too far */
    CHECK(tt.station_rejected == 3 && tt.count == 4);
}

static void test_station_table_full(void) {
    static char text[8192];
    lg_tt_t tt;
    size_t used;
    int i;

    used = (size_t)snprintf(text, sizeof text, "{\"trains\":[");
    for (i = 1; i <= LG_TT_MAX; i++) {
        used += (size_t)snprintf(text + used, sizeof text - used, "%s{\"id\":%d,\"dir\":\"east\",\"eta_s\":%d}",
                                 i > 1 ? "," : "", i, 20000 + i);
    }
    (void)snprintf(text + used, sizeof text - used, "]}");
    CHECK(load(&tt, text) == 0 && tt.count == LG_TT_MAX);
    /* Full and nothing expired: ignored (counted). */
    CHECK(lg_tt_station(&tt, LG_DIR_WEST, 1000, 0) == 0 && tt.station_ignored == 1);
    /* Later the first entries have expired: their slot is reused. */
    CHECK(lg_tt_station(&tt, LG_DIR_WEST, 20200000, 20200000) == 2);
}

static void test_station_ids_unique(void) {
    lg_tt_t tt;
    int i;
    int k;
    int j;
    int dup = 0;

    lg_tt_init(&tt);
    tt.next_station_id = 0xFFFE;
    for (i = 0; i < 6; i++) {
        CHECK(lg_tt_station(&tt, LG_DIR_EAST, (uint32_t)(i * 2000000), 0) == 2);
    }
    for (k = 0; k < tt.count; k++) {
        CHECK(tt.e[k].id >= LG_SCHED_STATION_ID_BASE);
        for (j = k + 1; j < tt.count; j++) {
            dup = dup || tt.e[k].id == tt.e[j].id;
        }
    }
    CHECK(!dup);
}

static void test_text(void) {
    lg_tt_t tt;
    char buf[2048];
    char small[40];
    size_t n;

    CHECK(load(&tt, k_basic) == 0);
    n = lg_tt_text(&tt, 0, buf, sizeof buf);
    CHECK(n > 0 && n == strlen(buf));
    CHECK(strstr(buf, "dir=west info=1 outage=0 listed=3") != NULL);
    CHECK(strstr(buf, "entry id=3 dir=west source=TIMETABLE eta_ms=400500") != NULL);
    CHECK(strstr(buf, "station rx=0") != NULL);
    n = lg_tt_text(&tt, 0, small, sizeof small);
    CHECK(n == sizeof small - 1 && small[sizeof small - 1] == '\0');
    CHECK(lg_tt_text(&tt, 0, NULL, 10) == 0);
}

static void test_reply(void) {
    lg_tt_t tt;
    lg_sched_reply_t r;

    CHECK(load(&tt, k_basic) == 0);
    memset(&r, 0xAB, sizeof r);
    r.hdr.seq = 77;
    lg_tt_reply(&tt, LG_DIR_WEST, 0, 5000, &r);
    CHECK(r.hdr.seq == 77); /* the header is the caller's */
    CHECK(r.dir == LG_DIR_WEST && r.flags == LG_SCHED_INFO && r.count == 3 && r.reserved == 0);
    CHECK(r.entry[0].id == 1 && r.entry[0].eta_ns == 105000ull * 1000000ull);
    CHECK(r.entry[0].lead_ms == 60000 && r.entry[0].tol_ms == 180000);
    CHECK(r.entry[0].confidence == LG_TT_CONF_TIMETABLE && r.entry[3].id == 0 && r.entry[3].eta_ns == 0);
    CHECK(lg_tt_station(&tt, LG_DIR_WEST, 90000, 0) == 1);
    lg_tt_reply(&tt, LG_DIR_WEST, 0, 0, &r);
    CHECK(r.entry[0].source == LG_SRC_STATION && r.entry[0].confidence == LG_TT_CONF_STATION);
    lg_tt_set_outage(&tt, 1u << LG_DIR_WEST);
    lg_tt_reply(&tt, LG_DIR_WEST, 0, 0, &r);
    CHECK(r.flags == 0 && r.count == 0 && r.entry[0].id == 0);
}

/* ---- window tracker ---------------------------------------------------------------------------------- */

static lg_win_entry_t entry(uint16_t id, uint32_t eta, uint32_t lead, uint32_t tol) {
    lg_win_entry_t e;

    memset(&e, 0, sizeof e);
    e.id = id;
    e.source = LG_SRC_TIMETABLE;
    e.eta_ms = eta;
    e.lead_ms = lead;
    e.tol_ms = tol;
    return e;
}

static void test_win_open_confirm(void) {
    lg_win_dir_t wd;
    lg_win_entry_t e[2];

    lg_win_init(&wd);
    e[0] = entry(1, 100000, 60000, 180000);
    e[1] = entry(2, 200000, 60000, 180000);
    lg_win_update(&wd, e, 2);
    CHECK(lg_win_count(&wd, LG_WIN_PENDING) == 2 && !lg_win_open(&wd));
    /* Before the start, or not uncertain: nothing opens. */
    CHECK(lg_win_tick(&wd, 1, 10, 39999) == 0 && !lg_win_open(&wd));
    CHECK(lg_win_tick(&wd, 0, 10, 50000) == 0 && !lg_win_open(&wd));
    CHECK(lg_win_tick(&wd, 1, 10, 40000) == 0 && lg_win_open(&wd) && wd.opened == 1);
    CHECK(!lg_win_axles_moved(&wd, 10) && lg_win_axles_moved(&wd, 18));
    CHECK(lg_win_confirm_island(&wd, 18) == 1 && !lg_win_open(&wd));
    CHECK(lg_win_confirm_island(&wd, 18) == 0);
    /* The second train opens its own window and needs the count to move again. */
    CHECK(lg_win_tick(&wd, 1, 18, 140000) == 0 && lg_win_open(&wd));
    CHECK(!lg_win_axles_moved(&wd, 18));
    CHECK(lg_win_confirm_island(&wd, 26) == 1 && !lg_win_open(&wd));
    CHECK(wd.confirmed_island == 2);
}

static void test_win_two_open_one_train(void) {
    lg_win_dir_t wd;
    lg_win_entry_t e[2];

    lg_win_init(&wd);
    e[0] = entry(1, 100000, 60000, 180000);
    e[1] = entry(2, 130000, 60000, 180000);
    lg_win_update(&wd, e, 2);
    (void)lg_win_tick(&wd, 1, 0, 80000); /* both windows open */
    CHECK(lg_win_count(&wd, LG_WIN_OPEN) == 2);
    /* One train passes: only the oldest window is confirmed; the other waits for another train. */
    CHECK(lg_win_confirm_island(&wd, 8) == 1);
    CHECK(lg_win_count(&wd, LG_WIN_OPEN) == 1 && !lg_win_axles_moved(&wd, 8) && lg_win_axles_moved(&wd, 16));
}

static void test_win_overdue_guard(void) {
    lg_win_dir_t wd;
    lg_win_entry_t e = entry(5, 100000, 60000, 180000);

    lg_win_init(&wd);
    lg_win_update(&wd, &e, 1);
    (void)lg_win_tick(&wd, 1, 0, 40000);
    CHECK(lg_win_tick(&wd, 1, 0, 280000) == 0); /* at the end: not yet overdue */
    CHECK(lg_win_tick(&wd, 1, 0, 280001) == 1); /* past the end: overdue once */
    CHECK(lg_win_tick(&wd, 1, 0, 300000) == 0 && lg_win_open(&wd) && wd.overdue == 1);
    /* lg-schedule stops listing it: still OPEN (the gate stays down). */
    lg_win_update(&wd, NULL, 0);
    CHECK(lg_win_open(&wd));
    CHECK(lg_win_confirm_guard(&wd) == 1 && !lg_win_open(&wd) && wd.confirmed_guard == 1);
    CHECK(lg_win_confirm_guard(&wd) == 0);
}

static void test_win_update_rules(void) {
    lg_win_dir_t wd;
    lg_win_entry_t e[3];

    lg_win_init(&wd);
    e[0] = entry(1, 100000, 60000, 180000);
    e[1] = entry(2, 500000, 60000, 180000);
    lg_win_update(&wd, e, 2);
    (void)lg_win_tick(&wd, 1, 0, 50000);
    /* A station moves entry 2 earlier: the new times are used. */
    e[1].eta_ms = 120000;
    lg_win_update(&wd, e, 2);
    CHECK(lg_win_tick(&wd, 1, 0, 60000) == 0 && lg_win_count(&wd, LG_WIN_OPEN) == 2);
    /* A pending entry that disappears is forgotten. */
    lg_win_init(&wd);
    lg_win_update(&wd, e, 2);
    lg_win_update(&wd, &e[0], 1);
    CHECK(lg_win_count(&wd, LG_WIN_PENDING) == 1);
    /* A confirmed entry that drops out of the reply and comes back is not opened again. */
    lg_win_init(&wd);
    lg_win_update(&wd, e, 1);
    (void)lg_win_tick(&wd, 1, 0, 50000);
    (void)lg_win_confirm_island(&wd, 4);
    lg_win_update(&wd, NULL, 0);
    (void)lg_win_tick(&wd, 1, 4, 60000);
    lg_win_update(&wd, e, 1);
    CHECK(lg_win_tick(&wd, 1, 4, 61000) == 0 && !lg_win_open(&wd) && lg_win_count(&wd, LG_WIN_CONFIRMED) == 1);
    /* After its end (and not listed) a confirmed window is forgotten. */
    lg_win_update(&wd, NULL, 0);
    (void)lg_win_tick(&wd, 1, 4, 280001);
    CHECK(lg_win_count(&wd, LG_WIN_CONFIRMED) == 0);
    /* More entries than LG_SCHED_ENTRIES: the rest is ignored. */
    lg_win_init(&wd);
    {
        lg_win_entry_t many[6];
        int i;

        for (i = 0; i < 6; i++) {
            many[i] = entry((uint16_t)(10 + i), (uint32_t)(100000 + i * 1000), 60000, 180000);
        }
        lg_win_update(&wd, many, 6);
        CHECK(lg_win_count(&wd, LG_WIN_PENDING) == LG_SCHED_ENTRIES);
    }
}

static void test_win_full_of_open(void) {
    lg_win_dir_t wd;
    lg_win_entry_t e[LG_SCHED_ENTRIES];
    int round;
    int i;

    lg_win_init(&wd);
    /* Fill every slot with OPEN windows, LG_SCHED_ENTRIES at a time. */
    for (round = 0; round < LG_WIN_MAX / LG_SCHED_ENTRIES; round++) {
        for (i = 0; i < LG_SCHED_ENTRIES; i++) {
            e[i] = entry((uint16_t)(100 + round * 10 + i), 100000, 60000, 180000);
        }
        lg_win_update(&wd, e, LG_SCHED_ENTRIES);
        (void)lg_win_tick(&wd, 1, 0, 50000);
    }
    CHECK(lg_win_count(&wd, LG_WIN_OPEN) == LG_WIN_MAX);
    e[0] = entry(999, 100000, 60000, 180000);
    lg_win_update(&wd, e, 1);
    CHECK(wd.dropped == 1 && lg_win_count(&wd, LG_WIN_OPEN) == LG_WIN_MAX);
}

/* Step 12 run 1 on QNX: a window whose train passed under normal detection must not open later. */
static void test_win_confirmed_while_trustworthy(void) {
    lg_win_dir_t wd;
    lg_win_entry_t e[2];

    lg_win_init(&wd);
    e[0] = entry(1, 100000, 60000, 180000); /* start 40000 */
    e[1] = entry(2, 110000, 60000, 180000); /* start 50000 */
    lg_win_update(&wd, e, 2);
    (void)lg_win_tick(&wd, 0, 5, 30000); /* before any start: nothing armed */
    CHECK(wd.w[0].armed == 0 && wd.w[1].armed == 0);
    (void)lg_win_tick(&wd, 0, 5, 41000);
    CHECK(wd.w[0].armed == 1 && wd.w[1].armed == 0 && lg_win_count(&wd, LG_WIN_PENDING) == 2);
    (void)lg_win_tick(&wd, 0, 5, 51000);
    CHECK(wd.w[1].armed == 1);
    /* The first train passes the island under normal detection: only the oldest window is done. */
    (void)lg_win_tick(&wd, 0, 13, 100000);
    CHECK(wd.confirmed_normal == 1 && lg_win_count(&wd, LG_WIN_CONFIRMED) == 1 &&
          lg_win_count(&wd, LG_WIN_PENDING) == 1);
    /* The direction becomes uncertain (a transient loss): only the window of the train still due opens. */
    (void)lg_win_tick(&wd, 1, 13, 101000);
    CHECK(lg_win_count(&wd, LG_WIN_OPEN) == 1 && wd.opened == 1);
    CHECK(!lg_win_axles_moved(&wd, 13) && lg_win_axles_moved(&wd, 21));
    /* Armed, then uncertain in the same step the count moves: it opens, and the move counts. */
    lg_win_init(&wd);
    lg_win_update(&wd, e, 1);
    (void)lg_win_tick(&wd, 0, 5, 41000);
    (void)lg_win_tick(&wd, 1, 13, 42000);
    CHECK(lg_win_open(&wd) && lg_win_axles_moved(&wd, 13) && wd.confirmed_normal == 0);
}

static void test_win_wrap(void) {
    lg_win_dir_t wd;
    lg_win_entry_t e = entry(1, 20000, 60000, 180000); /* start lies before 0: wraps */

    lg_win_init(&wd);
    lg_win_update(&wd, &e, 1);
    CHECK(lg_win_tick(&wd, 1, 0, 0xFFFF0000u) == 0 && !lg_win_open(&wd)); /* 85 s before the eta: not yet */
    CHECK(lg_win_tick(&wd, 1, 0, 0xFFFFFFF0u) == 0 && lg_win_open(&wd));
    CHECK(lg_win_tick(&wd, 1, 0, 200000) == 0);
    CHECK(lg_win_tick(&wd, 1, 0, 200001) == 1);
}

int main(void) {
    test_load_valid();
    test_load_invalid();
    test_too_many();
    test_listing_and_expiry();
    test_outage();
    test_station_rule();
    test_station_late_and_added();
    test_station_rejected();
    test_station_table_full();
    test_station_ids_unique();
    test_text();
    test_reply();
    test_win_open_confirm();
    test_win_two_open_one_train();
    test_win_overdue_guard();
    test_win_update_rules();
    test_win_full_of_open();
    test_win_confirmed_while_trustworthy();
    test_win_wrap();
    LG_TEST_DONE("test_timetable");
}
