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

/* Extra Step 11 cases: unknown diagnosis codes, lead time rules, many components, ring, text limits. */

/* A WARNING with a code outside lg_diag_t is still a WARNING. */
static void test_unknown_diag_code(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t base;
    uint32_t n;
    lg_ctl_event_t ev[16];
    char text[LG_CTL_EVENT_TEXT_MAX];

    start(&c, &t, 1, 1);
    base = c.events;
    say(COMP(E, A), LG_VERDICT_WARNING, (uint16_t)(LG_DIAG_COUNT + 40), 7);
    run(&c, &t, 2000);
    CHECK(g_last.level == LG_LEVEL_EARLY_WARNING);
    n = events_after(&c, base, ev);
    CHECK(n == 2 && ev[0].kind == LG_EVT_NOTIFY && ev[0].severity == LG_SEV_WARNING);
    CHECK(ev[0].code == HB(LG_DIAG_NONE)); /* an unknown code is clamped to NONE in the event */
    CHECK(c.health_diag[COMP(E, A)] == LG_DIAG_COUNT + 40);
    CHECK(ev[0].value == 7);
    (void)lg_ctl_event_text(&ev[0], text, sizeof text);
    CHECK(strstr(text, "component=east/a") != NULL);
    CHECK(strcmp(lg_ctl_diag_name(HB(LG_DIAG_COUNT)), "UNKNOWN") == 0);
    CHECK(strcmp(lg_ctl_diag_name(255), "UNKNOWN") == 0);
    CHECK(strcmp(lg_ctl_diag_name(HB(LG_DIAG_NO_DATA)), "NO_DATA") == 0);
}

/* WARNING -> OK -> FAILED: two episodes, the second has no WARNING before it, so no lead time. */
static void test_no_lead_time_after_ok(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t base;
    uint32_t n;
    lg_ctl_event_t ev[16];

    start(&c, &t, 1, 1);
    say(COMP(W, A), LG_VERDICT_WARNING, LG_DIAG_NOISE_RISING, 100);
    run(&c, &t, 1000);
    say(COMP(W, A), LG_VERDICT_OK, LG_DIAG_NONE, -1);
    run(&c, &t, 1000);
    CHECK(c.warnings == 1 && c.lead_times == 0);
    base = c.events;
    say(COMP(W, A), LG_VERDICT_FAILED, LG_DIAG_NOISE_HIGH, -1);
    run(&c, &t, 1000);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_METRIC, 0xFF) == 0);
    CHECK(count_kind(ev, n, LG_EVT_NOTIFY, LG_SEV_URGENT) == 1); /* the ticket still comes */
    CHECK(c.lead_times == 0 && c.last_lead_time_ms == 0);
}

/* OK -> FAILED directly: no lead time; FAILED -> WARNING -> FAILED does not invent one. */
static void test_no_lead_time_without_warning(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t n;
    uint32_t base;
    lg_ctl_event_t ev[16];

    start(&c, &t, 1, 1);
    say(COMP(E, B), LG_VERDICT_FAILED, LG_DIAG_DROPOUTS, -1);
    run(&c, &t, 1000);
    CHECK(c.lead_times == 0 && c.warnings == 0);
    base = c.events;
    say(COMP(E, B), LG_VERDICT_WARNING, LG_DIAG_DROPOUTS, -1); /* getting better, not a new episode */
    run(&c, &t, 1000);
    say(COMP(E, B), LG_VERDICT_FAILED, LG_DIAG_DROPOUTS, -1);
    run(&c, &t, 1000);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_METRIC, 0xFF) == 0);
    CHECK(c.warnings == 0 && c.lead_times == 0);
}

/* Two full episodes on the battery: two lead times, two tickets. */
static void test_two_episodes_two_lead_times(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    int k;

    start(&c, &t, 1, 1);
    for (k = 0; k < 2; k++) {
        say(LG_COMP_BATTERY, LG_VERDICT_WARNING, LG_DIAG_BATTERY_DRAIN, 60);
        run(&c, &t, 1500);
        say(LG_COMP_BATTERY, LG_VERDICT_FAILED, LG_DIAG_BATTERY_LOW, -1);
        run(&c, &t, 1500);
        CHECK(c.lead_times == (uint32_t)(k + 1));
        CHECK(c.last_lead_time_ms >= 1000 && c.last_lead_time_ms <= 2000);
        say(LG_COMP_BATTERY, LG_VERDICT_OK, LG_DIAG_NONE, -1);
        run(&c, &t, 1500);
    }
    CHECK(c.tickets == 2 && c.warnings == 2);
}

/* All eight components get WARNING in one step, then FAILED, then OK: the ring wraps. */
static void test_many_components_ring(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t base;
    uint32_t n;
    uint32_t k;
    lg_ctl_event_t ev[LG_CTL_EVENT_RING + 8];
    int warn_seen[LG_COMP_COUNT];

    memset(warn_seen, 0, sizeof warn_seen);
    start(&c, &t, 1, 1);
    base = c.events;
    for (k = 0; k < LG_COMP_COUNT; k++) {
        say((uint8_t)k, LG_VERDICT_WARNING, LG_DIAG_NOISE_HIGH, (int32_t)(k * 10));
    }
    run(&c, &t, 1000);
    n = lg_ctl_events_since(&c, base + 1, ev, LG_CTL_EVENT_RING + 8);
    CHECK(c.warnings == LG_COMP_COUNT);
    CHECK(count_kind(ev, n, LG_EVT_NOTIFY, LG_SEV_WARNING) == LG_COMP_COUNT);
    for (k = 0; k < n; k++) {
        if (ev[k].kind == LG_EVT_NOTIFY && ev[k].comp < LG_COMP_COUNT) {
            warn_seen[ev[k].comp]++;
            CHECK(ev[k].value == (int32_t)(ev[k].comp * 10)); /* every trend stays with its component */
        }
    }
    for (k = 0; k < LG_COMP_COUNT; k++) {
        CHECK(warn_seen[k] == 1);
    }
    for (k = 1; k < n; k++) {
        CHECK(ev[k].seq == ev[k - 1].seq + 1);
    }

    for (k = 0; k < LG_COMP_COUNT; k++) {
        say((uint8_t)k, LG_VERDICT_FAILED, LG_DIAG_NOISE_HIGH, -1);
    }
    run(&c, &t, 1000);
    CHECK(c.lead_times == LG_COMP_COUNT);
    for (k = 0; k < LG_COMP_COUNT; k++) {
        say((uint8_t)k, LG_VERDICT_OK, LG_DIAG_NONE, -1);
    }
    run(&c, &t, 1000);
    CHECK(c.events > LG_CTL_EVENT_RING);
    /* Asking for an overwritten sequence number returns the oldest that is still held. */
    n = lg_ctl_events_since(&c, 1, ev, LG_CTL_EVENT_RING + 8);
    CHECK(n == LG_CTL_EVENT_RING);
    CHECK(ev[0].seq == c.events - LG_CTL_EVENT_RING + 1u);
    CHECK(ev[n - 1].seq == c.events);
    CHECK(lg_ctl_events_since(&c, c.events + 1, ev, 4) == 0);
    CHECK(lg_ctl_events_since(&c, 1, ev, 0) == 0);
    CHECK(lg_ctl_events_since(&c, 1, NULL, 4) == 0);
}

/* Event text never overruns a small buffer. */
static void test_text_truncation(void) {
    lg_ctl_event_t e;
    char big[LG_CTL_EVENT_TEXT_MAX];
    char small[64];
    size_t full;
    size_t len;
    size_t n;
    uint8_t kind;

    for (kind = LG_EVT_LEVEL_CHANGE; kind <= LG_EVT_METRIC; kind++) {
        memset(&e, 0, sizeof e);
        e.kind = kind;
        e.severity = LG_SEV_URGENT;
        e.comp = 5;
        e.dir = E;
        e.id = I;
        e.code = HB(LG_DIAG_BATTERY_DRAIN);
        e.value = 123456;
        full = lg_ctl_event_text(&e, big, sizeof big);
        CHECK(full > 0 && full == strlen(big));
        for (len = 1; len < sizeof small && len <= full + 1; len++) {
            memset(small, 'x', sizeof small);
            n = lg_ctl_event_text(&e, small, len);
            CHECK(n < len);
            CHECK(small[n] == '\0');
            CHECK(strncmp(small, big, n) == 0);
        }
        CHECK(lg_ctl_event_text(&e, big, 0) == 0);
        CHECK(lg_ctl_event_text(&e, NULL, 10) == 0);
    }
    memset(&e, 0, sizeof e);
    e.kind = LG_EVT_METRIC;
    e.code = 99;
    e.comp = LG_CTL_EVENT_NO_SENSOR;
    (void)lg_ctl_event_text(&e, big, sizeof big);
    CHECK(strstr(big, "name=unknown") != NULL);
}

/* Verdicts are stored per component. */
static void test_partial_reports(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    start(&c, &t, 1, 0);
    CHECK(lg_ctl_on_health_verdict(&c, LG_COMP_BATTERY, 100, LG_VERDICT_OK, 0, -1, t) == 0);
    CHECK(c.health_armed == 1 && c.health_seen[LG_COMP_BATTERY] == 1 && c.health_seen[0] == 0);
    CHECK(lg_ctl_on_health_verdict(&c, LG_COMP_BATTERY, 100, LG_VERDICT_OK, 0, -1, t + 100) == 0);
    CHECK(c.health_rx == 2);
    CHECK(lg_ctl_on_health_verdict(&c, 0, 100, LG_VERDICT_FAILED, LG_DIAG_DROPOUTS, 5, t + 200) == 0);
    CHECK(c.health_verdict[0] == LG_VERDICT_FAILED);
    CHECK(c.health_trend_s[0] == 5);
    CHECK(lg_ctl_on_health_verdict(&c, 0, 0, LG_VERDICT_OK, 0, -5, t + 300) == 0);
    CHECK(c.health_trend_s[0] == -1); /* any negative trend is stored as unknown */
    CHECK(c.health_verdict[0] == LG_VERDICT_OK);
}

int main(void) {
    test_unknown_diag_code();
    test_no_lead_time_after_ok();
    test_no_lead_time_without_warning();
    test_two_episodes_two_lead_times();
    test_many_components_ring();
    test_text_truncation();
    test_partial_reports();
    LG_TEST_DONE("test_controller_health_extra");
}
