#include <stdio.h>
#include <string.h>

#include "lg_controller_logic.h"
#include "lg_gate_model.h"
#include "lg_test.h"

/* Step 9: controller events (level changes, maintenance tickets) and the status fields. */

#define W LG_DIR_WEST
#define E LG_DIR_EAST
#define A LG_SENSOR_A
#define B LG_SENSOR_B
#define I LG_SENSOR_ISLAND

static uint8_t g_occ[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
static uint8_t g_mute[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
static uint8_t g_gate = LG_GATE_DOWN;
static lg_ctl_out_t g_last;

static void tick(lg_ctl_t *c, uint32_t now) {
    int d;
    int i;

    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            if (!g_mute[d][i]) {
                (void)lg_ctl_on_sample(c, (uint8_t)d, (uint8_t)i, g_occ[d][i], 97, 0, now, now);
            }
        }
    }
    (void)lg_ctl_on_gate_reply(c, g_gate, now);
    g_last = lg_ctl_step(c, now);
    g_gate = g_last.gate_target == LG_GATE_TARGET_DOWN ? (uint8_t)LG_GATE_DOWN : (uint8_t)LG_GATE_UP;
}

static void run(lg_ctl_t *c, uint32_t *t, uint32_t ms) {
    uint32_t end = *t + ms;

    for (; *t != end; *t += 10) {
        tick(c, *t);
    }
}

static void released(lg_ctl_t *c, uint32_t *t) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();

    memset(g_occ, 0, sizeof g_occ);
    memset(g_mute, 0, sizeof g_mute);
    g_gate = LG_GATE_DOWN;
    lg_ctl_init(c, &cfg, *t);
    run(c, t, LG_T_BOOT_STABLE_MS + 500);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
}

/* The newest event, or a zeroed one when there is none. */
static lg_ctl_event_t newest(const lg_ctl_t *c) {
    lg_ctl_event_t e;

    memset(&e, 0, sizeof e);
    if (c->events > 0) {
        CHECK(lg_ctl_events_since(c, c->events, &e, 1) == 1);
    }
    return e;
}

static unsigned field_hex(const char *text, const char *key) {
    const char *p = strstr(text, key);
    unsigned v = 0xFFFFFFFFu;

    if (p != NULL && sscanf(p + strlen(key), "%x", &v) != 1) {
        v = 0xFFFFFFFFu;
    }
    return v;
}

static unsigned field_u(const char *text, const char *key) {
    const char *p = strstr(text, key);
    unsigned v = 0xFFFFFFFFu;

    if (p != NULL && sscanf(p + strlen(key), "%u", &v) != 1) {
        v = 0xFFFFFFFFu;
    }
    return v;
}

static void test_boot_raises_no_ticket(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    lg_ctl_event_t e;

    released(&c, &t);
    CHECK(c.tickets == 0);
    CHECK(c.events == 1);
    e = newest(&c);
    CHECK(e.seq == 1);
    CHECK(e.kind == LG_EVT_LEVEL_CHANGE);
    CHECK(e.level_from == LG_LEVEL_FAILSAFE && e.level_to == LG_LEVEL_NORMAL);
    CHECK(e.code == LG_REASON_BOOT_RELEASED);
    CHECK(e.dir == LG_CTL_EVENT_NO_SENSOR && e.id == LG_CTL_EVENT_NO_SENSOR);
    CHECK(e.severity == LG_SEV_INFO);
}

static void test_silent_sensor_ticket_and_resolution(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    lg_ctl_event_t ev[8];
    uint32_t n;
    uint32_t base;

    released(&c, &t);
    base = c.events;
    g_mute[W][A] = 1;
    run(&c, &t, 400);
    CHECK(c.tickets == 1);
    CHECK(c.ticket_open[W][A] == 1);
    n = lg_ctl_events_since(&c, base + 1, ev, 8);
    CHECK(n == 2);
    /* The ticket comes first, then the level change it caused (same step). */
    CHECK(ev[0].kind == LG_EVT_NOTIFY && ev[0].severity == LG_SEV_URGENT);
    CHECK(ev[0].dir == W && ev[0].id == A && ev[0].code == LG_CTL_DIAG_SILENT);
    CHECK(ev[1].kind == LG_EVT_LEVEL_CHANGE && ev[1].level_from == 0 && ev[1].level_to == 2);
    CHECK(ev[1].code == LG_REASON_SENSOR_FAILED);
    CHECK(ev[0].t_ms == ev[1].t_ms);
    CHECK(ev[0].seq + 1 == ev[1].seq);

    /* Back: an INFO event closes the ticket; level 0 follows after T_stable with its own event. */
    g_mute[W][A] = 0;
    run(&c, &t, 100);
    CHECK(c.ticket_open[W][A] == 0);
    CHECK(c.tickets == 1);
    n = lg_ctl_events_since(&c, base + 3, ev, 8);
    CHECK(n == 1);
    CHECK(ev[0].kind == LG_EVT_NOTIFY && ev[0].severity == LG_SEV_INFO);
    CHECK(ev[0].dir == W && ev[0].id == A && ev[0].code == LG_CTL_DIAG_NONE);
    run(&c, &t, LG_T_STABLE_MS + 500);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    n = lg_ctl_events_since(&c, base + 4, ev, 8);
    CHECK(n == 1);
    CHECK(ev[0].kind == LG_EVT_LEVEL_CHANGE && ev[0].level_from == 2 && ev[0].level_to == 0);
    CHECK(ev[0].code == LG_REASON_STABLE);
}

static void test_missed_train_ticket(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    lg_ctl_event_t ev[8];
    uint32_t base;
    uint32_t n;
    uint32_t k;
    int found = 0;

    released(&c, &t);
    base = c.events;
    g_occ[E][A] = 1; /* east B stuck clear: A sees a 4 s train */
    run(&c, &t, 4000);
    g_occ[E][A] = 0;
    run(&c, &t, 1000);
    CHECK(c.tickets == 1);
    n = lg_ctl_events_since(&c, base + 1, ev, 8);
    for (k = 0; k < n; k++) {
        if (ev[k].kind == LG_EVT_NOTIFY && ev[k].severity == LG_SEV_URGENT) {
            found = 1;
            CHECK(ev[k].dir == E && ev[k].id == B && ev[k].code == LG_CTL_DIAG_MISSED_TRAIN);
        }
    }
    CHECK(found);
}

static void test_flapping_sensor_two_tickets(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    g_mute[E][I] = 1;
    run(&c, &t, 400);
    g_mute[E][I] = 0;
    run(&c, &t, 400);
    g_mute[E][I] = 1;
    run(&c, &t, 400);
    CHECK(c.tickets == 2);
    CHECK(c.ticket_open[E][I] == 1);
    CHECK(newest(&c).kind != 0);
}

/*
 * A crashed sensor process that the supervisor restarts within a few ms never looks silent to the
 * controller, but its RESTARTING status is level 2 (README 3) and needs a ticket as well.
 */
static void test_restarting_sensor_process_ticket(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    lg_ctl_event_t e;
    uint8_t west_b = (uint8_t)(LG_PROC_SENSOR_BASE + W * LG_SENSOR_ID_COUNT + B);

    released(&c, &t);
    CHECK(lg_ctl_on_proc_status(&c, west_b, LG_PROC_RESTARTING, 1, t) == 0);
    run(&c, &t, 20); /* the samples never stop */
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    CHECK(c.tickets == 1);
    CHECK(c.ticket_open[W][B] == 1);
    CHECK(lg_ctl_events_since(&c, 2, &e, 1) == 1);
    CHECK(e.kind == LG_EVT_NOTIFY && e.severity == LG_SEV_URGENT);
    CHECK(e.dir == W && e.id == B && e.code == LG_CTL_DIAG_RESTARTING);
    /* RUNNING again: the ticket closes; a second report of the same restart raises nothing new. */
    CHECK(lg_ctl_on_proc_status(&c, west_b, LG_PROC_RUNNING, 1, t) == 0);
    run(&c, &t, 20);
    CHECK(c.ticket_open[W][B] == 0);
    CHECK(lg_ctl_on_proc_status(&c, west_b, LG_PROC_RUNNING, 1, t) == 0);
    run(&c, &t, 20);
    CHECK(c.tickets == 1);
    /* Next crash: RESTARTING (ticket 2), then the budget is used up while it is still open: DEAD
     * escalates with a ticket of its own (ticket 3). */
    CHECK(lg_ctl_on_proc_status(&c, west_b, LG_PROC_RESTARTING, 2, t) == 0);
    run(&c, &t, 20);
    CHECK(c.tickets == 2);
    CHECK(lg_ctl_on_proc_status(&c, west_b, LG_PROC_DEAD, 3, t) == 0);
    run(&c, &t, 20);
    CHECK(c.tickets == 3);
    CHECK(c.ticket_open[W][B] == 1);
    CHECK(lg_ctl_events_since(&c, c.events, &e, 1) == 1);
    CHECK(e.kind == LG_EVT_NOTIFY && e.severity == LG_SEV_URGENT);
    CHECK(e.dir == W && e.id == B && e.code == LG_CTL_DIAG_PROC_DEAD);
    /* Staying DEAD raises nothing more. */
    run(&c, &t, 2000);
    CHECK(c.tickets == 3);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
}

static void test_ring_overflow_and_limits(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    lg_ctl_event_t ev[LG_CTL_EVENT_RING + 4];
    uint32_t n;
    uint32_t k;
    int round;

    released(&c, &t);
    for (round = 0; round < 20; round++) { /* 2 events each (ticket, info): 40 and more */
        g_mute[W][B] = 1;
        run(&c, &t, 300);
        g_mute[W][B] = 0;
        run(&c, &t, 100);
    }
    CHECK(c.events > LG_CTL_EVENT_RING);
    n = lg_ctl_events_since(&c, 0, ev, LG_CTL_EVENT_RING + 4);
    CHECK(n == LG_CTL_EVENT_RING);
    CHECK(ev[0].seq == c.events - (uint32_t)LG_CTL_EVENT_RING + 1u);
    for (k = 1; k < n; k++) {
        CHECK(ev[k].seq == ev[k - 1].seq + 1);
    }
    CHECK(n > 0 && ev[n - 1].seq == (uint32_t)c.events);
    CHECK(lg_ctl_events_since(&c, c.events + 1, ev, 8) == 0);
    CHECK(lg_ctl_events_since(&c, 0, ev, 3) == 3);
    CHECK(ev[0].seq == c.events - (uint32_t)LG_CTL_EVENT_RING + 1u); /* still the oldest first */
    CHECK(lg_ctl_events_since(&c, c.events - 1, ev, 8) == 2);
    CHECK(lg_ctl_events_since(&c, 0, ev, 0) == 0);
    CHECK(lg_ctl_events_since(&c, 0, NULL, 8) == 0);
}

static void test_event_text(void) {
    lg_ctl_event_t e;
    char buf[200];
    char tiny[12];
    size_t n;

    memset(&e, 0, sizeof e);
    e.seq = 7;
    e.t_ms = 12345;
    e.kind = LG_EVT_NOTIFY;
    e.severity = LG_SEV_URGENT;
    e.level_from = 0;
    e.level_to = 2;
    e.dir = W;
    e.id = A;
    e.code = LG_CTL_DIAG_MISSED_TRAIN;
    n = lg_ctl_event_text(&e, buf, sizeof buf);
    CHECK(n == strlen(buf));
    CHECK(n > 0 && buf[n - 1] == '\n');
    CHECK(strstr(buf, "seq=7 ") == buf);
    CHECK(strstr(buf, "t_ms=12345") != NULL);
    CHECK(strstr(buf, "kind=NOTIFY") != NULL);
    CHECK(strstr(buf, "severity=URGENT") != NULL);
    CHECK(strstr(buf, "to=maintenance") != NULL);
    CHECK(strstr(buf, "sensor=west/a") != NULL);
    CHECK(strstr(buf, "diag=MISSED_TRAIN") != NULL);

    e.kind = LG_EVT_LEVEL_CHANGE;
    e.severity = LG_SEV_INFO;
    e.dir = LG_CTL_EVENT_NO_SENSOR;
    e.id = LG_CTL_EVENT_NO_SENSOR;
    e.code = LG_REASON_SENSOR_FAILED;
    n = lg_ctl_event_text(&e, buf, sizeof buf);
    CHECK(strstr(buf, "kind=LEVEL") != NULL);
    CHECK(strstr(buf, "from=0 to=2 reason=4") != NULL);
    CHECK(strstr(buf, "sensor=") == NULL);

    e.kind = LG_EVT_NOTIFY;
    e.dir = E;
    e.id = I;
    e.code = LG_CTL_DIAG_NONE;
    (void)lg_ctl_event_text(&e, buf, sizeof buf);
    CHECK(strstr(buf, "sensor=east/island") != NULL);
    CHECK(strstr(buf, "diag=OK") != NULL);

    n = lg_ctl_event_text(&e, tiny, sizeof tiny);
    CHECK(n == sizeof tiny - 1 && tiny[sizeof tiny - 1] == '\0');
    CHECK(lg_ctl_event_text(&e, NULL, 10) == 0);
    CHECK(lg_ctl_event_text(NULL, buf, sizeof buf) == 0 && buf[0] == '\0');
}

/* The worst-case status line must fit the readers: lg-meter reads it into 2048 bytes since Step 12 (1024 in
 * Step 11), lg-controller keeps 1536 for it plus its process counters. */
static void test_status_worst_case_length(void) {
    lg_ctl_t c;
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    char buf[1024];
    size_t n;
    int d;
    int i;

    lg_ctl_init(&c, &cfg, 0);
    c.mode.level = 255;
    c.mode.boot_lock = 255;
    c.gate_silent = 255;
    c.last_lights = 255;
    c.last_bell = 255;
    c.active[0] = 255;
    c.active[1] = 255;
    c.activations = c.cycles = c.ticks = c.late_ticks = c.samples = c.samples_rejected = 0xFFFFFFFFu;
    c.gate_replies = c.gate_timeouts = c.proc_status_rx = c.proc_status_rejected = 0xFFFFFFFFu;
    c.guard_acks = c.tickets = c.events = 0xFFFFFFFFu;
    c.sup_armed = c.sup_silent = 255;
    c.health_armed = c.health_silent = 255;
    c.health_rx = c.health_rejected = c.warnings = c.lead_times = c.last_lead_time_ms = 0xFFFFFFFFu;
    c.health_unassessed = 0xFFFFFFFFu;
    c.stalls = 0xFFFFFFFFu;
    for (i = 0; i < LG_COMP_COUNT; i++) {
        c.comp_verdict[i] = LG_VERDICT_FAILED;
    }
    for (i = 0; i < LG_PROC_COUNT; i++) {
        c.proc_state[i] = LG_PROC_DEAD;
    }
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            c.last_verdict[d][i] = LG_VERDICT_FAILED;
        }
    }
    /* Step 12 fields at their widest. */
    for (d = 0; d < LG_DIR_COUNT; d++) {
        c.sched_ok[d] = 1;
        c.uncertain[d] = 1;
        c.win[d].confirmed_island = 0xFFFFFFFFu;
        c.win[d].confirmed_guard = 0xFFFFFFFFu;
        for (i = 0; i < LG_WIN_MAX; i++) {
            c.win[d].w[i].state = LG_WIN_OPEN;
        }
    }
    c.sched_rejected = 0xFFFFFFFFu;
    c.guard_confirms = 0xFFFFFFFFu;
    n = lg_ctl_text(&c, buf, sizeof buf);
    printf("status worst case: %u bytes\n", (unsigned)n);
    CHECK(n < 900); /* inside the 1024 of lg-meter (raised from 800 for the Step 12 fields) */
    CHECK(buf[n - 1] == '\n');
}

static void test_status_fields(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    char buf[1024];

    released(&c, &t);
    (void)lg_ctl_text(&c, buf, sizeof buf);
    CHECK(field_u(buf, " tickets=") == 0);
    CHECK(field_hex(buf, " sensor_failed=0x") == 0);
    CHECK(field_u(buf, " events=") == 1);
    CHECK(strstr(buf, "level=0 ") == buf); /* old fields keep their place */
    g_mute[E][B] = 1; /* bit 1 * 3 + 1 = 4 */
    run(&c, &t, 400);
    (void)lg_ctl_text(&c, buf, sizeof buf);
    CHECK(field_u(buf, " tickets=") == 1);
    CHECK(field_hex(buf, " sensor_failed=0x") == 0x10);
    CHECK(field_u(buf, " events=") == 3);
    CHECK(buf[strlen(buf) - 1] == '\n');
}

static void test_diag_names(void) {
    CHECK(strcmp(lg_ctl_diag_name(LG_CTL_DIAG_NONE), "OK") == 0);
    CHECK(strcmp(lg_ctl_diag_name(LG_CTL_DIAG_MISSED_TRAIN), "MISSED_TRAIN") == 0);
    CHECK(strcmp(lg_ctl_diag_name(LG_CTL_DIAG_STUCK_OCCUPIED), "STUCK_OCCUPIED") == 0);
    CHECK(strcmp(lg_ctl_diag_name(LG_CTL_DIAG_SILENT), "SILENT") == 0);
    CHECK(strcmp(lg_ctl_diag_name(LG_CTL_DIAG_STALE), "STALE") == 0);
    CHECK(strcmp(lg_ctl_diag_name(LG_CTL_DIAG_BLIND), "BLIND") == 0);
    CHECK(strcmp(lg_ctl_diag_name(LG_CTL_DIAG_RESTARTING), "RESTARTING") == 0);
    CHECK(strcmp(lg_ctl_diag_name(LG_CTL_DIAG_PROC_DEAD), "PROC_DEAD") == 0);
    CHECK(strcmp(lg_ctl_diag_name(99), "UNKNOWN") == 0);
}

int main(void) {
    test_boot_raises_no_ticket();
    test_silent_sensor_ticket_and_resolution();
    test_missed_train_ticket();
    test_flapping_sensor_two_tickets();
    test_restarting_sensor_process_ticket();
    test_ring_overflow_and_limits();
    test_event_text();
    test_status_worst_case_length();
    test_status_fields();
    test_diag_names();
    LG_TEST_DONE("test_controller_events");
}
