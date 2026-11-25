/*
 * Level 3 in the controller logic (Step 12, README 3, 8.2 rule 5, 8.4): timetable windows, island and guard
 * confirmation, information timeout, notifications to station, guard and road sign. Host only.
 *
 * Harness: 10 ms ticks, samples with sample_ms = rx_ms = now, a scripted gate (4000 ms travel, replies one
 * tick behind the command), and a schedule feed that repeats the configured reply every 500 ms (as
 * lg-schedule does). Every test checks the wrong-side property on its own scripted world: whenever an
 * island sensor reports a train, the scripted gate must be DOWN.
 */
#include <stdio.h>
#include <string.h>

#include "lg_config.h"
#include "lg_controller_logic.h"
#include "lg_test.h"
#include "lg_time.h"

#define TRAVEL_MS 4000u
#define LEAD_MS 20000u
#define TOL_MS 30000u

typedef struct {
    lg_ctl_t c;
    uint32_t o;
    uint8_t occ[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    uint8_t mute[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    uint16_t axles[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    uint8_t g;
    uint32_t gs;
    lg_ctl_out_t out;
    /* schedule feed */
    uint8_t feed[LG_DIR_COUNT]; /* 0 off, 1 with LG_SCHED_INFO, 2 without (outage) */
    lg_sched_reply_t reply[LG_DIR_COUNT];
    uint32_t wrong_side; /* ticks with a train on the island and the gate not DOWN */
} L;

static void l_init(L *h, const lg_ctl_cfg_t *cfg) {
    int d;

    memset(h, 0, sizeof *h);
    h->g = LG_GATE_DOWN;
    for (d = 0; d < LG_DIR_COUNT; d++) {
        h->reply[d].hdr.type = LG_MSG_SCHED_REPLY;
        h->reply[d].hdr.version = LG_PROTO_VERSION;
        h->reply[d].dir = (uint8_t)d;
        h->feed[d] = 1;
    }
    lg_ctl_init(&h->c, cfg, 0);
}

/* Adds a timetable entry to the feed of a direction. */
static void l_entry(L *h, int d, uint16_t id, uint32_t eta_o) {
    lg_sched_reply_t *r = &h->reply[d];
    lg_sched_entry_t *e = &r->entry[r->count++];

    memset(e, 0, sizeof *e);
    e->id = id;
    e->eta_ns = (uint64_t)eta_o * 1000000ull;
    e->lead_ms = LEAD_MS;
    e->tol_ms = TOL_MS;
    e->source = LG_SRC_TIMETABLE;
    e->confidence = LG_TT_CONF_TIMETABLE;
}

static void sim_gate(L *h, uint32_t t) {
    if (h->out.gate_target == LG_GATE_TARGET_DOWN) {
        if (h->g == LG_GATE_UP || h->g == LG_GATE_RAISING) {
            h->g = LG_GATE_LOWERING;
            h->gs = t;
        }
    } else if (h->g == LG_GATE_DOWN || h->g == LG_GATE_LOWERING) {
        h->g = LG_GATE_RAISING;
        h->gs = t;
    }
    if ((h->g == LG_GATE_LOWERING || h->g == LG_GATE_RAISING) && lg_elapsed_ge(t, h->gs, TRAVEL_MS)) {
        h->g = h->g == LG_GATE_LOWERING ? (uint8_t)LG_GATE_DOWN : (uint8_t)LG_GATE_UP;
    }
}

static lg_ctl_out_t tick(L *h) {
    int d;
    int i;

    h->o += 10;
    for (d = 0; d < LG_DIR_COUNT; d++) {
        if (h->feed[d] != 0 && h->o % 500 == 0) {
            h->reply[d].flags = h->feed[d] == 1 ? (uint8_t)LG_SCHED_INFO : 0u;
            CHECK(lg_ctl_on_sched_reply(&h->c, &h->reply[d], h->o) == 0);
        }
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            if (!h->mute[d][i]) {
                (void)lg_ctl_on_sample(&h->c, (uint8_t)d, (uint8_t)i, h->occ[d][i], 97, h->axles[d][i], h->o,
                                       h->o);
            }
        }
    }
    (void)lg_ctl_on_gate_reply(&h->c, h->g, h->o);
    h->out = lg_ctl_step(&h->c, h->o);
    if ((h->occ[0][LG_SENSOR_ISLAND] || h->occ[1][LG_SENSOR_ISLAND]) && h->g != LG_GATE_DOWN) {
        h->wrong_side++;
    }
    sim_gate(h, h->o);
    return h->out;
}

static void run_to(L *h, uint32_t o_end) {
    while (h->o < o_end) {
        (void)tick(h);
    }
}

/* Boot release, gate UP by o = 10000, level 0. */
static void boot(L *h) {
    run_to(h, 10000);
    CHECK(h->out.level == LG_LEVEL_NORMAL && h->g == LG_GATE_UP);
}

/* Both West approach sensors go silent: FAILED after sensor_silence_ms. */
static void west_lost(L *h) {
    h->mute[LG_DIR_WEST][LG_SENSOR_A] = 1;
    h->mute[LG_DIR_WEST][LG_SENSOR_B] = 1;
}

/* A train of direction d crosses the island from o_on to o_off; its axles count at the exit. */
static void island_train(L *h, int d, uint32_t o_on, uint32_t o_off, uint16_t axles) {
    run_to(h, o_on);
    h->occ[0][LG_SENSOR_ISLAND] = 1; /* the island sensors see any train */
    h->occ[1][LG_SENSOR_ISLAND] = 1;
    run_to(h, o_off);
    h->occ[0][LG_SENSOR_ISLAND] = 0;
    h->occ[1][LG_SENSOR_ISLAND] = 0;
    h->axles[d][LG_SENSOR_ISLAND] = (uint16_t)(h->axles[d][LG_SENSOR_ISLAND] + axles);
}

/* Is there a NOTIFY for target with code and dir, with seq > from? */
static int has_notify(const lg_ctl_t *c, uint32_t from, uint8_t target, uint8_t code, uint8_t dir, uint8_t sev) {
    lg_ctl_event_t ev[LG_CTL_EVENT_RING];
    uint32_t n = lg_ctl_events_since(c, from + 1, ev, LG_CTL_EVENT_RING);
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (ev[i].kind == LG_EVT_NOTIFY && ev[i].target == target && ev[i].code == code && ev[i].dir == dir &&
            ev[i].severity == sev) {
            return 1;
        }
    }
    return 0;
}

static int has_level(const lg_ctl_t *c, uint32_t from, uint8_t to, uint8_t reason) {
    lg_ctl_event_t ev[LG_CTL_EVENT_RING];
    uint32_t n = lg_ctl_events_since(c, from + 1, ev, LG_CTL_EVENT_RING);
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (ev[i].kind == LG_EVT_LEVEL_CHANGE && ev[i].level_to == to && ev[i].code == reason) {
            return 1;
        }
    }
    return 0;
}

/* ---- tests ---------------------------------------------------------------------------------------- */

/* Both West sensors lost with a timetable: level 3, notifications, gate UP outside the windows, DOWN in the
 * window, UP after the West train passed the island. */
static void test_window_protects_west_train(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    L h;
    uint32_t seq0;
    char text[LG_CTL_EVENT_TEXT_MAX];
    lg_ctl_event_t ev[LG_CTL_EVENT_RING];
    uint32_t n;
    uint32_t i;
    int found = 0;

    l_init(&h, &cfg);
    l_entry(&h, LG_DIR_WEST, 1, 60000); /* window 40000 .. 90000 */
    l_entry(&h, LG_DIR_EAST, 2, 500000);
    boot(&h);
    seq0 = h.c.events;
    west_lost(&h);
    run_to(&h, 11000);
    CHECK(h.out.level == LG_LEVEL_UNCERTAIN);
    CHECK(has_level(&h.c, seq0, LG_LEVEL_UNCERTAIN, LG_REASON_DETECT_LOST_SCHEDULE));
    CHECK(has_notify(&h.c, seq0, LG_CTL_TO_STATION, LG_CTL_DIAG_DETECTION_LOST, LG_DIR_WEST, LG_SEV_URGENT));
    CHECK(has_notify(&h.c, seq0, LG_CTL_TO_GUARD, LG_CTL_DIAG_DETECTION_LOST, LG_DIR_WEST, LG_SEV_URGENT));
    CHECK(has_notify(&h.c, seq0, LG_CTL_TO_ROAD_SIGN, LG_CTL_DIAG_DETECTION_LOST, LG_DIR_WEST, LG_SEV_WARNING));
    n = lg_ctl_events_since(&h.c, seq0 + 1, ev, LG_CTL_EVENT_RING);
    for (i = 0; i < n; i++) {
        if (ev[i].kind == LG_EVT_NOTIFY && ev[i].target == LG_CTL_TO_ROAD_SIGN) {
            (void)lg_ctl_event_text(&ev[i], text, sizeof text);
            found = strstr(text, "to=road_sign sensor=none diag=DETECTION_LOST level=3 component=none trend_s=-1 "
                                 "dir=west\n") != NULL;
        }
    }
    CHECK(found);
    /* Before the window: gate UP, lights off. */
    while (h.o < 39990) {
        (void)tick(&h);
        CHECK(h.out.gate_target == LG_GATE_TARGET_UP && !h.out.lights);
    }
    /* Window start: lights and gate down at once (level 3 has no lowering delay). */
    run_to(&h, 40010);
    CHECK(h.out.lights && h.out.gate_target == LG_GATE_TARGET_DOWN && h.c.active[LG_DIR_WEST]);
    CHECK(lg_win_count(&h.c.win[LG_DIR_WEST], LG_WIN_OPEN) == 1);
    /* The train is 2 s early, still well inside the window. */
    island_train(&h, LG_DIR_WEST, 58000, 62000, 8);
    run_to(&h, 63900);
    CHECK(h.out.gate_target == LG_GATE_TARGET_DOWN); /* island clear for T_clear not yet */
    run_to(&h, 64100);
    CHECK(h.out.gate_target == LG_GATE_TARGET_UP && !h.c.active[LG_DIR_WEST]);
    CHECK(h.c.win[LG_DIR_WEST].confirmed_island == 1 && !lg_win_open(&h.c.win[LG_DIR_WEST]));
    CHECK(h.out.level == LG_LEVEL_UNCERTAIN);
    run_to(&h, 100000);
    CHECK(h.out.gate_target == LG_GATE_TARGET_UP && h.wrong_side == 0);
}

/* An East train passing the island during a West window does not confirm the West window. */
static void test_other_direction_does_not_confirm(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    L h;

    l_init(&h, &cfg);
    l_entry(&h, LG_DIR_WEST, 1, 60000);
    boot(&h);
    west_lost(&h);
    run_to(&h, 41000);
    CHECK(lg_win_open(&h.c.win[LG_DIR_WEST]));
    /* East train: approach sensors see it and count it in, the island counts it out. */
    h.occ[LG_DIR_EAST][LG_SENSOR_A] = 1;
    h.occ[LG_DIR_EAST][LG_SENSOR_B] = 1;
    run_to(&h, 45000);
    h.occ[LG_DIR_EAST][LG_SENSOR_A] = 0;
    h.occ[LG_DIR_EAST][LG_SENSOR_B] = 0;
    h.axles[LG_DIR_EAST][LG_SENSOR_A] = 8;
    h.axles[LG_DIR_EAST][LG_SENSOR_B] = 8;
    island_train(&h, LG_DIR_EAST, 50000, 53000, 8);
    run_to(&h, 70000);
    CHECK(!h.c.active[LG_DIR_EAST]); /* the East cycle is cleared */
    CHECK(lg_win_open(&h.c.win[LG_DIR_WEST]) && h.c.active[LG_DIR_WEST]);
    CHECK(h.out.gate_target == LG_GATE_TARGET_DOWN && (h.c.last_unmet[LG_DIR_WEST] & LG_CTL_UNMET_WINDOW) != 0);
    /* Then the West train. */
    island_train(&h, LG_DIR_WEST, 72000, 75000, 6);
    run_to(&h, 78000);
    CHECK(h.out.gate_target == LG_GATE_TARGET_UP && h.wrong_side == 0);
}

/* No train in the window: still DOWN past its end, overdue notification to the guard, guard confirms. */
static void test_overdue_and_guard_confirm(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    L h;
    uint32_t seq0;

    l_init(&h, &cfg);
    l_entry(&h, LG_DIR_WEST, 1, 60000);
    boot(&h);
    west_lost(&h);
    /* A guard confirmation with nothing waiting changes nothing. */
    run_to(&h, 20000);
    CHECK(lg_ctl_on_guard_confirm(&h.c, LG_DIR_WEST) == 0);
    CHECK(lg_ctl_on_guard_confirm(&h.c, LG_DIR_COUNT) == -1);
    run_to(&h, 20100);
    CHECK(h.out.gate_target == LG_GATE_TARGET_UP && h.c.guard_confirms_used == 0);
    seq0 = h.c.events;
    run_to(&h, 90000);
    CHECK(!has_notify(&h.c, seq0, LG_CTL_TO_GUARD, LG_CTL_DIAG_TRAIN_OVERDUE, LG_DIR_WEST, LG_SEV_WARNING));
    run_to(&h, 90100);
    CHECK(has_notify(&h.c, seq0, LG_CTL_TO_GUARD, LG_CTL_DIAG_TRAIN_OVERDUE, LG_DIR_WEST, LG_SEV_WARNING));
    /* lg-schedule stops listing the entry (eta + tolerance passed): the window stays OPEN. */
    h.reply[LG_DIR_WEST].count = 0;
    run_to(&h, 200000);
    CHECK(h.out.gate_target == LG_GATE_TARGET_DOWN && lg_win_open(&h.c.win[LG_DIR_WEST]));
    CHECK(h.c.win[LG_DIR_WEST].overdue == 1);
    CHECK(lg_ctl_on_guard_confirm(&h.c, LG_DIR_WEST) == 0);
    (void)tick(&h);
    CHECK(h.out.gate_target == LG_GATE_TARGET_UP && h.c.win[LG_DIR_WEST].confirmed_guard == 1);
    CHECK(h.c.guard_confirms_used == 1 && h.wrong_side == 0);
}

/* Schedule information stops: level 3 until info_timeout_ms, then level 4 with notifications. */
static void test_info_timeout_level4(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    L h;
    uint32_t seq0;
    uint32_t stop;

    l_init(&h, &cfg);
    l_entry(&h, LG_DIR_WEST, 1, 600000);
    boot(&h);
    west_lost(&h);
    run_to(&h, 12000);
    CHECK(h.out.level == LG_LEVEL_UNCERTAIN);
    /* Outage: replies keep coming, without information. */
    h.feed[LG_DIR_WEST] = 2;
    h.feed[LG_DIR_EAST] = 2;
    stop = 12000; /* the last reply with information was at 12000 */
    seq0 = h.c.events;
    run_to(&h, stop + cfg.info_timeout_ms - 10);
    CHECK(h.out.level == LG_LEVEL_UNCERTAIN && h.out.gate_target == LG_GATE_TARGET_UP);
    run_to(&h, stop + cfg.info_timeout_ms + 10);
    CHECK(h.out.level == LG_LEVEL_FAILSAFE && h.out.gate_target == LG_GATE_TARGET_DOWN);
    CHECK(has_level(&h.c, seq0, LG_LEVEL_FAILSAFE, LG_REASON_DETECT_LOST_NO_INFO));
    CHECK(has_notify(&h.c, seq0, LG_CTL_TO_STATION, LG_CTL_DIAG_NO_SCHEDULE, LG_DIR_WEST, LG_SEV_URGENT));
    CHECK(has_notify(&h.c, seq0, LG_CTL_TO_GUARD, LG_CTL_DIAG_NO_SCHEDULE, LG_DIR_WEST, LG_SEV_URGENT));
    CHECK(has_notify(&h.c, seq0, LG_CTL_TO_ROAD_SIGN, LG_CTL_DIAG_NO_SCHEDULE, LG_DIR_WEST, LG_SEV_URGENT));
    CHECK(h.c.sched_ok[LG_DIR_WEST] == 0);
    /* Information back: level 4 stays (locked until a guard acknowledges). */
    h.feed[LG_DIR_WEST] = 1;
    h.feed[LG_DIR_EAST] = 1;
    run_to(&h, h.o + 40000);
    CHECK(h.out.level == LG_LEVEL_FAILSAFE);
    /* Sensors repaired, guard acknowledges: level 4 is left after T_stable. */
    h.mute[LG_DIR_WEST][LG_SENSOR_A] = 0;
    h.mute[LG_DIR_WEST][LG_SENSOR_B] = 0;
    run_to(&h, h.o + 1000);
    lg_ctl_on_guard_ack(&h.c);
    run_to(&h, h.o + LG_T_STABLE_MS + 1000);
    CHECK(h.out.level < LG_LEVEL_UNCERTAIN && h.wrong_side == 0);
}

/* Never any schedule information: both West sensors lost is level 4 (as before Step 12). A schedule
 * process given up (DEAD) is level 4 at once, without waiting for the timeout. */
static void test_no_info_and_dead_schedule(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    L h;

    l_init(&h, &cfg);
    h.feed[0] = 0;
    h.feed[1] = 0;
    boot(&h);
    west_lost(&h);
    run_to(&h, 11000);
    CHECK(h.out.level == LG_LEVEL_FAILSAFE && h.out.level_reason == LG_REASON_DETECT_LOST_NO_INFO);

    l_init(&h, &cfg);
    l_entry(&h, LG_DIR_WEST, 1, 600000);
    boot(&h);
    west_lost(&h);
    run_to(&h, 11000);
    CHECK(h.out.level == LG_LEVEL_UNCERTAIN);
    CHECK(lg_ctl_on_proc_status(&h.c, LG_PROC_SCHEDULE, LG_PROC_DEAD, 4, h.o) == 0);
    (void)tick(&h);
    CHECK(h.out.level == LG_LEVEL_FAILSAFE && h.c.sched_ok[LG_DIR_WEST] == 0);
    /* A restarting lg-schedule is level 2 by itself and does not drop the information. */
    l_init(&h, &cfg);
    l_entry(&h, LG_DIR_WEST, 1, 600000);
    boot(&h);
    west_lost(&h);
    run_to(&h, 11000);
    CHECK(lg_ctl_on_proc_status(&h.c, LG_PROC_SCHEDULE, LG_PROC_RESTARTING, 1, h.o) == 0);
    run_to(&h, 12000);
    CHECK(h.out.level == LG_LEVEL_UNCERTAIN && h.c.sched_ok[LG_DIR_WEST] == 1);
}

static void test_reply_validation(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    L h;
    lg_sched_reply_t r;
    uint32_t rej;

    l_init(&h, &cfg);
    h.feed[0] = 0;
    h.feed[1] = 0;
    l_entry(&h, LG_DIR_WEST, 1, 60000);
    r = h.reply[LG_DIR_WEST];
    r.flags = LG_SCHED_INFO;
    CHECK(lg_ctl_on_sched_reply(&h.c, &r, 0) == 0 && h.c.sched_heard[LG_DIR_WEST]);
    CHECK(lg_ctl_on_sched_reply(&h.c, NULL, 0) == -1);
    rej = h.c.sched_rejected;
#define BAD(field_change)                                                                                  \
    do {                                                                                                   \
        r = h.reply[LG_DIR_WEST];                                                                          \
        r.flags = LG_SCHED_INFO;                                                                           \
        field_change;                                                                                      \
        CHECK(lg_ctl_on_sched_reply(&h.c, &r, 0) == -1);                                                   \
    } while (0)
    BAD(r.dir = LG_DIR_COUNT);
    BAD(r.count = LG_SCHED_ENTRIES + 1);
    BAD(r.entry[0].id = 0);
    BAD(r.entry[0].source = 2);
    BAD(r.entry[0].confidence = 0);
    BAD(r.entry[0].confidence = 101);
    BAD(r.entry[0].lead_ms = LG_TT_LEAD_MAX_MS + 1);
    BAD(r.entry[0].tol_ms = LG_TT_TOL_MAX_MS + 1);
    BAD(r.entry[0].eta_ns = (uint64_t)(2ull * LG_TT_ETA_MAX_MS + 1000ull) * 1000000ull);
#undef BAD
    CHECK(h.c.sched_rejected == rej + 9);
    /* A refused reply refreshes nothing: the information timer still runs from 0. */
    h.c.sched_rx_ms[LG_DIR_WEST] = 0;
    r = h.reply[LG_DIR_WEST];
    r.flags = LG_SCHED_INFO;
    r.entry[0].id = 0;
    CHECK(lg_ctl_on_sched_reply(&h.c, &r, 5000) == -1 && h.c.sched_rx_ms[LG_DIR_WEST] == 0);
    /* Without LG_SCHED_INFO: windows are updated, the timer is not refreshed. */
    r = h.reply[LG_DIR_WEST];
    r.flags = 0;
    CHECK(lg_ctl_on_sched_reply(&h.c, &r, 6000) == 0 && h.c.sched_rx_ms[LG_DIR_WEST] == 0);
}

/* A window opened in level 3 keeps the gate down after the level went down again, until a train of the
 * direction passes the island; leaving level 3 tells the station and the road sign. */
static void test_open_window_survives_level_drop(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    L h;
    uint32_t seq0;

    l_init(&h, &cfg);
    l_entry(&h, LG_DIR_WEST, 1, 40000); /* window 20000 .. 70000 */
    boot(&h);
    west_lost(&h);
    run_to(&h, 21000);
    CHECK(h.out.level == LG_LEVEL_UNCERTAIN && lg_win_open(&h.c.win[LG_DIR_WEST]));
    /* Sensors repaired: level 3 is left after T_stable, the window is still OPEN. */
    seq0 = h.c.events;
    h.mute[LG_DIR_WEST][LG_SENSOR_A] = 0;
    h.mute[LG_DIR_WEST][LG_SENSOR_B] = 0;
    run_to(&h, 21000 + LG_T_STABLE_MS + 500);
    CHECK(h.out.level < LG_LEVEL_UNCERTAIN);
    CHECK(has_notify(&h.c, seq0, LG_CTL_TO_STATION, LG_CTL_DIAG_NONE, LG_DIR_WEST, LG_SEV_INFO));
    CHECK(has_notify(&h.c, seq0, LG_CTL_TO_ROAD_SIGN, LG_CTL_DIAG_NONE, LG_DIR_WEST, LG_SEV_INFO));
    CHECK(h.out.gate_target == LG_GATE_TARGET_DOWN && lg_win_open(&h.c.win[LG_DIR_WEST]));
    /* The (late) train passes the repaired approach sensors (counted in), then the island: the window is
     * confirmed with the level 3 rules. */
    run_to(&h, 55000);
    h.occ[LG_DIR_WEST][LG_SENSOR_A] = 1;
    h.occ[LG_DIR_WEST][LG_SENSOR_B] = 1;
    run_to(&h, 58000);
    h.occ[LG_DIR_WEST][LG_SENSOR_A] = 0;
    h.occ[LG_DIR_WEST][LG_SENSOR_B] = 0;
    h.axles[LG_DIR_WEST][LG_SENSOR_A] = 8;
    h.axles[LG_DIR_WEST][LG_SENSOR_B] = 8;
    island_train(&h, LG_DIR_WEST, 60000, 63000, 8);
    run_to(&h, 66000);
    CHECK(h.out.gate_target == LG_GATE_TARGET_UP && !lg_win_open(&h.c.win[LG_DIR_WEST]) && h.wrong_side == 0);
}

/* East island lost: level 3; an East train is detected by its approach sensors and cleared only by a guard. */
static void test_island_lost_needs_guard(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    L h;

    l_init(&h, &cfg);
    l_entry(&h, LG_DIR_EAST, 1, 900000);
    boot(&h);
    h.mute[LG_DIR_EAST][LG_SENSOR_ISLAND] = 1;
    run_to(&h, 11000);
    CHECK(h.out.level == LG_LEVEL_UNCERTAIN && h.out.gate_target == LG_GATE_TARGET_UP);
    h.occ[LG_DIR_EAST][LG_SENSOR_A] = 1;
    h.occ[LG_DIR_EAST][LG_SENSOR_B] = 1;
    run_to(&h, 15000);
    h.occ[LG_DIR_EAST][LG_SENSOR_A] = 0;
    h.occ[LG_DIR_EAST][LG_SENSOR_B] = 0;
    island_train(&h, LG_DIR_EAST, 20000, 23000, 8); /* only the West island sensor reports it */
    run_to(&h, 40000);
    CHECK(h.out.gate_target == LG_GATE_TARGET_DOWN && h.c.active[LG_DIR_EAST]);
    CHECK(lg_ctl_on_guard_confirm(&h.c, LG_DIR_EAST) == 0);
    (void)tick(&h);
    CHECK(h.out.gate_target == LG_GATE_TARGET_UP && !h.c.active[LG_DIR_EAST] && h.c.guard_confirms_used == 1);
    CHECK(h.wrong_side == 0);
}

/*
 * Step 12 run 1 on QNX: an East train passed under normal detection inside its window; later both East
 * sensors are lost for a while. The window of the train that already passed must not open (it would keep
 * the gate down until a guard came).
 */
static void test_passed_train_window_not_reopened(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    L h;

    l_init(&h, &cfg);
    l_entry(&h, LG_DIR_EAST, 6, 40000); /* window 20000 .. 70000 */
    boot(&h);
    run_to(&h, 30000);
    h.occ[LG_DIR_EAST][LG_SENSOR_A] = 1;
    h.occ[LG_DIR_EAST][LG_SENSOR_B] = 1;
    run_to(&h, 33000);
    h.occ[LG_DIR_EAST][LG_SENSOR_A] = 0;
    h.occ[LG_DIR_EAST][LG_SENSOR_B] = 0;
    h.axles[LG_DIR_EAST][LG_SENSOR_A] = 8;
    h.axles[LG_DIR_EAST][LG_SENSOR_B] = 8;
    island_train(&h, LG_DIR_EAST, 45000, 48000, 8); /* gate down by 39 s: 5 s pre-warning + 4 s travel */
    run_to(&h, 55000);
    CHECK(h.out.gate_target == LG_GATE_TARGET_UP && h.c.win[LG_DIR_EAST].confirmed_normal == 1);
    /* Both East sensors silent (a VM stall, a cable): level 3, but no window opens. */
    h.mute[LG_DIR_EAST][LG_SENSOR_A] = 1;
    h.mute[LG_DIR_EAST][LG_SENSOR_B] = 1;
    run_to(&h, 60000);
    CHECK(h.out.level == LG_LEVEL_UNCERTAIN && h.c.uncertain[LG_DIR_EAST]);
    CHECK(!lg_win_open(&h.c.win[LG_DIR_EAST]) && h.c.win[LG_DIR_EAST].opened == 0);
    CHECK(h.out.gate_target == LG_GATE_TARGET_UP && h.wrong_side == 0);
}

/* In level 4 a guard confirmation does nothing (level 4 is left only through guard-ack). */
static void test_guard_confirm_in_level4(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    L h;

    l_init(&h, &cfg);
    h.feed[0] = 0;
    h.feed[1] = 0;
    boot(&h);
    west_lost(&h);
    run_to(&h, 11000);
    CHECK(h.out.level == LG_LEVEL_FAILSAFE);
    CHECK(lg_ctl_on_guard_confirm(&h.c, LG_DIR_WEST) == 0);
    run_to(&h, 12000);
    CHECK(h.out.level == LG_LEVEL_FAILSAFE && h.out.gate_target == LG_GATE_TARGET_DOWN);
    CHECK(h.c.guard_confirms_used == 0);
}

/* The status line carries the level 3 fields. */
static void test_status_fields(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    L h;
    char buf[1024];

    l_init(&h, &cfg);
    l_entry(&h, LG_DIR_WEST, 1, 60000);
    boot(&h);
    west_lost(&h);
    run_to(&h, 41000);
    (void)lg_ctl_text(&h.c, buf, sizeof buf);
    CHECK(strstr(buf, " sched_ok=1,1 uncertain=1,0 win_open=1,0 win_island=0,0 win_guard=0,0 ") != NULL);
}

int main(void) {
    test_window_protects_west_train();
    test_other_direction_does_not_confirm();
    test_overdue_and_guard_confirm();
    test_info_timeout_level4();
    test_no_info_and_dead_schedule();
    test_reply_validation();
    test_open_window_survives_level_drop();
    test_island_lost_needs_guard();
    test_passed_train_window_not_reopened();
    test_guard_confirm_in_level4();
    test_status_fields();
    LG_TEST_DONE("test_controller_level3");
}
