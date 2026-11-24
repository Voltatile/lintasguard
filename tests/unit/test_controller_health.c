#include <stdio.h>
#include <string.h>

#include "lg_controller_logic.h"
#include "lg_gate_model.h"
#include "lg_health.h"
#include "lg_test.h"

/* Step 11: lg-health verdicts in the controller, level 1, early-warning notifications, lead time. */

#define W LG_DIR_WEST
#define E LG_DIR_EAST
#define A LG_SENSOR_A
#define B LG_SENSOR_B
#define I LG_SENSOR_ISLAND
#define COMP(d, i) ((uint8_t)((d) * LG_SENSOR_ID_COUNT + (i)))
#define HB(d) ((uint8_t)(LG_CTL_DIAG_HEALTH_BASE + (d)))

static uint8_t g_occ[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
static uint8_t g_gate = LG_GATE_DOWN;
static lg_ctl_out_t g_last;

/* What lg-health says, sent every 500 ms while g_health_on. */
static uint8_t g_health_on;
static uint8_t g_hv[LG_COMP_COUNT];
static uint16_t g_hd[LG_COMP_COUNT];
static int32_t g_ht[LG_COMP_COUNT];

static void health_all_ok(void) {
    int k;

    for (k = 0; k < LG_COMP_COUNT; k++) {
        g_hv[k] = LG_VERDICT_OK;
        g_hd[k] = LG_DIAG_NONE;
        g_ht[k] = -1;
    }
}

static void say(uint8_t comp, uint8_t verdict, uint16_t diag, int32_t trend) {
    g_hv[comp] = verdict;
    g_hd[comp] = diag;
    g_ht[comp] = trend;
}

static void tick(lg_ctl_t *c, uint32_t now) {
    int d;
    int i;

    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            (void)lg_ctl_on_sample(c, (uint8_t)d, (uint8_t)i, g_occ[d][i], 97, 0, now, now);
        }
    }
    if (g_health_on && now % 500u == 0) {
        int k;

        for (k = 0; k < LG_COMP_COUNT; k++) {
            uint8_t score = g_hv[k] == LG_VERDICT_OK ? 100 : g_hv[k] == LG_VERDICT_WARNING ? 60 : 20;

            CHECK(lg_ctl_on_health_verdict(c, (uint8_t)k, score, g_hv[k], g_hd[k], g_ht[k], now) == 0);
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

static void start(lg_ctl_t *c, uint32_t *t, uint8_t required, uint8_t health_on) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();

    memset(g_occ, 0, sizeof g_occ);
    g_gate = LG_GATE_DOWN;
    health_all_ok();
    g_health_on = health_on;
    cfg.health_required = required;
    lg_ctl_init(c, &cfg, *t);
    run(c, t, LG_T_BOOT_STABLE_MS + 500);
}

/* The events after `base`, at most 16. */
static uint32_t events_after(const lg_ctl_t *c, uint32_t base, lg_ctl_event_t ev[16]) {
    return lg_ctl_events_since(c, base + 1, ev, 16);
}

static uint32_t count_kind(const lg_ctl_event_t *ev, uint32_t n, uint8_t kind, uint8_t sev) {
    uint32_t k;
    uint32_t m = 0;

    for (k = 0; k < n; k++) {
        if (ev[k].kind == kind && (sev == 0xFF || ev[k].severity == sev)) {
            m++;
        }
    }
    return m;
}

static void test_verdict_validation(void) {
    lg_ctl_t c;
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();

    lg_ctl_init(&c, &cfg, 0);
    CHECK(lg_ctl_on_health_verdict(&c, LG_COMP_COUNT, 100, LG_VERDICT_OK, 0, -1, 0) == -1);
    CHECK(lg_ctl_on_health_verdict(&c, 0, 101, LG_VERDICT_OK, 0, -1, 0) == -1);
    CHECK(c.health_rejected == 2 && c.health_armed == 0);
    CHECK(lg_ctl_on_health_verdict(&c, 0, 50, 7, LG_DIAG_DROPOUTS, -9, 0) == 0);
    CHECK(c.health_verdict[0] == LG_VERDICT_FAILED); /* unknown verdict fails closed */
    CHECK(c.health_trend_s[0] == -1);
    CHECK(c.health_armed == 1 && c.health_rx == 1);
}

static void test_not_installed_changes_nothing(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    start(&c, &t, 0, 0);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    CHECK(c.health_armed == 0 && c.health_silent == 0);
    CHECK(c.events == 1); /* only the boot release */
}

static void test_required_holds_the_boot_lock(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    start(&c, &t, 1, 0);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    CHECK(c.mode.boot_lock == 1);
    g_health_on = 1;
    run(&c, &t, LG_T_BOOT_STABLE_MS + 1000);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
}

/*
 * A start-up transient (found on QNX, Step 11 part 1): the sensors send blind samples until the plant
 * answers, lg-health reports DROPOUTS for a moment while level 4 (boot) still holds the gate down. That is
 * no maintenance case: no notification before the boot release, like the Step 9 tickets.
 */
static void test_boot_transient_raises_nothing(void) {
    lg_ctl_t c;
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    uint32_t t = 0;

    memset(g_occ, 0, sizeof g_occ);
    g_gate = LG_GATE_DOWN;
    health_all_ok();
    g_health_on = 1;
    cfg.health_required = 1;
    lg_ctl_init(&c, &cfg, t);
    run(&c, &t, 1000); /* verdicts OK (no data yet) */
    say(COMP(W, A), LG_VERDICT_WARNING, LG_DIAG_DROPOUTS, -1);
    say(COMP(E, I), LG_VERDICT_WARNING, LG_DIAG_DROPOUTS, -1);
    run(&c, &t, 2500);
    health_all_ok();
    run(&c, &t, LG_T_BOOT_STABLE_MS + 1000);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    CHECK(c.warnings == 0 && c.tickets == 0);
    CHECK(c.events == 1); /* only the boot release */
    /* After the release a warning is a maintenance case again. */
    say(COMP(W, A), LG_VERDICT_WARNING, LG_DIAG_DROPOUTS, -1);
    run(&c, &t, 1000);
    CHECK(c.warnings == 1);
}

/* DRIFT seen by lg-health: WARNING (level 1, one notification), then FAILED (ticket, lead time, level 2). */
static void test_warning_then_failed_sensor(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t base;
    uint32_t t_warn;
    uint32_t n;
    lg_ctl_event_t ev[16];
    char text[LG_CTL_EVENT_TEXT_MAX];

    start(&c, &t, 1, 1);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    base = c.events;
    say(COMP(W, A), LG_VERDICT_WARNING, LG_DIAG_NOISE_RISING, 42);
    run(&c, &t, 3000);
    CHECK(g_last.level == LG_LEVEL_EARLY_WARNING);
    n = events_after(&c, base, ev);
    CHECK(n == 2);
    CHECK(ev[0].kind == LG_EVT_NOTIFY && ev[0].severity == LG_SEV_WARNING);
    CHECK(ev[0].comp == COMP(W, A) && ev[0].dir == W && ev[0].id == A);
    CHECK(ev[0].code == HB(LG_DIAG_NOISE_RISING));
    CHECK(ev[0].value == 42);
    CHECK(ev[1].kind == LG_EVT_LEVEL_CHANGE && ev[1].level_to == LG_LEVEL_EARLY_WARNING);
    CHECK(ev[1].code == LG_REASON_WARNING);
    CHECK(c.warnings == 1 && c.tickets == 0);
    (void)lg_ctl_event_text(&ev[0], text, sizeof text);
    CHECK(strstr(text, "severity=WARNING") != NULL && strstr(text, "sensor=west/a") != NULL);
    CHECK(strstr(text, "diag=NOISE_RISING") != NULL && strstr(text, "component=west/a") != NULL);
    CHECK(strstr(text, "trend_s=42") != NULL);
    t_warn = ev[0].t_ms;

    base = c.events;
    say(COMP(W, A), LG_VERDICT_FAILED, LG_DIAG_NOISE_HIGH, 0);
    run(&c, &t, 1000);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_NOTIFY, LG_SEV_URGENT) == 1);
    CHECK(count_kind(ev, n, LG_EVT_METRIC, 0xFF) == 1);
    CHECK(ev[0].kind == LG_EVT_NOTIFY && ev[0].code == HB(LG_DIAG_NOISE_HIGH));
    CHECK(ev[0].value == -1); /* a ticket carries no trend: -1, never "0 s to failure" */
    CHECK(ev[1].kind == LG_EVT_METRIC && ev[1].code == LG_CTL_METRIC_LEAD_TIME_MS);
    CHECK(ev[1].comp == COMP(W, A));
    CHECK((uint32_t)ev[1].value == ev[1].t_ms - t_warn);
    CHECK(c.last_lead_time_ms == (uint32_t)ev[1].value && c.lead_times == 1);
    (void)lg_ctl_event_text(&ev[1], text, sizeof text);
    CHECK(strstr(text, "kind=METRIC name=lead_time_ms component=west/a value=") != NULL);
    (void)lg_ctl_event_text(&ev[0], text, sizeof text);
    CHECK(strstr(text, "diag=NOISE_HIGH") != NULL);

    /* Isolated: west/a alone no longer activates the west direction. */
    g_occ[W][A] = 1;
    run(&c, &t, 1000);
    CHECK(c.active[W] == 0);
    g_occ[W][A] = 0;

    /* Repaired: INFO, then level 0 after T_stable. */
    base = c.events;
    say(COMP(W, A), LG_VERDICT_OK, LG_DIAG_NONE, -1);
    run(&c, &t, 1000);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_NOTIFY, LG_SEV_INFO) == 1);
    run(&c, &t, LG_T_STABLE_MS + 1000);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
}

/* A warning that clears by itself: INFO, no lead time, level 0 after T_stable. */
static void test_warning_clears(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t base;
    uint32_t n;
    lg_ctl_event_t ev[16];

    start(&c, &t, 1, 1);
    say(COMP(E, B), LG_VERDICT_WARNING, LG_DIAG_RESPONSE_DELAY, -1);
    run(&c, &t, 2000);
    CHECK(g_last.level == LG_LEVEL_EARLY_WARNING);
    base = c.events;
    say(COMP(E, B), LG_VERDICT_OK, LG_DIAG_NONE, -1);
    run(&c, &t, 2000);
    n = events_after(&c, base, ev);
    CHECK(n == 1 && ev[0].kind == LG_EVT_NOTIFY && ev[0].severity == LG_SEV_INFO);
    CHECK(c.lead_times == 0);
    CHECK(g_last.level == LG_LEVEL_EARLY_WARNING); /* de-escalation needs T_stable */
    run(&c, &t, LG_T_STABLE_MS);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
}

/* Gate wear: WARNING (level 1), then FAILED by lg-health (level 4, URGENT ticket, lead time). */
static void test_gate_slow(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t base;
    uint32_t n;
    lg_ctl_event_t ev[16];
    char text[LG_CTL_EVENT_TEXT_MAX];

    start(&c, &t, 1, 1);
    base = c.events;
    say(LG_COMP_GATE, LG_VERDICT_WARNING, LG_DIAG_GATE_SLOW, 300);
    run(&c, &t, 2000);
    CHECK(g_last.level == LG_LEVEL_EARLY_WARNING);
    n = events_after(&c, base, ev);
    CHECK(n == 2 && ev[0].comp == LG_COMP_GATE && ev[0].severity == LG_SEV_WARNING);
    CHECK(ev[0].dir == LG_CTL_EVENT_NO_SENSOR);
    (void)lg_ctl_event_text(&ev[0], text, sizeof text);
    CHECK(strstr(text, "sensor=none diag=GATE_SLOW") != NULL);
    CHECK(strstr(text, "component=gate trend_s=300") != NULL);
    base = c.events;
    say(LG_COMP_GATE, LG_VERDICT_FAILED, LG_DIAG_GATE_SLOW, 0);
    run(&c, &t, 1000);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    CHECK(g_last.gate_target == LG_GATE_TARGET_DOWN);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_METRIC, 0xFF) == 1);
    CHECK(count_kind(ev, n, LG_EVT_NOTIFY, LG_SEV_URGENT) == 1);
    CHECK(c.comp_ticket[LG_COMP_GATE] == 1 && c.tickets == 1);
    /* Repaired (verdict OK again) but level 4 stays until a guard acknowledges. */
    say(LG_COMP_GATE, LG_VERDICT_OK, LG_DIAG_NONE, -1);
    run(&c, &t, LG_T_STABLE_MS + 1000);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    lg_ctl_on_guard_ack(&c);
    run(&c, &t, LG_T_STABLE_MS + 1000);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
}

/* A gate that gets a WARNING and then reports FAULT itself: the lead time counts too. */
static void test_gate_warning_then_own_fault(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t base;
    uint32_t n;
    lg_ctl_event_t ev[16];

    start(&c, &t, 1, 1);
    say(LG_COMP_GATE, LG_VERDICT_WARNING, LG_DIAG_GATE_SLOW, -1);
    run(&c, &t, 2000);
    base = c.events;
    (void)lg_ctl_on_gate_reply(&c, LG_GATE_FAULT, t);
    g_gate = LG_GATE_FAULT;
    g_last = lg_ctl_step(&c, t);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_METRIC, 0xFF) == 1);
    CHECK(ev[0].kind == LG_EVT_NOTIFY && ev[0].code == LG_CTL_DIAG_GATE_FAULT);
    CHECK(ev[1].kind == LG_EVT_METRIC);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
}

static void test_battery(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t base;
    uint32_t n;
    lg_ctl_event_t ev[16];

    start(&c, &t, 1, 1);
    say(LG_COMP_BATTERY, LG_VERDICT_WARNING, LG_DIAG_BATTERY_DRAIN, 500);
    run(&c, &t, 1000);
    CHECK(g_last.level == LG_LEVEL_EARLY_WARNING);
    base = c.events;
    say(LG_COMP_BATTERY, LG_VERDICT_FAILED, LG_DIAG_BATTERY_LOW, 0);
    run(&c, &t, 1000);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_NOTIFY, LG_SEV_URGENT) == 1);
    CHECK(ev[0].comp == LG_COMP_BATTERY && ev[0].code == HB(LG_DIAG_BATTERY_LOW));
}

/* lg-health falls silent: its verdicts stop counting and the controller goes to level 2. */
static void test_health_silent(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    start(&c, &t, 1, 1);
    say(LG_COMP_BATTERY, LG_VERDICT_WARNING, LG_DIAG_BATTERY_DRAIN, -1);
    run(&c, &t, 1000);
    CHECK(g_last.level == LG_LEVEL_EARLY_WARNING);
    g_health_on = 0;
    run(&c, &t, LG_CTL_HEALTH_SILENCE_MS - 600);
    CHECK(c.health_silent == 0);
    run(&c, &t, 1000);
    CHECK(c.health_silent == 1);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    CHECK(c.warn_open[LG_COMP_BATTERY] == 1); /* silence is not a repair: the episode stays open */
    health_all_ok();
    g_health_on = 1;
    run(&c, &t, 1000);
    CHECK(c.health_silent == 0);
    run(&c, &t, LG_T_STABLE_MS + 1000);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
}

/* A whole-system stall is not lg-health being silent (the stall counts as 50 ms, Step 9 rule). */
static void test_stall_is_not_health_silence(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    start(&c, &t, 1, 1);
    t += 1900; /* the controller did not run for almost 2 s */
    tick(&c, t + 5);
    CHECK(c.health_silent == 0);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
}

/* Level 1 changes nothing in the gate cycle: the same trains give the same outputs as in level 0. */
static uint32_t g_trace[2][4000];

static void train_script(lg_ctl_t *c, uint32_t *t, int which) {
    uint32_t k;

    for (k = 0; k < 4000; k++) {
        uint32_t s = k * 10;

        g_occ[W][A] = g_occ[W][B] = (uint8_t)(s >= 1000 && s < 8000);
        g_occ[W][I] = (uint8_t)(s >= 12000 && s < 15000);
        tick(c, *t);
        *t += 10;
        g_trace[which][k] = (uint32_t)g_last.gate_target | ((uint32_t)g_last.lights << 1) |
                            ((uint32_t)g_last.bell << 2);
    }
}

static void test_level1_same_gate_cycle(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    start(&c, &t, 1, 1);
    train_script(&c, &t, 0);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    CHECK(c.cycles == 1);

    t = 0;
    start(&c, &t, 1, 1);
    say(LG_COMP_BATTERY, LG_VERDICT_WARNING, LG_DIAG_BATTERY_DRAIN, -1);
    run(&c, &t, 1000);
    CHECK(g_last.level == LG_LEVEL_EARLY_WARNING);
    t = LG_T_BOOT_STABLE_MS + 500; /* the same clock as the first run */
    train_script(&c, &t, 1);
    CHECK(g_last.level == LG_LEVEL_EARLY_WARNING);
    CHECK(c.cycles == 1);
    CHECK(memcmp(g_trace[0], g_trace[1], sizeof g_trace[0]) == 0);
}

static void test_event_text_worst_case(void) {
    lg_ctl_event_t e;
    char buf[LG_CTL_EVENT_TEXT_MAX * 2];
    size_t n;
    uint8_t kind;

    for (kind = LG_EVT_LEVEL_CHANGE; kind <= LG_EVT_METRIC; kind++) {
        memset(&e, 0xFF, sizeof e);
        e.kind = kind;
        e.severity = LG_SEV_WARNING;
        e.comp = LG_COMP_SENSOR_BASE + 5; /* east/island: the longest name */
        e.dir = E;
        e.id = I;
        e.code = HB(LG_DIAG_GATE_HIGH_CURRENT);
        e.value = -2147483647 - 1;
        n = lg_ctl_event_text(&e, buf, sizeof buf);
        CHECK(n < LG_CTL_EVENT_TEXT_MAX - 1);
        e.comp = LG_COMP_BATTERY;
        e.dir = e.id = LG_CTL_EVENT_NO_SENSOR;
        n = lg_ctl_event_text(&e, buf, sizeof buf);
        CHECK(n < LG_CTL_EVENT_TEXT_MAX - 1);
    }
    CHECK(strcmp(lg_ctl_diag_name(HB(LG_DIAG_GATE_SLOW)), "GATE_SLOW") == 0);
    CHECK(strcmp(lg_ctl_diag_name(LG_CTL_DIAG_GATE_FAULT), "GATE_FAULT") == 0);
    CHECK(strcmp(lg_ctl_diag_name(HB(LG_DIAG_COUNT)), "UNKNOWN") == 0);
}

int main(void) {
    test_verdict_validation();
    test_not_installed_changes_nothing();
    test_required_holds_the_boot_lock();
    test_boot_transient_raises_nothing();
    test_warning_then_failed_sensor();
    test_warning_clears();
    test_gate_slow();
    test_gate_warning_then_own_fault();
    test_battery();
    test_health_silent();
    test_stall_is_not_health_silence();
    test_level1_same_gate_cycle();
    test_event_text_worst_case();
    LG_TEST_DONE("test_controller_health");
}
