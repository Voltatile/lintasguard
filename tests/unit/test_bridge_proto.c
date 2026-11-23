#include <stdio.h>
#include <string.h>

#include "lg_bridge_proto.h"
#include "lg_json_mini.h"
#include "lg_proto.h"
#include "lg_test.h"

/* Step 10: lg-bridge host protocol (NDJSON out, strict whitelist in) and the device-text parsers. */

static lg_json_doc_t g_doc;
static lg_json_doc_t g_doc2;

/* The line is one JSON object followed by exactly one newline. */
static const lg_json_node_t *parse_line(const char *line, size_t n) {
    CHECK(n > 0 && line[n - 1] == '\n');
    CHECK(strchr(line, '\n') == line + n - 1);
    if (n == 0 || lg_json_parse(&g_doc2, line, n - 1) != LG_JSON_OK) {
        return NULL;
    }
    return lg_json_root(&g_doc2);
}

static int str_is(const lg_json_node_t *o, const char *key, const char *want) {
    const char *v = NULL;

    return o != NULL && lg_json_get_str(&g_doc2, o, key, &v) == 0 && strcmp(v, want) == 0;
}

static int64_t num(const lg_json_node_t *o, const char *key) {
    int64_t v = -999999;

    (void)lg_json_get_milli(&g_doc2, o, key, &v);
    return v;
}

static const char *CTL =
    "level=2 boot_lock=0 gate=DOWN gate_silent=0 target=DOWN lights=1 bell=1 active=1,0 activations=3 cycles=2 "
    "ticks=100 late_ticks=1 samples=600 rejected=0 gate_replies=99 gate_timeouts=0 sup_armed=1 sup_silent=0 "
    "proc_status_rx=5 proc_status_rejected=0 proc_bad=0x0 guard_acks=0 tickets=1 sensor_failed=0x1 events=3 "
    "unmet=0x0,0x0 stalls=4 ignored_ticks=0 gate_connects=1\n";
static const char *GATE = "state=LOWERING position=45 current_ma=900 travel_ms=0 fault=NONE factor_pct=100 watchdog_trips=0\n";
static const char *PLANT = "t_ms=30000 scenario=pos trains=2 waiting=0 active=2 finished=0 island=1\n"
                           "train id=1 dir=west front_mm=500000 rear_mm=380000\n"
                           "train id=2 dir=east front_mm=200000 rear_mm=50000\n";

static void test_kv(void) {
    uint32_t u = 0;
    int32_t i = 0;
    char s[16];

    CHECK(lg_bridge_kv_u32(CTL, "level", &u) == 0 && u == 2);
    CHECK(lg_bridge_kv_u32(CTL, "late_ticks", &u) == 0 && u == 1);
    CHECK(lg_bridge_kv_u32(CTL, "ticks", &u) == 0 && u == 100); /* not late_ticks */
    CHECK(lg_bridge_kv_u32(CTL, "sensor_failed", &u) == 0 && u == 1); /* hex 0x1 */
    CHECK(lg_bridge_kv_u32(CTL, "nope", &u) == -1);
    CHECK(lg_bridge_kv_u32(CTL, "gate", &u) == -1); /* DOWN is no number */
    CHECK(lg_bridge_kv_str(CTL, "gate", s, sizeof s) == 0 && strcmp(s, "DOWN") == 0);
    CHECK(lg_bridge_kv_str(CTL, "gate", s, 3) == -1); /* does not fit */
    CHECK(lg_bridge_kv_i32("front_mm=-120000 x=1", "front_mm", &i) == 0 && i == -120000);
    CHECK(lg_bridge_kv_u32("level=99999999999", "level", &u) == -1); /* overflow */
    CHECK(lg_bridge_kv_u32("level=", "level", &u) == -1);
    CHECK(lg_bridge_kv_u32("xlevel=3", "level", &u) == -1);
    CHECK(lg_bridge_kv_u32(NULL, "level", &u) == -1);
}

static void test_state(void) {
    lg_bridge_state_t st;
    char line[LG_BRIDGE_LINE_MAX];
    const lg_json_node_t *o;
    const lg_json_node_t *trains;
    size_t n;

    memset(&st, 0, sizeof st);
    st.t_ms = 12345;
    CHECK(lg_bridge_from_controller(&st, CTL) == 0);
    CHECK(lg_bridge_from_gate(&st, GATE) == 0);
    CHECK(lg_bridge_from_plant(&st, PLANT) == 0);
    CHECK(st.level == 2 && st.lights == 1 && st.active_west == 1 && st.active_east == 0);
    CHECK(st.tickets == 1 && st.stalls == 4 && st.sensor_failed == 1 && st.events == 3);
    CHECK(strcmp(st.gate_state, "LOWERING") == 0 && st.gate_position == 45);
    CHECK(st.plant_ms == 30000 && st.island == 1 && st.n_trains == 2);
    CHECK(st.trains[0].id == 1 && st.trains[0].dir == LG_DIR_WEST && st.trains[0].front_mm == 500000);
    CHECK(st.trains[1].id == 2 && st.trains[1].dir == LG_DIR_EAST && st.trains[1].rear_mm == 50000);

    n = lg_bridge_state_json(&st, line, sizeof line);
    o = parse_line(line, n);
    CHECK(o != NULL);
    CHECK(str_is(o, "kind", "state"));
    CHECK(num(o, "t") == 12345);
    CHECK(num(o, "level") == 2000);
    CHECK(str_is(o, "gate", "LOWERING"));
    CHECK(num(o, "gate_position") == 45000);
    CHECK(num(o, "stale") == 0);
    trains = lg_json_get(&g_doc2, o, "trains");
    CHECK(trains != NULL && trains->type == LG_JSON_ARRAY && trains->count == 2);

    /* Missing fields: parse fails, the line still builds with what is known. */
    memset(&st, 0, sizeof st);
    CHECK(lg_bridge_from_controller(&st, "garbage") == -1);
    CHECK(lg_bridge_from_gate(&st, "") == -1);
    CHECK(lg_bridge_from_plant(&st, "x=1") == -1);
    st.stale = 1;
    n = lg_bridge_state_json(&st, line, sizeof line);
    o = parse_line(line, n);
    CHECK(o != NULL && num(o, "stale") == 1000);
    CHECK(lg_bridge_state_json(&st, line, 20) == 0); /* does not fit: nothing half-written */

    /* More train lines than fit: capped, never overflowing. */
    {
        char big[2048];
        size_t used = 0;
        int k;

        used += (size_t)snprintf(big, sizeof big, "t_ms=1 island=0\n");
        for (k = 0; k < 20; k++) {
            used += (size_t)snprintf(big + used, sizeof big - used, "train id=%d dir=west front_mm=1 rear_mm=0\n", k);
        }
        memset(&st, 0, sizeof st);
        CHECK(lg_bridge_from_plant(&st, big) == 0);
        CHECK(st.n_trains == LG_BRIDGE_MAX_TRAINS);
    }
}

static void test_events(void) {
    lg_bridge_event_t ev;
    char line[LG_BRIDGE_LINE_MAX];
    const lg_json_node_t *o;
    size_t n;

    CHECK(lg_bridge_parse_event("seq=3 t_ms=24013929 kind=LEVEL severity=WARNING from=0 to=2 reason=4", &ev) == 0);
    CHECK(ev.seq == 3 && ev.kind == LG_BRIDGE_EV_LEVEL && ev.from == 0 && ev.to == 2 && ev.reason == 4);
    n = lg_bridge_event_json(&ev, line, sizeof line);
    o = parse_line(line, n);
    CHECK(str_is(o, "kind", "level") && num(o, "to") == 2000 && str_is(o, "reason", "SENSOR_FAILED"));
    CHECK(num(o, "t") == 24013929);
    CHECK(num(o, "seq") == 3000);

    CHECK(lg_bridge_parse_event(
              "seq=2 t_ms=10 kind=NOTIFY severity=URGENT to=maintenance sensor=west/a diag=STUCK_OCCUPIED level=0",
              &ev) == 0);
    CHECK(ev.kind == LG_BRIDGE_EV_NOTIFY && strcmp(ev.sensor, "west/a") == 0);
    n = lg_bridge_event_json(&ev, line, sizeof line);
    o = parse_line(line, n);
    CHECK(str_is(o, "kind", "notify") && str_is(o, "to", "maintenance") && str_is(o, "severity", "urgent"));
    CHECK(str_is(o, "sensor", "west/a") && str_is(o, "diag", "STUCK_OCCUPIED"));
    CHECK(o != NULL && lg_json_get(&g_doc2, o, "text") != NULL);

    CHECK(lg_bridge_parse_event("", &ev) == -1);
    CHECK(lg_bridge_parse_event("seq=1 t_ms=1 kind=UNKNOWN code=3", &ev) == -1);
    CHECK(lg_bridge_parse_event("hello world", &ev) == -1);
    /* A hostile token in a device text cannot break the JSON (quotes and backslashes escaped). */
    CHECK(lg_bridge_parse_event("seq=9 t_ms=1 kind=NOTIFY severity=URGENT to=maintenance sensor=a\"b\\c diag=X\"",
                                &ev) == 0);
    n = lg_bridge_event_json(&ev, line, sizeof line);
    CHECK(parse_line(line, n) != NULL);
    CHECK(strcmp(lg_bridge_reason_name(2), "BOOT_RELEASED") == 0);
    CHECK(strcmp(lg_bridge_reason_name(999), "UNKNOWN") == 0);
}

static void test_metric_and_reply(void) {
    char line[LG_BRIDGE_LINE_MAX];
    const lg_json_node_t *o;
    size_t n;

    n = lg_bridge_metric_json(5000, "closure_ms_per_hour", 1814451, line, sizeof line);
    o = parse_line(line, n);
    CHECK(str_is(o, "kind", "metric") && str_is(o, "name", "closure_ms_per_hour"));
    CHECK(num(o, "value") == 1814451000);
    n = lg_bridge_reply_json(1, "subscribe", NULL, line, sizeof line);
    o = parse_line(line, n);
    CHECK(str_is(o, "kind", "ack") && str_is(o, "what", "subscribe"));
    n = lg_bridge_reply_json(0, "rejected", "unknown_kind", line, sizeof line);
    o = parse_line(line, n);
    CHECK(str_is(o, "kind", "error") && str_is(o, "reason", "unknown_kind"));
}

static uint8_t in_kind(const char *s, char *reason_out) {
    lg_bridge_in_t in;

    lg_bridge_parse_in(&g_doc, s, strlen(s), &in);
    if (reason_out != NULL) {
        strcpy(reason_out, in.reason);
    }
    return in.kind;
}

static void test_inbound_whitelist(void) {
    lg_bridge_in_t in;
    char reason[40];
    char big[LG_BRIDGE_IN_MAX + 10];

    CHECK(in_kind("{\"kind\":\"subscribe\"}", NULL) == LG_BRIDGE_IN_SUBSCRIBE);
    lg_bridge_parse_in(&g_doc, "{\"kind\":\"station_departure\",\"dir\":\"east\",\"eta_s\":120}", 53, &in);
    CHECK(in.kind == LG_BRIDGE_IN_STATION_DEPARTURE && in.dir == LG_DIR_EAST && in.eta_s == 120);

    /* Everything that might look like a gate command, or anything else, is rejected. */
    CHECK(in_kind("{\"kind\":\"gate\",\"cmd\":\"raise\"}", reason) == LG_BRIDGE_IN_REJECT);
    CHECK(strcmp(reason, "unknown_kind") == 0);
    CHECK(in_kind("{\"kind\":\"gate_cmd\",\"cmd\":2}", NULL) == LG_BRIDGE_IN_REJECT);
    CHECK(in_kind("{\"kind\":\"guard_ack\",\"id\":\"G01\"}", NULL) == LG_BRIDGE_IN_REJECT);
    CHECK(in_kind("{\"kind\":\"subscribe\",\"cmd\":\"raise\"}", reason) == LG_BRIDGE_IN_REJECT);
    CHECK(strcmp(reason, "extra_field") == 0);
    CHECK(in_kind("{\"kind\":\"station_departure\",\"dir\":\"north\",\"eta_s\":1}", reason) == LG_BRIDGE_IN_REJECT);
    CHECK(strcmp(reason, "bad_field") == 0);
    CHECK(in_kind("{\"kind\":\"station_departure\",\"dir\":\"west\",\"eta_s\":-1}", NULL) == LG_BRIDGE_IN_REJECT);
    CHECK(in_kind("{\"kind\":\"station_departure\",\"dir\":\"west\",\"eta_s\":86401}", NULL) == LG_BRIDGE_IN_REJECT);
    CHECK(in_kind("{\"kind\":\"station_departure\",\"dir\":\"west\",\"eta_s\":1.5}", NULL) == LG_BRIDGE_IN_REJECT);
    CHECK(in_kind("{\"kind\":\"station_departure\",\"dir\":\"west\"}", NULL) == LG_BRIDGE_IN_REJECT);
    CHECK(in_kind("{\"kind\":\"station_departure\",\"dir\":\"west\",\"eta_s\":5,\"x\":1}", NULL) ==
          LG_BRIDGE_IN_REJECT);
    CHECK(in_kind("{\"kind\":1}", NULL) == LG_BRIDGE_IN_REJECT);
    CHECK(in_kind("[\"subscribe\"]", reason) == LG_BRIDGE_IN_REJECT && strcmp(reason, "not_object") == 0);
    CHECK(in_kind("subscribe", reason) == LG_BRIDGE_IN_REJECT && strcmp(reason, "not_json") == 0);
    CHECK(in_kind("", NULL) == LG_BRIDGE_IN_REJECT);
    memset(big, ' ', sizeof big);
    memcpy(big, "{\"kind\":\"subscribe\"}", 20);
    lg_bridge_parse_in(&g_doc, big, sizeof big, &in);
    CHECK(in.kind == LG_BRIDGE_IN_REJECT && strcmp(in.reason, "too_long") == 0);
    lg_bridge_parse_in(&g_doc, NULL, 3, &in);
    CHECK(in.kind == LG_BRIDGE_IN_REJECT);
}

static uint32_t g_rng = 20261122u;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/*
 * Mutation fuzz: random edits of valid lines. Whatever comes in, the result is one of the three
 * kinds, and an accepted line always has valid fields (seed printed, fixed).
 */
static void test_inbound_fuzz(void) {
    static const char *seeds[] = {"{\"kind\":\"subscribe\"}",
                                  "{\"kind\":\"station_departure\",\"dir\":\"west\",\"eta_s\":60}",
                                  "{\"kind\":\"gate\",\"cmd\":\"raise\"}"};
    static const char alphabet[] = "{}[]\",:01-9.aegkinrstuwxdb \\\n\t";
    char buf[LG_BRIDGE_IN_MAX + 8];
    lg_bridge_in_t in;
    uint32_t accepted = 0;
    uint32_t k;

    printf("bridge inbound fuzz seed=%u\n", (unsigned)g_rng);
    for (k = 0; k < 100000; k++) {
        const char *s = seeds[rnd() % 3];
        size_t n = strlen(s);
        int edits = (int)(rnd() % 4);
        int e;

        memcpy(buf, s, n);
        for (e = 0; e < edits; e++) {
            size_t pos = n ? rnd() % n : 0;

            switch (rnd() % 3) {
            case 0: buf[pos] = alphabet[rnd() % (sizeof alphabet - 1)]; break;
            case 1:
                if (n > 1) {
                    memmove(buf + pos, buf + pos + 1, n - pos - 1);
                    n--;
                }
                break;
            default:
                if (n + 1 < sizeof buf) {
                    memmove(buf + pos + 1, buf + pos, n - pos);
                    buf[pos] = alphabet[rnd() % (sizeof alphabet - 1)];
                    n++;
                }
                break;
            }
        }
        lg_bridge_parse_in(&g_doc, buf, n, &in);
        CHECK(in.kind <= LG_BRIDGE_IN_STATION_DEPARTURE);
        if (in.kind == LG_BRIDGE_IN_STATION_DEPARTURE) {
            CHECK(in.dir == LG_DIR_WEST || in.dir == LG_DIR_EAST);
            CHECK(in.eta_s <= 86400);
            accepted++;
        }
        if (in.kind == LG_BRIDGE_IN_REJECT) {
            CHECK(in.reason[0] != '\0');
        }
    }
    CHECK(accepted > 0);
}

/* Recent event lines, replayed to a client that connects later (a dashboard opened or reconnected after
 * a ticket must still see it). */
static lg_bridge_recent_t g_recent;

static void test_recent(void) {
    char line[64];
    uint32_t i;
    size_t one;

    lg_bridge_recent_init(&g_recent);
    CHECK(lg_bridge_recent_select(&g_recent, 100000) == 0);
    CHECK(lg_bridge_recent_get(&g_recent, 0) == NULL);

    for (i = 0; i < 3; i++) {
        (void)snprintf(line, sizeof line, "{\"seq\":%u}\n", (unsigned)(i + 1));
        lg_bridge_recent_push(&g_recent, line);
    }
    CHECK(lg_bridge_recent_select(&g_recent, 100000) == 3);
    CHECK(strcmp(lg_bridge_recent_get(&g_recent, 0), "{\"seq\":1}\n") == 0); /* oldest first */
    CHECK(strcmp(lg_bridge_recent_get(&g_recent, 2), "{\"seq\":3}\n") == 0);
    CHECK(lg_bridge_recent_get(&g_recent, 3) == NULL);

    /* Full: the oldest lines are overwritten, the newest LG_BRIDGE_RECENT are kept in order. */
    for (i = 3; i < LG_BRIDGE_RECENT + 8; i++) {
        (void)snprintf(line, sizeof line, "{\"seq\":%u}\n", (unsigned)(i + 1));
        lg_bridge_recent_push(&g_recent, line);
    }
    CHECK(lg_bridge_recent_select(&g_recent, 100000) == LG_BRIDGE_RECENT);
    (void)snprintf(line, sizeof line, "{\"seq\":%u}\n", 9u);
    CHECK(strcmp(lg_bridge_recent_get(&g_recent, 0), line) == 0);
    (void)snprintf(line, sizeof line, "{\"seq\":%u}\n", (unsigned)(LG_BRIDGE_RECENT + 8));
    CHECK(strcmp(lg_bridge_recent_get(&g_recent, LG_BRIDGE_RECENT - 1), line) == 0);

    /* A byte budget keeps the newest lines that fit (the replay must never overflow a client buffer). */
    one = strlen(lg_bridge_recent_get(&g_recent, LG_BRIDGE_RECENT - 1));
    CHECK(lg_bridge_recent_select(&g_recent, 5 * one) == 5);
    CHECK(lg_bridge_recent_select(&g_recent, 5 * one + 1) == 5);
    CHECK(lg_bridge_recent_select(&g_recent, one - 1) == 0);
    CHECK(lg_bridge_recent_select(&g_recent, 0) == 0);

    /* A line longer than a slot is not stored half (a broken JSON line would reach the browsers). */
    {
        static char big[LG_BRIDGE_LINE_MAX + 10];

        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        lg_bridge_recent_init(&g_recent);
        lg_bridge_recent_push(&g_recent, big);
        CHECK(lg_bridge_recent_select(&g_recent, 100000) == 0);
    }
}

/* Review finding M1: a missing controller or gate reading must not look like level 0 or a gate UP. */
static void test_unknown_fields_are_null(void) {
    lg_bridge_state_t st;
    char line[LG_BRIDGE_LINE_MAX];

    memset(&st, 0, sizeof st);
    CHECK(lg_bridge_state_json(&st, line, sizeof line) > 0);
    CHECK(strstr(line, "\"level\":null") != NULL);
    CHECK(strstr(line, "\"gate\":null") != NULL);
    CHECK(strstr(line, "\"gate_position\":null") != NULL);
    CHECK(parse_line(line, strlen(line)) != NULL);

    st.ctl_ok = 1;
    st.gate_ok = 1;
    st.level = 2;
    memcpy(st.gate_state, "DOWN", 5);
    st.gate_position = 90;
    CHECK(lg_bridge_state_json(&st, line, sizeof line) > 0);
    CHECK(strstr(line, "\"level\":2") != NULL);
    CHECK(strstr(line, "\"gate\":\"DOWN\"") != NULL);
    CHECK(strstr(line, "\"gate_position\":90") != NULL);

    st.gate_ok = 0; /* controller read, gate not */
    CHECK(lg_bridge_state_json(&st, line, sizeof line) > 0);
    CHECK(strstr(line, "\"level\":2") != NULL);
    CHECK(strstr(line, "\"gate\":null") != NULL);
}

static lg_bridge_event_t mk_ev(uint32_t seq, uint32_t t_ms) {
    lg_bridge_event_t e;

    memset(&e, 0, sizeof e);
    e.seq = seq;
    e.t_ms = t_ms;
    e.kind = LG_BRIDGE_EV_LEVEL;
    return e;
}

/* Review finding M2: which events of a ring read are new. A restarted controller numbers again from 1;
 * the bridge must not skip its first events because their numbers were already seen. */
static void test_event_tracker(void) {
    lg_bridge_evtrack_t k;
    lg_bridge_event_t ev[4];

    lg_bridge_evtrack_init(&k);
    CHECK(lg_bridge_evtrack_first_new(&k, ev, 0) == 0); /* empty ring, nothing */

    ev[0] = mk_ev(1, 1000);
    CHECK(lg_bridge_evtrack_first_new(&k, ev, 1) == 0); /* first read: all new */
    CHECK(lg_bridge_evtrack_first_new(&k, ev, 1) == 1); /* same ring again: none */
    ev[1] = mk_ev(2, 2000);
    ev[2] = mk_ev(3, 2000);
    CHECK(lg_bridge_evtrack_first_new(&k, ev, 3) == 1); /* only seq 2 and 3 */
    CHECK(lg_bridge_evtrack_first_new(&k, ev, 3) == 3);

    /* The controller restarts: its ring is empty for a while, then seq 1 (another time) appears. */
    CHECK(lg_bridge_evtrack_first_new(&k, ev, 0) == 0);
    ev[0] = mk_ev(1, 90000);
    CHECK(lg_bridge_evtrack_first_new(&k, ev, 1) == 0); /* new controller: sent */
    /* ... and grows past the old numbers: seq 2..4 of the new controller, all new once. */
    ev[1] = mk_ev(2, 91000);
    ev[2] = mk_ev(3, 92000);
    ev[3] = mk_ev(4, 93000);
    CHECK(lg_bridge_evtrack_first_new(&k, ev, 4) == 1);
    CHECK(lg_bridge_evtrack_first_new(&k, ev, 4) == 4);

    /* Restart where the new ring at the first read already holds as many events as the old one had. */
    lg_bridge_evtrack_init(&k);
    ev[0] = mk_ev(1, 100);
    ev[1] = mk_ev(2, 200);
    CHECK(lg_bridge_evtrack_first_new(&k, ev, 2) == 0);
    ev[0] = mk_ev(1, 5000);
    ev[1] = mk_ev(2, 5100);
    CHECK(lg_bridge_evtrack_first_new(&k, ev, 2) == 0);

    /* Same controller, the last seen event already left the ring (more than a ring of events between
     * two reads): every event in the ring is newer, all sent. */
    lg_bridge_evtrack_init(&k);
    ev[0] = mk_ev(5, 500);
    CHECK(lg_bridge_evtrack_first_new(&k, ev, 1) == 0);
    ev[0] = mk_ev(40, 4000);
    ev[1] = mk_ev(41, 4100);
    CHECK(lg_bridge_evtrack_first_new(&k, ev, 2) == 0);
}

int main(void) {
    test_unknown_fields_are_null();
    test_event_tracker();
    test_recent();
    test_kv();
    test_state();
    test_events();
    test_metric_and_reply();
    test_inbound_whitelist();
    test_inbound_fuzz();
    LG_TEST_DONE("test_bridge_proto");
}
