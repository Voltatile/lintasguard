/*
 * Step 11: lg-health falling silent is not a repair (found by the tester agent, failed on the code before
 * the fix): no false "working again" INFO while it is silent, and no second ticket for the same fault when
 * it comes back. Covers the gate/battery path (health_events) and the sensor path (sensor_tickets).
 */
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
static uint8_t g_force_fault; /* the gate answers FAULT */
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
    (void)lg_ctl_on_gate_reply(c, g_force_fault ? (uint8_t)LG_GATE_FAULT : g_gate, now);
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

static void test_battery_ticket_survives_silence(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t base;
    uint32_t n;
    lg_ctl_event_t ev[16];

    start(&c, &t, 1, 1);
    say(LG_COMP_BATTERY, LG_VERDICT_FAILED, LG_DIAG_BATTERY_LOW, -1);
    run(&c, &t, 1000);
    CHECK(c.tickets == 1);
    base = c.events;
    g_health_on = 0;
    run(&c, &t, LG_CTL_HEALTH_SILENCE_MS + 1000);
    CHECK(c.health_silent == 1);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_NOTIFY, LG_SEV_INFO) == 0); /* no "repaired" while merely silent */
    CHECK(c.comp_ticket[LG_COMP_BATTERY] == 1);
    g_health_on = 1; /* lg-health is back, battery still FAILED */
    run(&c, &t, 1000);
    CHECK(c.tickets == 1); /* no second ticket for the same fault */
    /* A real repair afterwards still closes it. */
    base = c.events;
    say(LG_COMP_BATTERY, LG_VERDICT_OK, LG_DIAG_NONE, -1);
    run(&c, &t, 1000);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_NOTIFY, LG_SEV_INFO) == 1);
    CHECK(c.comp_ticket[LG_COMP_BATTERY] == 0);
}

static void test_sensor_ticket_survives_silence(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t base;
    uint32_t n;
    lg_ctl_event_t ev[16];

    start(&c, &t, 1, 1);
    say(0, LG_VERDICT_FAILED, LG_DIAG_NOISE_HIGH, -1); /* west/a FAILED by lg-health */
    run(&c, &t, 1000);
    CHECK(c.tickets == 1);
    CHECK(c.ticket_open[0][0] == 1);
    base = c.events;
    g_health_on = 0;
    run(&c, &t, LG_CTL_HEALTH_SILENCE_MS + 1000);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_NOTIFY, LG_SEV_INFO) == 0);
    CHECK(c.ticket_open[0][0] == 1);
    g_health_on = 1;
    run(&c, &t, 1000);
    CHECK(c.tickets == 1);
    base = c.events;
    say(0, LG_VERDICT_OK, LG_DIAG_NONE, -1);
    run(&c, &t, 1000);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_NOTIFY, LG_SEV_INFO) == 1);
    CHECK(c.ticket_open[0][0] == 0);
}

static void test_warning_episode_survives_silence(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t base;
    uint32_t n;
    lg_ctl_event_t ev[16];

    start(&c, &t, 1, 1);
    say(LG_COMP_GATE, LG_VERDICT_WARNING, LG_DIAG_GATE_SLOW, 100);
    run(&c, &t, 1000);
    CHECK(c.warnings == 1);
    g_health_on = 0;
    run(&c, &t, LG_CTL_HEALTH_SILENCE_MS + 1000);
    CHECK(c.warn_open[LG_COMP_GATE] == 1);
    g_health_on = 1; /* back, still WARNING: no second notification */
    run(&c, &t, 1000);
    CHECK(c.warnings == 1);
    base = c.events;
    say(LG_COMP_GATE, LG_VERDICT_FAILED, LG_DIAG_GATE_SLOW, -1);
    run(&c, &t, 1000);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_METRIC, 0xFF) == 1); /* the lead time still counts from the first warning */
}

/*
 * Review M2: while lg-health is silent, a sensor it declared FAILED stays out of the vote and out of the
 * stuck rules (its phantom trains must not latch the healthy partner). Its WARNINGs lapse.
 */
static void test_failed_sensor_stays_isolated_while_silent(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    start(&c, &t, 1, 1);
    say(COMP(LG_DIR_WEST, LG_SENSOR_A), LG_VERDICT_FAILED, LG_DIAG_NOISE_HIGH, -1);
    say(COMP(LG_DIR_EAST, LG_SENSOR_B), LG_VERDICT_WARNING, LG_DIAG_RESPONSE_DELAY, -1);
    run(&c, &t, 1000);
    g_health_on = 0;
    run(&c, &t, LG_CTL_HEALTH_SILENCE_MS + 500);
    CHECK(c.health_silent == 1);
    CHECK(c.comp_verdict[COMP(LG_DIR_WEST, LG_SENSOR_A)] == LG_VERDICT_FAILED);
    CHECK(c.warn_open[COMP(LG_DIR_EAST, LG_SENSOR_B)] == 1); /* the warning episode is held, not repaired */
    /* A phantom train on west/a alone, 1.5 s: no activation, and west/b is not latched MISSED_TRAIN. */
    g_occ[LG_DIR_WEST][LG_SENSOR_A] = 1;
    run(&c, &t, 1500);
    g_occ[LG_DIR_WEST][LG_SENSOR_A] = 0;
    CHECK(c.active[LG_DIR_WEST] == 0);
    run(&c, &t, 1000);
    CHECK(lg_ctl_sensor_diag(&c, LG_DIR_WEST, LG_SENSOR_B) == LG_CTL_DIAG_NONE);
}

/*
 * Review M3: with lg-health gone for good, the controller's own gate rules still raise and close tickets:
 * a FAULT, a recovery (INFO) and a second FAULT give two tickets.
 */
static void test_own_gate_tickets_while_health_silent(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t base;
    uint32_t n;
    lg_ctl_event_t ev[16];

    start(&c, &t, 1, 1);
    g_health_on = 0;
    run(&c, &t, LG_CTL_HEALTH_SILENCE_MS + 500);
    CHECK(c.health_silent == 1);
    g_force_fault = 1;
    run(&c, &t, 500);
    CHECK(c.tickets == 1);
    base = c.events;
    g_force_fault = 0;
    run(&c, &t, 500);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_NOTIFY, LG_SEV_INFO) == 1); /* the gate's own recovery is a real one */
    g_force_fault = 1;
    run(&c, &t, 500);
    CHECK(c.tickets == 2);
    g_force_fault = 0;
}

/*
 * Review M1: a restarted lg-health sends OK for every component before it has looked at them. That must
 * not repair a gate that was FAILED by wear: level 4 stays and the gate stays FAILED until a technician's
 * reset (an assessed OK) arrives.
 */
/* The restarted lg-health: OK, not assessed, for every component every 500 ms, for ms. */
static void unassessed_for(lg_ctl_t *c, uint32_t *t, uint32_t ms) {
    uint32_t k;

    for (k = 0; k < ms / 500u; k++) {
        uint8_t comp;

        for (comp = 0; comp < LG_COMP_COUNT; comp++) {
            CHECK(lg_ctl_on_health_verdict_flags(c, comp, 100, LG_VERDICT_OK, LG_DIAG_NONE, -1, 0, *t) == 0);
        }
        run(c, t, 500);
    }
}

static void test_restarted_health_is_not_a_repair(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t base;
    uint32_t n;
    lg_ctl_event_t ev[16];

    start(&c, &t, 1, 1);
    say(LG_COMP_GATE, LG_VERDICT_FAILED, LG_DIAG_GATE_SLOW, -1);
    run(&c, &t, 1000);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    base = c.events;
    g_health_on = 0; /* the old process is gone; the new one speaks through unassessed_for */
    unassessed_for(&c, &t, 10000);
    CHECK(c.health_unassessed > 0);
    CHECK(c.health_silent == 0); /* it is alive */
    CHECK(c.comp_verdict[LG_COMP_GATE] == LG_VERDICT_FAILED);
    n = events_after(&c, base, ev);
    CHECK(count_kind(ev, n, LG_EVT_NOTIFY, LG_SEV_INFO) == 0); /* no false "Gate is working again" */
    lg_ctl_on_guard_ack(&c);
    unassessed_for(&c, &t, LG_T_STABLE_MS + 1000);
    CHECK(c.health_silent == 0); /* the level 4 below is the gate, not a silent monitor */
    CHECK(g_last.level == LG_LEVEL_FAILSAFE); /* the guard cannot release a gate that is still FAILED */
    /* The technician's reset: an assessed OK. Then a guard can release it. */
    CHECK(lg_ctl_on_health_verdict_flags(&c, LG_COMP_GATE, 100, LG_VERDICT_OK, LG_DIAG_NONE, -1, LG_HV_ASSESSED, t) == 0);
    health_all_ok();
    g_health_on = 1;
    run(&c, &t, 1000);
    CHECK(c.comp_verdict[LG_COMP_GATE] == LG_VERDICT_OK);
    lg_ctl_on_guard_ack(&c);
    run(&c, &t, LG_T_STABLE_MS + 1000);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
}

int main(void) {
    test_restarted_health_is_not_a_repair();
    test_failed_sensor_stays_isolated_while_silent();
    test_own_gate_tickets_while_health_silent();
    test_battery_ticket_survives_silence();
    test_sensor_ticket_survives_silence();
    test_warning_episode_survives_silence();
    LG_TEST_DONE("test_controller_health_silence");
}
