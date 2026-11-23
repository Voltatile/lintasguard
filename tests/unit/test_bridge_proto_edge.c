#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "lg_bridge_proto.h"
#include "lg_json_mini.h"
#include "lg_plant_model.h"
#include "lg_proto.h"
#include "lg_test.h"

/*
 * Step 10 edge cases for lg-bridge: worst-case line sizes, JSON validity of every outbound line with hostile
 * device text, whole-word key lookups, inbound whitelist corners, the recent-event ring at its boundaries and
 * the plant text -> bridge state round trip. Complements test_bridge_proto.c; no randomness here.
 */

static lg_json_doc_t g_doc;
static lg_json_doc_t g_out;

/* One JSON object plus exactly one trailing newline, pure ASCII, no raw control characters. */
static const lg_json_node_t *valid_line(const char *line, size_t n) {
    size_t i;
    int ascii = 1;

    CHECK(n > 0 && n < LG_BRIDGE_LINE_MAX);
    if (n == 0) {
        return NULL;
    }
    CHECK(strlen(line) == n && line[n - 1] == '\n');
    for (i = 0; i + 1 < n; i++) {
        unsigned char c = (unsigned char)line[i];

        if (c < 0x20 || c >= 0x80) {
            ascii = 0;
        }
    }
    CHECK(ascii);
    if (lg_json_parse(&g_out, line, n - 1) != LG_JSON_OK) {
        printf("invalid outbound line: %s", line);
        CHECK(!"outbound line is valid JSON");
        return NULL;
    }
    CHECK(lg_json_root(&g_out)->type == LG_JSON_OBJECT);
    return lg_json_root(&g_out);
}

static int64_t numv(const lg_json_node_t *o, const char *key) {
    int64_t v = INT64_MIN;

    (void)lg_json_get_milli(&g_out, o, key, &v);
    return v;
}

/* ---- kv lookups ---------------------------------------------------------------------------------- */

static void test_kv_whole_words_and_numbers(void) {
    uint32_t u = 7;
    int32_t i = 7;
    char s[16];

    /* The second, whole-word occurrence is found after a partial one. */
    CHECK(lg_bridge_kv_u32("xlevel=3 level_end=5 level=4", "level", &u) == 0 && u == 4);
    CHECK(lg_bridge_kv_u32("levels=3", "level", &u) == -1);
    CHECK(lg_bridge_kv_u32("a=level=3", "level", &u) == -1); /* inside another value */
    CHECK(lg_bridge_kv_u32("x\nlevel=3\n", "level", &u) == 0 && u == 3);
    CHECK(lg_bridge_kv_u32("x\tlevel=3", "level", &u) == 0 && u == 3);
    CHECK(lg_bridge_kv_u32("x\r\nlevel=3\r\n", "level", &u) == 0 && u == 3);
    CHECK(lg_bridge_kv_u32("level=3", "", &u) == -1);
    CHECK(lg_bridge_kv_u32("level=3", NULL, &u) == -1);
    CHECK(lg_bridge_kv_u32("level=3", "level", NULL) == -1);

    u = 7;
    CHECK(lg_bridge_kv_u32("v=4294967295", "v", &u) == 0 && u == 4294967295u);
    CHECK(lg_bridge_kv_u32("v=4294967296", "v", &u) == -1);
    CHECK(lg_bridge_kv_u32("v=0xFFFFFFFF", "v", &u) == 0 && u == 0xFFFFFFFFu);
    CHECK(lg_bridge_kv_u32("v=0xffffffff", "v", &u) == 0 && u == 0xFFFFFFFFu);
    CHECK(lg_bridge_kv_u32("v=0x100000000", "v", &u) == -1);
    CHECK(lg_bridge_kv_u32("v=0x", "v", &u) == -1);
    CHECK(lg_bridge_kv_u32("v=0xg", "v", &u) == -1);
    CHECK(lg_bridge_kv_u32("v=-1", "v", &u) == -1);
    CHECK(lg_bridge_kv_u32("v=+1", "v", &u) == -1);
    CHECK(lg_bridge_kv_u32("v=1.5", "v", &u) == -1);
    CHECK(lg_bridge_kv_u32("v=12abc", "v", &u) == -1);
    CHECK(lg_bridge_kv_u32("v=000000000000000000000000012", "v", &u) == 0 && u == 12);
    CHECK(lg_bridge_kv_u32("v=99999999999999999999999999", "v", &u) == -1);
    CHECK(lg_bridge_kv_u32("v=2,3", "v", &u) == 0 && u == 2); /* a comma ends a number */
    CHECK(u == 2);

    i = 7;
    CHECK(lg_bridge_kv_i32("v=-2147483648", "v", &i) == 0 && i == INT32_MIN);
    CHECK(lg_bridge_kv_i32("v=2147483647", "v", &i) == 0 && i == INT32_MAX);
    CHECK(lg_bridge_kv_i32("v=2147483648", "v", &i) == -1);
    CHECK(lg_bridge_kv_i32("v=-2147483649", "v", &i) == -1);
    CHECK(lg_bridge_kv_i32("v=-", "v", &i) == -1);
    CHECK(lg_bridge_kv_i32("v=--5", "v", &i) == -1);
    CHECK(lg_bridge_kv_i32("v=+5", "v", &i) == -1);
    CHECK(lg_bridge_kv_i32("v=-0", "v", &i) == 0 && i == 0);
    CHECK(lg_bridge_kv_i32("v=99999999999999999999", "v", &i) == -1);

    /* A failed lookup leaves the output alone. */
    u = 55;
    CHECK(lg_bridge_kv_u32("v=oops", "v", &u) == -1 && u == 55);
    i = 55;
    CHECK(lg_bridge_kv_i32("v=oops", "v", &i) == -1 && i == 55);

    /* Strings: runs to blank, keeps commas, exact fit and one byte short. */
    CHECK(lg_bridge_kv_str("a=1,0 b=2", "a", s, sizeof s) == 0 && strcmp(s, "1,0") == 0);
    CHECK(lg_bridge_kv_str("a=abc", "a", s, 4) == 0 && strcmp(s, "abc") == 0);
    CHECK(lg_bridge_kv_str("a=abc", "a", s, 3) == -1);
    CHECK(lg_bridge_kv_str("a= b=1", "a", s, sizeof s) == -1); /* empty value */
    CHECK(lg_bridge_kv_str("a=x", "a", s, 0) == -1);
    CHECK(lg_bridge_kv_str("a=x", "a", NULL, 4) == -1);
}

/* ---- device texts -------------------------------------------------------------------------------- */

static void test_device_text_corners(void) {
    lg_bridge_state_t st;
    lg_bridge_event_t ev;

    memset(&st, 0, sizeof st);
    CHECK(lg_bridge_from_controller(&st, "") == -1 && st.ctl_ok == 0);
    CHECK(lg_bridge_from_controller(&st, "level=abc") == -1);
    CHECK(lg_bridge_from_controller(&st, "level=4 active=1,1") == 0);
    CHECK(st.level == 4 && st.active_west == 1 && st.active_east == 1 && st.ctl_ok == 1);
    /* Malformed "active" leaves the previous flags, it is not guessed. */
    CHECK(lg_bridge_from_controller(&st, "level=0 active=2,0") == 0 && st.active_west == 1 && st.active_east == 1);
    CHECK(lg_bridge_from_controller(&st, "level=0 active=1") == 0 && st.active_west == 1);
    CHECK(lg_bridge_from_controller(&st, "level=0 active=0,0") == 0 && st.active_west == 0 && st.active_east == 0);
    /* A bad controller text after a good one clears ctl_ok (the snapshot must not claim a fresh read). */
    CHECK(lg_bridge_from_controller(&st, "garbage") == -1 && st.ctl_ok == 0);

    CHECK(lg_bridge_from_gate(&st, "state=UP") == -1 && st.gate_ok == 0); /* position missing */
    CHECK(lg_bridge_from_gate(&st, "position=3") == -1);
    CHECK(lg_bridge_from_gate(&st, "state=VERYLONGSTATE1 position=3") == -1); /* 12-byte field: no truncation */
    CHECK(lg_bridge_from_gate(&st, "state=DOWN position=-5 watchdog_trips=2") == 0);
    CHECK(st.gate_position == -5 && st.watchdog_trips == 2 && strcmp(st.gate_state, "DOWN") == 0);
    CHECK(lg_bridge_from_gate(&st, "state=ELEVEN_CHAR position=1") == 0 && strlen(st.gate_state) == 11);

    /* Plant: header missing, bad trains skipped, never more than LG_BRIDGE_MAX_TRAINS. */
    CHECK(lg_bridge_from_plant(&st, "train id=1 dir=west front_mm=1 rear_mm=0\n") == -1 && st.plant_ok == 0);
    CHECK(st.n_trains == 0);
    CHECK(lg_bridge_from_plant(&st,
                               "t_ms=5 island=0\n"
                               "train id=70000 dir=west front_mm=1 rear_mm=0\n"     /* id > uint16 */
                               "train id=2 dir=up front_mm=1 rear_mm=0\n"           /* bad dir */
                               "train id=3 dir=west front_mm=x rear_mm=0\n"         /* bad number */
                               "train id=4 dir=west front_mm=1\n"                   /* rear missing */
                               "trainx id=5 dir=west front_mm=1 rear_mm=0\n"        /* not a train line */
                               " train id=6 dir=west front_mm=1 rear_mm=0\n"        /* leading blank */
                               "train id=65535 dir=east front_mm=-9 rear_mm=-99\n"  /* the only good one */
                               "train id=8 dir=west front_mm=3 rear_mm=2") == 0);   /* no final newline: good */
    CHECK(st.n_trains == 2);
    CHECK(st.trains[0].id == 65535 && st.trains[0].dir == LG_DIR_EAST && st.trains[0].front_mm == -9 &&
          st.trains[0].rear_mm == -99);
    CHECK(st.trains[1].id == 8 && st.trains[1].dir == LG_DIR_WEST);
    {
        char many[1024];
        size_t used = 0;
        int k;

        used += (size_t)snprintf(many, sizeof many, "t_ms=1 island=1\n");
        for (k = 0; k < 20; k++) {
            used += (size_t)snprintf(many + used, sizeof many - used, "train id=%d dir=west front_mm=%d rear_mm=0\n",
                                     k + 1, k);
        }
        CHECK(lg_bridge_from_plant(&st, many) == 0 && st.n_trains == LG_BRIDGE_MAX_TRAINS);
        CHECK(st.trains[LG_BRIDGE_MAX_TRAINS - 1].id == LG_BRIDGE_MAX_TRAINS); /* the first 8, in order */
    }
    /* A second call replaces, it does not append. */
    CHECK(lg_bridge_from_plant(&st, "t_ms=2 island=0\n") == 0 && st.n_trains == 0);
    /* A train line of 128+ bytes is skipped, not half-parsed. */
    {
        char longline[300];

        (void)snprintf(longline, sizeof longline, "t_ms=2\ntrain id=1 dir=west front_mm=1 rear_mm=0 pad=%0150d\n", 0);
        CHECK(lg_bridge_from_plant(&st, longline) == 0 && st.n_trains == 0);
    }

    /* Events. */
    CHECK(lg_bridge_parse_event(NULL, &ev) == -1);
    CHECK(lg_bridge_parse_event("seq=1 t_ms=1 kind=LEVEL from=0 to=2", &ev) == -1); /* reason missing */
    CHECK(lg_bridge_parse_event("seq=1 t_ms=1 kind=LEVEL from=0 to=2 reason=4", NULL) == -1);
    CHECK(lg_bridge_parse_event("seq=1 t_ms=1 kind=level from=0 to=2 reason=4", &ev) == -1); /* case matters */
    CHECK(lg_bridge_parse_event("seq=1 t_ms=1 kind=NOTIFY sensor=west/a", &ev) == -1);       /* diag missing */
    CHECK(lg_bridge_parse_event("seq=1 t_ms=1 kind=NOTIFY sensor=0123456789abcdef diag=X", &ev) == -1); /* 16 chars */
    CHECK(lg_bridge_parse_event("seq=1 t_ms=1 kind=NOTIFY sensor=0123456789abcde diag=X", &ev) == 0);
    CHECK(lg_bridge_parse_event("seq=4294967295 t_ms=4294967295 kind=LEVEL from=4294967295 to=4294967295 reason=4294967295",
                                &ev) == 0);
    CHECK(ev.seq == 4294967295u && ev.reason == 4294967295u);
}

/* ---- outbound lines ------------------------------------------------------------------------------ */

static void test_state_worst_case_fits_and_parses(void) {
    static lg_bridge_state_t st;
    char line[LG_BRIDGE_LINE_MAX];
    const lg_json_node_t *o;
    const lg_json_node_t *tr;
    size_t n;
    uint32_t k;

    memset(&st, 0, sizeof st);
    st.t_ms = UINT32_MAX;
    st.stale = 1;
    st.ctl_ok = st.gate_ok = st.plant_ok = 1;
    st.level = st.lights = st.bell = st.tickets = st.stalls = st.sensor_failed = st.events = UINT32_MAX;
    st.watchdog_trips = st.plant_ms = st.island = UINT32_MAX;
    st.active_west = st.active_east = UINT32_MAX;
    memset(st.gate_state, 'G', sizeof st.gate_state - 1);
    st.gate_position = INT32_MIN;
    st.n_trains = LG_BRIDGE_MAX_TRAINS;
    for (k = 0; k < LG_BRIDGE_MAX_TRAINS; k++) {
        st.trains[k].id = 65535;
        st.trains[k].dir = (k & 1) ? LG_DIR_WEST : LG_DIR_EAST;
        st.trains[k].front_mm = INT32_MIN;
        st.trains[k].rear_mm = INT32_MIN;
    }
    n = lg_bridge_state_json(&st, line, sizeof line);
    CHECK(n > 0 && n < LG_BRIDGE_LINE_MAX);
    printf("worst-case state line: %u of %d bytes\n", (unsigned)n, LG_BRIDGE_LINE_MAX);
    o = valid_line(line, n);
    CHECK(o != NULL);
    if (o == NULL) {
        return;
    }
    /* "t" is seconds with 3 decimals: 4294967.295 s, stored times 1000 by the parser. */
    CHECK(numv(o, "t") == 4294967295LL);
    CHECK(numv(o, "level") == 4294967295000LL && numv(o, "plant_ms") == 4294967295000LL);
    CHECK(numv(o, "gate_position") == -2147483648000LL && numv(o, "sensor_failed") == 4294967295000LL);
    tr = lg_json_get(&g_out, o, "trains");
    CHECK(tr != NULL && tr->type == LG_JSON_ARRAY && tr->count == LG_BRIDGE_MAX_TRAINS);

    /* Values that the parser can hold (12 integer digits) must come back unchanged. */
    st.level = 2;
    st.plant_ms = 123456789u;
    st.gate_position = -5;
    st.n_trains = 1;
    st.trains[0].front_mm = -120000;
    st.trains[0].rear_mm = -240000;
    st.trains[0].id = 9;
    st.trains[0].dir = LG_DIR_WEST;
    st.active_west = 1;
    st.active_east = 0;
    n = lg_bridge_state_json(&st, line, sizeof line);
    o = valid_line(line, n);
    CHECK(o != NULL && numv(o, "level") == 2000 && numv(o, "plant_ms") == 123456789000LL);
    CHECK(o != NULL && numv(o, "gate_position") == -5000);
    if (o != NULL) {
        const lg_json_node_t *t0 = lg_json_get(&g_out, o, "trains");
        const lg_json_node_t *e = t0 ? &g_out.nodes[t0->first_child] : NULL;

        CHECK(e != NULL && numv(e, "front_mm") == -120000000LL && numv(e, "rear_mm") == -240000000LL);
        CHECK(e != NULL && numv(e, "id") == 9000);
    }

    /* Corrupt count: capped at LG_BRIDGE_MAX_TRAINS, never reads past the array. */
    st.n_trains = 1000;
    n = lg_bridge_state_json(&st, line, sizeof line);
    o = valid_line(line, n);
    CHECK(o != NULL && lg_json_get(&g_out, o, "trains")->count == LG_BRIDGE_MAX_TRAINS);
}

static void test_hostile_text_stays_json(void) {
    lg_bridge_state_t st;
    lg_bridge_event_t ev;
    char line[LG_BRIDGE_LINE_MAX];
    char big[400];
    const lg_json_node_t *o;
    const char *v = NULL;
    size_t n;

    /* Quotes, backslashes and control characters in the gate state field. */
    memset(&st, 0, sizeof st);
    memcpy(st.gate_state, "a\"b\\c\n\t\x01" "d", 9);
    n = lg_bridge_state_json(&st, line, sizeof line);
    o = valid_line(line, n);
    CHECK(o != NULL && lg_json_get_str(&g_out, o, "gate", &v) == 0 && strcmp(v, "a\"b\\cd") == 0);

    /* A string ending in a backslash and one with only quotes. */
    memset(&st, 0, sizeof st);
    memcpy(st.gate_state, "\\\\\\\\\\\\\\\\\\\\\\", 11);
    n = lg_bridge_state_json(&st, line, sizeof line);
    CHECK(valid_line(line, n) != NULL);
    memcpy(st.gate_state, "\"\"\"\"\"\"\"\"\"\"\"", 11);
    n = lg_bridge_state_json(&st, line, sizeof line);
    CHECK(valid_line(line, n) != NULL);

    /* Metric names and reply texts: very long, quotes and backslashes at the cut-off position. */
    memset(big, '\\', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    n = lg_bridge_metric_json(1, big, -99999999999LL, line, sizeof line);
    o = valid_line(line, n);
    CHECK(o != NULL && lg_json_get_str(&g_out, o, "kind", &v) == 0 && strcmp(v, "metric") == 0);
    memset(big, '"', sizeof big - 1);
    n = lg_bridge_reply_json(0, big, big, line, sizeof line);
    CHECK(valid_line(line, n) != NULL);
    memset(big, 'a', sizeof big - 1);
    big[155] = '\\';
    n = lg_bridge_reply_json(1, big, "x", line, sizeof line);
    CHECK(valid_line(line, n) != NULL);
    n = lg_bridge_reply_json(0, NULL, NULL, line, sizeof line); /* NULL strings do not crash */
    CHECK(valid_line(line, n) != NULL);
    n = lg_bridge_metric_json(1, NULL, 0, line, sizeof line);
    CHECK(valid_line(line, n) != NULL);

    /* Notify with the longest possible severity/sensor/diag, all quotes and backslashes. */
    memset(&ev, 0, sizeof ev);
    ev.kind = LG_BRIDGE_EV_NOTIFY;
    ev.seq = UINT32_MAX;
    ev.t_ms = UINT32_MAX;
    memset(ev.severity, '"', sizeof ev.severity - 1);
    memset(ev.sensor, '\\', sizeof ev.sensor - 1);
    memset(ev.diag, '"', sizeof ev.diag - 1);
    n = lg_bridge_event_json(&ev, line, sizeof line);
    CHECK(valid_line(line, n) != NULL);
    memset(ev.severity, 'S', sizeof ev.severity - 1);
    memset(ev.sensor, 'W', sizeof ev.sensor - 1);
    memset(ev.diag, 'D', sizeof ev.diag - 1);
    n = lg_bridge_event_json(&ev, line, sizeof line);
    CHECK(valid_line(line, n) != NULL);
    /* A level event with a reason number outside the table. */
    ev.kind = LG_BRIDGE_EV_LEVEL;
    ev.reason = UINT32_MAX;
    n = lg_bridge_event_json(&ev, line, sizeof line);
    o = valid_line(line, n);
    CHECK(o != NULL && lg_json_get_str(&g_out, o, "reason", &v) == 0 && strcmp(v, "UNKNOWN") == 0);
    /* An empty severity falls back to "info"; the diag OK message says the sensor works again. */
    memset(&ev, 0, sizeof ev);
    ev.kind = LG_BRIDGE_EV_NOTIFY;
    strcpy(ev.sensor, "east/island");
    strcpy(ev.diag, "OK");
    n = lg_bridge_event_json(&ev, line, sizeof line);
    o = valid_line(line, n);
    CHECK(o != NULL && lg_json_get_str(&g_out, o, "severity", &v) == 0 && strcmp(v, "info") == 0);
    CHECK(o != NULL && lg_json_get_str(&g_out, o, "text", &v) == 0 && strcmp(v, "Sensor east/island is working again") == 0);
}

/* Bytes >= 0x80 in device text. The relay decodes every line as UTF-8; a lone 0x80..0xFF byte would make the
 * whole line a "bad line" (the event is lost). Device texts are ASCII today, but the writer must not pass raw
 * non-ASCII bytes through, as it already filters control characters. */
static void test_non_ascii_device_text_does_not_break_line(void) {
    lg_bridge_state_t st;
    char line[LG_BRIDGE_LINE_MAX];
    size_t n;
    size_t i;
    int clean = 1;

    memset(&st, 0, sizeof st);
    memcpy(st.gate_state, "UP\xff\xfe", 5);
    n = lg_bridge_state_json(&st, line, sizeof line);
    CHECK(n > 0);
    for (i = 0; i < n; i++) {
        if ((unsigned char)line[i] >= 0x80) {
            clean = 0;
        }
    }
    CHECK(clean); /* known to fail today: bridge_proto.c jw_str only drops bytes < 0x20 */
}

static void test_small_buffers(void) {
    lg_bridge_state_t st;
    lg_bridge_event_t ev;
    char line[LG_BRIDGE_LINE_MAX];
    char small[LG_BRIDGE_LINE_MAX];
    size_t full;
    size_t len;
    int round;

    memset(&st, 0, sizeof st);
    memset(&ev, 0, sizeof ev);
    ev.kind = LG_BRIDGE_EV_LEVEL;
    ev.reason = 4;
    for (round = 0; round < 4; round++) {
        switch (round) {
        case 0: full = lg_bridge_state_json(&st, line, sizeof line); break;
        case 1: full = lg_bridge_event_json(&ev, line, sizeof line); break;
        case 2: full = lg_bridge_metric_json(1, "m", 5, line, sizeof line); break;
        default: full = lg_bridge_reply_json(0, "rejected", "too_long", line, sizeof line); break;
        }
        CHECK(full > 0);
        /* Exactly full + 1 bytes (room for the NUL) works and gives the same line; one byte less gives 0. */
        for (len = full - 1; len <= full + 1; len++) {
            size_t got;

            memset(small, 'Z', sizeof small);
            switch (round) {
            case 0: got = lg_bridge_state_json(&st, small, len); break;
            case 1: got = lg_bridge_event_json(&ev, small, len); break;
            case 2: got = lg_bridge_metric_json(1, "m", 5, small, len); break;
            default: got = lg_bridge_reply_json(0, "rejected", "too_long", small, len); break;
            }
            if (len <= full) {
                CHECK(got == 0 && small[0] == '\0'); /* no half line is ever left behind */
            } else {
                CHECK(got == full && memcmp(small, line, full + 1) == 0);
            }
        }
        for (len = 0; len < 5; len++) {
            size_t got = 1;

            switch (round) {
            case 0: got = lg_bridge_state_json(&st, small, len); break;
            case 1: got = lg_bridge_event_json(&ev, small, len); break;
            case 2: got = lg_bridge_metric_json(1, "m", 5, small, len); break;
            default: got = lg_bridge_reply_json(0, "rejected", "too_long", small, len); break;
            }
            CHECK(got == 0);
        }
    }
    CHECK(lg_bridge_state_json(&st, NULL, 100) == 0);
    CHECK(lg_bridge_metric_json(1, "m", 1, NULL, 100) == 0);
    CHECK(lg_bridge_reply_json(1, "a", NULL, NULL, 0) == 0);
}

/* ---- inbound ------------------------------------------------------------------------------------- */

static lg_bridge_in_t in_len(const char *s, size_t n) {
    lg_bridge_in_t in;

    lg_bridge_parse_in(&g_doc, s, n, &in);
    return in;
}

static lg_bridge_in_t in_str(const char *s) {
    return in_len(s, strlen(s));
}

static void test_inbound_corners(void) {
    lg_bridge_in_t in;
    char buf[LG_BRIDGE_IN_MAX + 8];
    static const char SUB[] = "{\"kind\":\"subscribe\"}";

    /* Embedded NUL: the length decides, not the C string. */
    in = in_len("{\"kind\":\"subscribe\"}\0{\"kind\":\"gate\"}", 20 + 1 + 15);
    CHECK(in.kind == LG_BRIDGE_IN_REJECT);
    in = in_len("{\"kind\":\"subscribe\"}\0", 21);
    CHECK(in.kind == LG_BRIDGE_IN_REJECT);
    in = in_len("{\"kind\":\"subscribe\0x\"}", 22);
    CHECK(in.kind == LG_BRIDGE_IN_REJECT);
    in = in_len("{\"kind\":\"subscribe\"}\0", 20); /* the NUL is not part of the line */
    CHECK(in.kind == LG_BRIDGE_IN_SUBSCRIBE);

    /* Line endings and spaces. */
    CHECK(in_str("{\"kind\":\"subscribe\"}\n").kind == LG_BRIDGE_IN_SUBSCRIBE);
    CHECK(in_str("{\"kind\":\"subscribe\"}\r\n").kind == LG_BRIDGE_IN_SUBSCRIBE);
    CHECK(in_str("  { \"kind\" : \"subscribe\" }  ").kind == LG_BRIDGE_IN_SUBSCRIBE);
    CHECK(in_str("\n").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"subscribe\"}{\"kind\":\"subscribe\"}").kind == LG_BRIDGE_IN_REJECT); /* trailing text */
    CHECK(in_str("{\"kind\":\"subscribe\"} x").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_len(NULL, 0).kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_len(SUB, 0).kind == LG_BRIDGE_IN_REJECT);
    lg_bridge_parse_in(NULL, SUB, sizeof SUB - 1, &in);
    CHECK(in.kind == LG_BRIDGE_IN_REJECT);

    /* Length limit: LG_BRIDGE_IN_MAX itself passes the length check, one more does not. */
    memset(buf, ' ', sizeof buf);
    memcpy(buf, SUB, sizeof SUB - 1);
    CHECK(in_len(buf, LG_BRIDGE_IN_MAX).kind == LG_BRIDGE_IN_SUBSCRIBE);
    in = in_len(buf, LG_BRIDGE_IN_MAX + 1);
    CHECK(in.kind == LG_BRIDGE_IN_REJECT && strcmp(in.reason, "too_long") == 0);
    in = in_len(buf, (size_t)-1); /* absurd length: must not read it */
    CHECK(in.kind == LG_BRIDGE_IN_REJECT && strcmp(in.reason, "too_long") == 0);

    /* Duplicate keys, nested values, wrong types. */
    CHECK(in_str("{\"kind\":\"subscribe\",\"kind\":\"subscribe\"}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"subscribe\",\"kind\":\"station_departure\"}").kind == LG_BRIDGE_IN_REJECT);
    in = in_str("{\"kind\":\"subscribe\",\"x\":{}}");
    CHECK(in.kind == LG_BRIDGE_IN_REJECT && strcmp(in.reason, "extra_field") == 0);
    in = in_str("{\"kind\":\"subscribe\",\"x\":[1,2]}");
    CHECK(in.kind == LG_BRIDGE_IN_REJECT && strcmp(in.reason, "extra_field") == 0);
    CHECK(in_str("{\"kind\":{\"kind\":\"subscribe\"}}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":[\"subscribe\"]}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":null}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("null").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("\"subscribe\"").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("42").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"Kind\":\"subscribe\"}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"Subscribe\"}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"subscribe \"}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"subscrib\\u0065\"}").kind == LG_BRIDGE_IN_REJECT); /* \u escapes are not accepted */
    CHECK(in_str("{\"kind\":\"subscribe\xc3\xa9\"}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"\xf0\x9f\x9a\x86\"}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"west\",\"eta_s\":5,\"eta_s\":6}").kind == LG_BRIDGE_IN_REJECT);

    /* eta_s boundaries. */
    in = in_str("{\"kind\":\"station_departure\",\"dir\":\"west\",\"eta_s\":0}");
    CHECK(in.kind == LG_BRIDGE_IN_STATION_DEPARTURE && in.eta_s == 0 && in.dir == LG_DIR_WEST);
    in = in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":86400}");
    CHECK(in.kind == LG_BRIDGE_IN_STATION_DEPARTURE && in.eta_s == 86400 && in.dir == LG_DIR_EAST);
    in = in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":86401}");
    CHECK(in.kind == LG_BRIDGE_IN_REJECT && strcmp(in.reason, "bad_field") == 0 && in.eta_s == 0);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":86400.001}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":5.0}").kind == LG_BRIDGE_IN_STATION_DEPARTURE ||
          in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":5.0}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":0.5}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":-0.001}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":1e3}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":+5}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":05}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":\"5\"}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":true}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":null}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":[5]}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":{\"a\":5}}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":99999999999999}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":4294967296}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":4294967295}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":9223372036854775807}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":-9223372036854775808}").kind == LG_BRIDGE_IN_REJECT);
    /* dir. */
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"West\",\"eta_s\":5}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"\",\"eta_s\":5}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":0,\"eta_s\":5}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":[\"west\"],\"eta_s\":5}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"west \",\"eta_s\":5}").kind == LG_BRIDGE_IN_REJECT);
    CHECK(in_str("{\"kind\":\"station_departure\",\"eta_s\":5,\"dir\":\"west\"}").kind == LG_BRIDGE_IN_STATION_DEPARTURE);
    CHECK(in_str("{\"kind\":\"station_departure\",\"dir\":\"west\",\"x\":5}").kind == LG_BRIDGE_IN_REJECT);

    /* A rejected line never leaks dir/eta from an earlier accepted one. */
    in = in_str("{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":77}");
    CHECK(in.kind == LG_BRIDGE_IN_STATION_DEPARTURE && in.eta_s == 77);
    in = in_str("{\"kind\":\"gate\"}");
    CHECK(in.kind == LG_BRIDGE_IN_REJECT && in.eta_s == 0 && in.dir == 0);

    /* Depth and node limits: rejected, never accepted and never a crash. */
    memset(buf, '[', 200);
    in = in_len(buf, 200);
    CHECK(in.kind == LG_BRIDGE_IN_REJECT);
    {
        char deep[LG_BRIDGE_IN_MAX];
        size_t n = 0;
        int k;

        n += (size_t)snprintf(deep, sizeof deep, "{\"kind\":\"subscribe\",\"x\":");
        for (k = 0; k < 40; k++) {
            deep[n++] = '[';
        }
        for (k = 0; k < 40; k++) {
            deep[n++] = ']';
        }
        deep[n++] = '}';
        CHECK(in_len(deep, n).kind == LG_BRIDGE_IN_REJECT);
    }
    /* Many members: 256 bytes can hold about 60 short keys; the node pool is not exceeded, the line is no subscribe. */
    {
        char many[LG_BRIDGE_IN_MAX];
        size_t n = 0;
        int k;

        many[n++] = '{';
        n += (size_t)snprintf(many + n, sizeof many - n, "\"kind\":\"subscribe\"");
        for (k = 0; k < 40 && n + 12 < sizeof many; k++) {
            n += (size_t)snprintf(many + n, sizeof many - n, ",\"k%d\":1", k);
        }
        many[n++] = '}';
        in = in_len(many, n);
        CHECK(in.kind == LG_BRIDGE_IN_REJECT);
    }
}

/* ---- recent ring --------------------------------------------------------------------------------- */

static lg_bridge_recent_t g_ring;

/* Line number k with a length that varies between 12 and about 211 bytes. */
static void make_line(char *out, size_t cap, uint32_t k) {
    size_t pad = (k * 37u) % 200u;
    size_t n = (size_t)snprintf(out, cap, "{\"seq\":%u,\"p\":\"", (unsigned)k);

    while (pad-- > 0 && n + 4 < cap) {
        out[n++] = 'p';
    }
    out[n++] = '"';
    out[n++] = '}';
    out[n++] = '\n';
    out[n] = '\0';
}

static void test_recent_ring_wraparound_and_budget(void) {
    char line[400];
    static char big[LG_BRIDGE_LINE_MAX + 1];
    uint32_t k;
    uint32_t total = 3 * LG_BRIDGE_RECENT + 5;
    size_t sum = 0;

    lg_bridge_recent_init(&g_ring);
    CHECK(g_ring.count == 0 && lg_bridge_recent_get(&g_ring, 0) == NULL);
    CHECK(lg_bridge_recent_select(&g_ring, 100000) == 0);
    CHECK(lg_bridge_recent_select(&g_ring, 0) == 0);
    lg_bridge_recent_push(&g_ring, "");
    CHECK(g_ring.count == 0); /* an empty line is not an event */

    for (k = 0; k < total; k++) {
        make_line(line, sizeof line, k);
        lg_bridge_recent_push(&g_ring, line);
        CHECK(g_ring.count == (k + 1 < LG_BRIDGE_RECENT ? k + 1 : LG_BRIDGE_RECENT));
        /* After every push the newest line is last and the oldest follows the count. */
        CHECK(strcmp(lg_bridge_recent_get(&g_ring, g_ring.count - 1), line) == 0);
    }
    CHECK(g_ring.count == LG_BRIDGE_RECENT);
    CHECK(lg_bridge_recent_get(&g_ring, LG_BRIDGE_RECENT) == NULL);
    CHECK(lg_bridge_recent_get(&g_ring, UINT32_MAX) == NULL);
    for (k = 0; k < LG_BRIDGE_RECENT; k++) {
        make_line(line, sizeof line, total - LG_BRIDGE_RECENT + k); /* oldest kept first, in order */
        CHECK(strcmp(lg_bridge_recent_get(&g_ring, k), line) == 0);
        sum += strlen(line);
    }
    CHECK(lg_bridge_recent_select(&g_ring, sum) == LG_BRIDGE_RECENT);
    CHECK(lg_bridge_recent_select(&g_ring, sum + 1) == LG_BRIDGE_RECENT);
    CHECK(lg_bridge_recent_select(&g_ring, sum - 1) == LG_BRIDGE_RECENT - 1);
    CHECK(lg_bridge_recent_select(&g_ring, (size_t)-1) == LG_BRIDGE_RECENT); /* no overflow with a huge budget */

    /* For every budget boundary: exactly the newest k lines that fit, never a partial one. */
    {
        size_t acc = 0;

        for (k = 1; k <= LG_BRIDGE_RECENT; k++) {
            acc += strlen(lg_bridge_recent_get(&g_ring, LG_BRIDGE_RECENT - k));
            CHECK(lg_bridge_recent_select(&g_ring, acc) == k);
            CHECK(lg_bridge_recent_select(&g_ring, acc - 1) == k - 1);
            CHECK(lg_bridge_recent_select(&g_ring, acc + 1) >= k);
        }
    }

    /* A line of exactly LG_BRIDGE_LINE_MAX - 1 bytes is stored; LG_BRIDGE_LINE_MAX bytes is dropped whole. */
    lg_bridge_recent_init(&g_ring);
    memset(big, 'x', LG_BRIDGE_LINE_MAX - 1);
    big[LG_BRIDGE_LINE_MAX - 1] = '\0';
    lg_bridge_recent_push(&g_ring, big);
    CHECK(g_ring.count == 1 && strlen(lg_bridge_recent_get(&g_ring, 0)) == LG_BRIDGE_LINE_MAX - 1);
    big[LG_BRIDGE_LINE_MAX - 1] = 'x';
    big[LG_BRIDGE_LINE_MAX] = '\0';
    lg_bridge_recent_push(&g_ring, big);
    CHECK(g_ring.count == 1 && g_ring.next == 1); /* nothing changed */
    big[LG_BRIDGE_LINE_MAX - 1] = '\0';           /* a storable line again */
    CHECK(lg_bridge_recent_select(&g_ring, LG_BRIDGE_LINE_MAX - 2) == 0);
    CHECK(lg_bridge_recent_select(&g_ring, LG_BRIDGE_LINE_MAX - 1) == 1);

    /* A short line overwriting a long slot leaves no remains of the long one. */
    lg_bridge_recent_init(&g_ring);
    for (k = 0; k < LG_BRIDGE_RECENT; k++) {
        lg_bridge_recent_push(&g_ring, big);
    }
    lg_bridge_recent_push(&g_ring, "{}");
    CHECK(g_ring.count == LG_BRIDGE_RECENT);
    CHECK(strcmp(lg_bridge_recent_get(&g_ring, LG_BRIDGE_RECENT - 1), "{}") == 0);
    CHECK(strlen(lg_bridge_recent_get(&g_ring, 0)) == LG_BRIDGE_LINE_MAX - 1);
}

/* ---- plant text -> bridge state ------------------------------------------------------------------ */

static lg_crossing_cfg_t g_cfg;
static lg_scenario_t g_sc;
static lg_plant_t g_plant;

static void add_train(uint16_t id, uint8_t dir, uint32_t t_strike_ms, int32_t speed_mmps, int32_t length_mm) {
    lg_train_t *t = &g_sc.trains[g_sc.n_trains++];

    t->id = id;
    t->dir = dir;
    t->axles = 8;
    t->t_strike_ms = t_strike_ms;
    t->speed_mmps = speed_mmps;
    t->length_mm = length_mm;
}

static void test_plant_text_roundtrip(void) {
    static char text[8192];
    lg_plant_pos_t pos[LG_SCENARIO_MAX_TRAINS];
    static lg_bridge_state_t st;
    char line[LG_BRIDGE_LINE_MAX];
    size_t n;
    size_t np;
    size_t i;

    memset(&g_cfg, 0, sizeof g_cfg);
    memset(&g_sc, 0, sizeof g_sc);
    g_cfg.strike_in_mm = 400000;
    g_cfg.island_mm = 12000;
    g_cfg.sensor_zone_mm = 2000;
    (void)snprintf(g_sc.name, sizeof g_sc.name, "rt");
    for (i = 0; i < 12; i++) {
        add_train((uint16_t)(65535 - i), (i & 1) ? LG_DIR_EAST : LG_DIR_WEST, 1000, 20000 + (int32_t)i * 1000, 120000 + (int32_t)i);
    }
    lg_plant_init(&g_plant, &g_cfg, &g_sc);

    /* Boundaries of visibility: front exactly 0 is not visible, front 1 mm is. */
    CHECK(lg_plant_positions(&g_plant, 1000, pos, LG_SCENARIO_MAX_TRAINS) == 0);
    CHECK(lg_plant_positions(&g_plant, 1001, pos, LG_SCENARIO_MAX_TRAINS) == 12);
    CHECK(pos[0].front_mm == 20 && pos[0].rear_mm == 20 - 120000);

    n = lg_plant_text(&g_plant, 11000, text, sizeof text);
    CHECK(n == strlen(text));
    np = lg_plant_positions(&g_plant, 11000, pos, LG_SCENARIO_MAX_TRAINS);
    CHECK(np == 12);
    memset(&st, 0, sizeof st);
    CHECK(lg_bridge_from_plant(&st, text) == 0);
    CHECK(st.plant_ms == 11000 && st.n_trains == LG_BRIDGE_MAX_TRAINS); /* 12 visible, bridge keeps 8 */
    for (i = 0; i < st.n_trains; i++) {
        CHECK(st.trains[i].id == pos[i].id && st.trains[i].dir == pos[i].dir);
        CHECK(st.trains[i].front_mm == pos[i].front_mm && st.trains[i].rear_mm == pos[i].rear_mm);
    }
    st.ctl_ok = st.gate_ok = 1;
    n = lg_bridge_state_json(&st, line, sizeof line);
    CHECK(valid_line(line, n) != NULL);

    /* Gone once the rear has passed 2 * 400000 + 12000: the first train (speed 20000, length 120000) leaves at
     * front = 932000 mm, i.e. t = 1000 + 46600 = 47600. Rear == 812000 exactly is already gone. */
    CHECK(lg_plant_positions(&g_plant, 47599, pos, LG_SCENARIO_MAX_TRAINS) >= 1 && pos[0].id == 65535);
    {
        lg_plant_pos_t p2[LG_SCENARIO_MAX_TRAINS];
        size_t a = lg_plant_positions(&g_plant, 47599, p2, LG_SCENARIO_MAX_TRAINS);
        size_t b = lg_plant_positions(&g_plant, 47600, p2, LG_SCENARIO_MAX_TRAINS);

        CHECK(b == a - 1 || a == 0);
    }

    /* A tiny text buffer: no half train lines, still parseable. */
    for (i = 80; i < 400; i += 7) {
        char tiny[400];

        n = lg_plant_text(&g_plant, 11000, tiny, i);
        CHECK(n == strlen(tiny) && n < i);
        CHECK(n == 0 || tiny[n - 1] == '\n');
        memset(&st, 0, sizeof st);
        CHECK(lg_bridge_from_plant(&st, tiny) == 0 || n < 20);
    }
}

int main(void) {
    test_kv_whole_words_and_numbers();
    test_device_text_corners();
    test_state_worst_case_fits_and_parses();
    test_hostile_text_stays_json();
    test_non_ascii_device_text_does_not_break_line();
    test_small_buffers();
    test_inbound_corners();
    test_recent_ring_wraparound_and_budget();
    test_plant_text_roundtrip();
    LG_TEST_DONE("test_bridge_proto_edge");
}
