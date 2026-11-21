#include <stdio.h>
#include <string.h>

#include "lg_controller_logic.h"
#include "lg_gate_model.h"
#include "lg_test.h"

/*
 * Step 9, additional coverage: 32-bit time wrap in the stuck rules, flicker, diagnosis priority,
 * release of both latches, island sensors, ticket causes (island, STALE, BLIND), boot and restarted
 * controller raising no tickets, event sequence continuity, event text with unknown values.
 * The gate is ideal (reports the last target at once). Wrong side is checked on every tick.
 */

#define W LG_DIR_WEST
#define E LG_DIR_EAST
#define A LG_SENSOR_A
#define B LG_SENSOR_B
#define I LG_SENSOR_ISLAND

static uint8_t g_occ[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
static uint8_t g_mute[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
static uint8_t g_q[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];    /* sample quality */
static uint32_t g_age[LG_DIR_COUNT][LG_SENSOR_ID_COUNT]; /* header time = now - age */
static uint16_t g_ax[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
static uint8_t g_gate = LG_GATE_DOWN;
static lg_ctl_out_t g_last;
static int g_wrong_side;

static void tick(lg_ctl_t *c, uint32_t now) {
    int d;
    int i;

    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            if (!g_mute[d][i]) {
                CHECK(lg_ctl_on_sample(c, (uint8_t)d, (uint8_t)i, g_occ[d][i], g_q[d][i], g_ax[d][i],
                                       now - g_age[d][i], now) == 0);
            }
        }
    }
    CHECK(lg_ctl_on_gate_reply(c, g_gate, now) == 0);
    g_last = lg_ctl_step(c, now);
    if ((g_occ[W][I] || g_occ[E][I]) && g_gate != LG_GATE_DOWN) {
        g_wrong_side++;
    }
    g_gate = g_last.gate_target == LG_GATE_TARGET_DOWN ? (uint8_t)LG_GATE_DOWN : (uint8_t)LG_GATE_UP;
}

static void run(lg_ctl_t *c, uint32_t *t, uint32_t ms) {
    uint32_t end = *t + ms;

    for (; *t != end; *t += 10) {
        tick(c, *t);
    }
}

static void reset_world(void) {
    int d;
    int i;

    memset(g_occ, 0, sizeof g_occ);
    memset(g_mute, 0, sizeof g_mute);
    memset(g_age, 0, sizeof g_age);
    memset(g_ax, 0, sizeof g_ax);
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            g_q[d][i] = 97;
        }
    }
    g_gate = LG_GATE_DOWN;
    g_wrong_side = 0;
}

static void released(lg_ctl_t *c, uint32_t *t) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();

    reset_world();
    lg_ctl_init(c, &cfg, *t);
    run(c, t, LG_T_BOOT_STABLE_MS + 500);
    CHECK(g_last.level == LG_LEVEL_NORMAL);
    CHECK(g_gate == LG_GATE_UP);
}

/* Same train as in test_controller_stuck.c: approach runs as given, island 12..16 s, 22 s in total. */
static void train(lg_ctl_t *c, uint32_t *t, int d, int a_on, int a_off, int b_on, int b_off) {
    uint32_t t0 = *t;
    uint32_t end = t0 + 22000;

    for (; *t != end; *t += 10) {
        int rel = (int)(*t - t0);

        if (a_on >= 0) {
            g_occ[d][A] = (uint8_t)(rel >= a_on && rel < a_off);
        }
        if (b_on >= 0) {
            g_occ[d][B] = (uint8_t)(rel >= b_on && rel < b_off);
        }
        g_occ[d][I] = (uint8_t)(rel >= 12000 && rel < 16000);
        if (rel == 2000) {
            g_ax[d][A] = (uint16_t)(g_ax[d][A] + 8);
            g_ax[d][B] = (uint16_t)(g_ax[d][B] + 8);
        }
        if (rel == 16000) {
            g_ax[d][I] = (uint16_t)(g_ax[d][I] + 8);
        }
        tick(c, *t);
    }
}

/* Counts NOTIFY events with the given severity among the events still in the ring. */
static int count_notify(const lg_ctl_t *c, uint8_t sev) {
    lg_ctl_event_t ev[LG_CTL_EVENT_RING];
    uint32_t n = lg_ctl_events_since(c, 0, ev, LG_CTL_EVENT_RING);
    uint32_t k;
    int cnt = 0;

    for (k = 0; k < n; k++) {
        if (ev[k].kind == LG_EVT_NOTIFY && ev[k].severity == sev) {
            cnt++;
        }
    }
    return cnt;
}

/* The newest URGENT NOTIFY event; zeroed when none. */
static lg_ctl_event_t last_urgent(const lg_ctl_t *c) {
    lg_ctl_event_t ev[LG_CTL_EVENT_RING];
    lg_ctl_event_t r;
    uint32_t n = lg_ctl_events_since(c, 0, ev, LG_CTL_EVENT_RING);
    uint32_t k;

    memset(&r, 0, sizeof r);
    for (k = 0; k < n; k++) {
        if (ev[k].kind == LG_EVT_NOTIFY && ev[k].severity == LG_SEV_URGENT) {
            r = ev[k];
        }
    }
    return r;
}

/* ---- 1. 32-bit time wrap during stuck judgements ---- */

typedef struct {
    int missed_diag; /* west A after the missed train */
    int missed_level;
    int missed_cycles;
    int released_diag; /* west A after an agreeing train */
    int stuck_diag;    /* east B after a long occupied run without partner */
    int stuck_level;
    int east_active_ticks;
    int wrong_side;
    int tickets;
    int events;
} wrap_res_t;

static void wrap_script(uint32_t base, wrap_res_t *r) {
    lg_ctl_t c;
    uint32_t t = base;
    uint32_t ticks_active = 0;
    uint32_t k;

    memset(r, 0, sizeof *r);
    released(&c, &t);
    train(&c, &t, W, 0, 0, 0, 5000);
    r->missed_diag = lg_ctl_sensor_diag(&c, W, A);
    r->missed_level = g_last.level;
    r->missed_cycles = (int)c.cycles;
    train(&c, &t, W, 0, 5000, 100, 5100);
    r->released_diag = lg_ctl_sensor_diag(&c, W, A);
    g_occ[E][B] = 1;
    for (k = 0; k < (LG_SENSOR_MAX_OCCUPIED_MS + 2000) / 10; k++) {
        tick(&c, t);
        t += 10;
        if (c.active[E]) {
            ticks_active++;
        }
    }
    r->stuck_diag = lg_ctl_sensor_diag(&c, E, B);
    r->stuck_level = g_last.level;
    r->east_active_ticks = (int)ticks_active;
    g_occ[E][B] = 0;
    run(&c, &t, 2000);
    r->wrong_side = g_wrong_side;
    r->tickets = (int)c.tickets;
    r->events = (int)c.events;
}

static void test_time_wrap_in_stuck_rules(void) {
    wrap_res_t ref;
    wrap_res_t w;
    uint32_t bases[40];
    size_t n = 0;
    size_t i;
    uint32_t k;

    wrap_script(0, &ref);
    CHECK(ref.missed_diag == LG_CTL_DIAG_MISSED_TRAIN);
    CHECK(ref.missed_level == LG_LEVEL_DEGRADED);
    CHECK(ref.missed_cycles == 1);
    CHECK(ref.released_diag == LG_CTL_DIAG_NONE);
    CHECK(ref.stuck_diag == LG_CTL_DIAG_STUCK_OCCUPIED);
    CHECK(ref.wrong_side == 0);
    CHECK(ref.tickets == 2);

    for (k = 0; k < 24; k++) { /* the wrap falls in every phase of the stages */
        bases[n++] = (uint32_t)0 - (6000u + k * 3770u);
    }
    bases[n++] = 0xFFFFFFFFu - 9u;
    bases[n++] = 0x7FFFFFF0u;
    bases[n++] = 0x7FFFFFFFu - 25000u;
    bases[n++] = 0x80000000u;
    bases[n++] = 0xFFFFFFFFu - 11500u; /* about when the first train starts */
    bases[n++] = 0xFFFFFFFFu - 13000u;
    for (i = 0; i < n; i++) {
        wrap_script(bases[i], &w);
        if (memcmp(&w, &ref, sizeof w) != 0) {
            printf("wrap mismatch base=%u: diag %d/%d rel %d/%d stuck %d/%d level %d/%d tickets %d/%d "
                   "events %d/%d\n",
                   (unsigned)bases[i], w.missed_diag, ref.missed_diag, w.released_diag, ref.released_diag,
                   w.stuck_diag, ref.stuck_diag, w.stuck_level, ref.stuck_level, w.tickets, ref.tickets,
                   w.events, ref.events);
        }
        CHECK(memcmp(&w, &ref, sizeof w) == 0);
    }
}

/* ---- 2. A run replaced by a new rise before it is judged ---- */

static void test_flicker_run_is_replaced_before_judgement(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    g_occ[W][A] = 1;
    run(&c, &t, 600); /* a run of at least t_disagree ... */
    g_occ[W][A] = 0;
    run(&c, &t, 100); /* ... that falls, and A rises again before it is judged (t_disagree later) */
    g_occ[W][A] = 1;
    run(&c, &t, 2300);
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_NONE); /* the first run was dropped, not judged */
    run(&c, &t, 2000);
    g_occ[W][A] = 0;
    run(&c, &t, LG_T_DISAGREE_MS + 500);
    /* The second run is a whole train that B missed. */
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_MISSED_TRAIN);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE);
    CHECK(g_wrong_side == 0);
}

/* ---- 3. The first diagnosis stays ---- */

static void test_first_diagnosis_is_kept(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    train(&c, &t, W, 0, 0, 0, 5000);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_MISSED_TRAIN);
    /* A now reports occupied for far longer than a train while B is clear. */
    g_occ[W][A] = 1;
    run(&c, &t, LG_SENSOR_MAX_OCCUPIED_MS + 3000);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_MISSED_TRAIN);
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_NONE);
    g_occ[W][A] = 0;
    run(&c, &t, 2000);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_MISSED_TRAIN);
    CHECK(c.tickets == 1); /* one fault, one ticket, no second one for the changed symptom */
    CHECK(g_wrong_side == 0);
}

/* ---- 4. One agreeing run releases both latches ---- */

static void test_one_run_releases_both_latches(void) {
    lg_ctl_t c;
    uint32_t t = 0;

    released(&c, &t);
    train(&c, &t, W, 0, 0, 0, 5000); /* A missed the train */
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_MISSED_TRAIN);
    g_occ[W][B] = 1; /* B stuck occupied while A is clear */
    run(&c, &t, LG_SENSOR_MAX_OCCUPIED_MS + 2000);
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_STUCK_OCCUPIED);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_MISSED_TRAIN);
    CHECK(g_last.level >= LG_LEVEL_UNCERTAIN); /* no usable approach sensor in west */
    g_occ[W][B] = 0;
    run(&c, &t, 2000);
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_STUCK_OCCUPIED); /* its long run judges nobody */
    train(&c, &t, W, 0, 5000, 200, 5200);
    CHECK(lg_ctl_sensor_diag(&c, W, A) == LG_CTL_DIAG_NONE);
    CHECK(lg_ctl_sensor_diag(&c, W, B) == LG_CTL_DIAG_NONE);
    CHECK(g_wrong_side == 0);
}

/* ---- 5. Island sensors ---- */

static void test_island_never_gets_a_stuck_diagnosis(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    uint32_t t0;
    int d;

    released(&c, &t);
    t0 = t;
    g_occ[W][A] = 1;
    g_occ[W][B] = 1;
    g_ax[W][A] = 8; /* 8 axles counted in, none out yet: the direction stays active */
    g_ax[W][B] = 8;
    run(&c, &t, 5000);
    g_occ[W][A] = 0;
    g_occ[W][B] = 0;
    run(&c, &t, 7000);
    g_occ[W][I] = 1; /* the train sits on the island far longer than max_occupied_ms */
    while (t - t0 < 12000 + 3u * LG_SENSOR_MAX_OCCUPIED_MS) {
        run(&c, &t, 10);
        CHECK(g_gate == LG_GATE_DOWN);
    }
    for (d = 0; d < LG_DIR_COUNT; d++) {
        CHECK(lg_ctl_sensor_diag(&c, (uint8_t)d, I) == LG_CTL_DIAG_NONE);
    }
    CHECK(c.tickets == 0);
    CHECK(count_notify(&c, LG_SEV_URGENT) == 0);
    CHECK(g_wrong_side == 0);
}

static void test_silent_island_raises_urgent_ticket(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    lg_ctl_event_t e;

    released(&c, &t);
    g_mute[W][I] = 1;
    run(&c, &t, 400);
    CHECK(c.tickets == 1);
    CHECK(c.ticket_open[W][I] == 1);
    e = last_urgent(&c);
    CHECK(e.kind == LG_EVT_NOTIFY && e.severity == LG_SEV_URGENT);
    CHECK(e.dir == W && e.id == I && e.code == LG_CTL_DIAG_SILENT);
    CHECK(g_last.level >= LG_LEVEL_UNCERTAIN); /* an island that is not heard keeps the gate closed */
    CHECK(g_gate == LG_GATE_DOWN);
    CHECK(lg_ctl_sensor_diag(&c, W, I) == LG_CTL_DIAG_NONE); /* diag API stays NONE for the island */
    g_mute[W][I] = 0;
    run(&c, &t, 100);
    CHECK(c.ticket_open[W][I] == 0);
    CHECK(c.tickets == 1);
    CHECK(g_wrong_side == 0);
}

/* ---- 6. STALE and BLIND causes ---- */

static void test_stale_sensor_ticket(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    lg_ctl_event_t e;

    released(&c, &t);
    g_age[W][B] = 100; /* samples arrive every tick but are older than LG_SAMPLE_MAX_AGE_MS */
    run(&c, &t, 100);
    CHECK(c.tickets == 0); /* a short staleness is not a failure */
    run(&c, &t, 300);
    CHECK(c.tickets == 1);
    e = last_urgent(&c);
    CHECK(e.dir == W && e.id == B && e.code == LG_CTL_DIAG_STALE);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    g_age[W][B] = 0;
    run(&c, &t, 50);
    CHECK(c.ticket_open[W][B] == 0);
    CHECK(c.tickets == 1);
    CHECK(g_wrong_side == 0);
}

static void test_blind_sensor_ticket_and_no_stuck_judgement(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    lg_ctl_event_t e;

    released(&c, &t);
    g_q[E][A] = 0;
    run(&c, &t, 50);
    CHECK(c.tickets == 0);                 /* blind for less than sensor_silence_ms: */
    CHECK(g_last.level == LG_LEVEL_NORMAL); /* unusable, but not FAILED yet (Step 9) */
    run(&c, &t, 200);
    CHECK(c.tickets == 1);
    e = last_urgent(&c);
    CHECK(e.dir == E && e.id == A && e.code == LG_CTL_DIAG_BLIND);
    CHECK(g_last.level == LG_LEVEL_DEGRADED);
    /* A blind sensor is not alive: a train seen by B only is not held against it. */
    g_occ[E][B] = 1;
    run(&c, &t, 3000);
    g_occ[E][B] = 0;
    run(&c, &t, 2000);
    CHECK(lg_ctl_sensor_diag(&c, E, A) == LG_CTL_DIAG_NONE);
    CHECK(c.tickets == 1);
    g_q[E][A] = 97;
    run(&c, &t, 50);
    CHECK(c.ticket_open[E][A] == 0);
    CHECK(g_wrong_side == 0);
}

/*
 * Step 9 campaign: one sample with quality 0 (the sensor's truth query timed out in a VM stall) made a
 * ticket for a 13 ms glitch and, on an island, level 4 for good. Decided with the user: a blind sensor
 * is unusable at once but FAILED (verdict, level, ticket) only when the blindness lasted
 * sensor_silence_ms, like silence and staleness.
 */
static void test_single_blind_sample_raises_no_ticket(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    int k;

    released(&c, &t);
    for (k = 0; k < 5; k++) { /* five glitches, one tick each, 1 s apart */
        g_q[W][B] = 0;
        run(&c, &t, 10);
        CHECK(g_last.level == LG_LEVEL_NORMAL); /* one blind sample changes no level (Step 9) */
        g_q[W][B] = 97;
        run(&c, &t, 1000);
    }
    CHECK(c.tickets == 0);
    /* Just below the bound: still nothing; at the bound: the ticket. */
    g_q[W][B] = 0;
    run(&c, &t, cfg.sensor_silence_ms - 10);
    CHECK(c.tickets == 0);
    run(&c, &t, 20);
    CHECK(c.tickets == 1);
    CHECK(last_urgent(&c).code == LG_CTL_DIAG_BLIND);
    CHECK(g_wrong_side == 0);
}

/* ---- 7. Event sequence and text ---- */

static void test_event_seq_is_continuous(void) {
    lg_ctl_t c;
    uint32_t t = 0;
    lg_ctl_event_t ev[LG_CTL_EVENT_RING];
    uint32_t n;
    uint32_t k;
    int round;

    released(&c, &t);
    for (round = 0; round < 3; round++) {
        g_mute[W][A] = 1;
        run(&c, &t, 400);
        g_mute[W][A] = 0;
        run(&c, &t, 100);
        g_mute[E][B] = 1;
        run(&c, &t, 400);
        g_mute[E][B] = 0;
        run(&c, &t, 100);
    }
    CHECK(c.events < LG_CTL_EVENT_RING);
    CHECK(c.events >= 12);
    n = lg_ctl_events_since(&c, 0, ev, LG_CTL_EVENT_RING);
    CHECK(n == c.events);
    for (k = 0; k < n; k++) {
        CHECK(ev[k].seq == k + 1u);
        if (k > 0) {
            CHECK(ev[k].t_ms >= ev[k - 1].t_ms);
        }
    }
    n = lg_ctl_events_since(&c, 5, ev, LG_CTL_EVENT_RING);
    CHECK(n == c.events - 4u);
    CHECK(ev[0].seq == 5);
    CHECK(c.tickets == 6);
    CHECK(g_wrong_side == 0);
}

static void test_event_text_unknown_values(void) {
    lg_ctl_event_t e;
    char buf[200];
    size_t n;

    memset(&e, 0, sizeof e);
    e.seq = 3;
    e.t_ms = 99;
    e.kind = 0; /* not a known kind */
    e.code = 42;
    n = lg_ctl_event_text(&e, buf, sizeof buf);
    CHECK(n == strlen(buf));
    CHECK(strstr(buf, "seq=3 t_ms=99 kind=UNKNOWN code=42") == buf);
    CHECK(buf[n - 1] == '\n');
    e.kind = 200;
    n = lg_ctl_event_text(&e, buf, sizeof buf);
    CHECK(strstr(buf, "kind=UNKNOWN") != NULL && n > 0);

    /* Unknown severity, sensor and diag values never crash and show a placeholder. */
    e.kind = LG_EVT_NOTIFY;
    e.severity = 77;
    e.dir = 9;
    e.id = 9;
    e.code = 99;
    n = lg_ctl_event_text(&e, buf, sizeof buf);
    CHECK(n == strlen(buf));
    CHECK(strstr(buf, "severity=?") != NULL);
    CHECK(strstr(buf, "sensor=?/?") != NULL);
    CHECK(strstr(buf, "diag=UNKNOWN") != NULL);
    e.kind = LG_EVT_LEVEL_CHANGE;
    n = lg_ctl_event_text(&e, buf, sizeof buf);
    CHECK(strstr(buf, "kind=LEVEL severity=?") != NULL && n > 0);
}

/* ---- 8. No tickets at boot or after a controller restart ---- */

static void test_no_ticket_before_a_sensor_was_fine(void) {
    lg_ctl_t c;
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    uint32_t t = 0;
    int d;
    int i;

    reset_world();
    lg_ctl_init(&c, &cfg, t);
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            g_mute[d][i] = 1; /* nothing is sampled yet: every sensor looks failed */
        }
    }
    g_mute[W][A] = 0; /* only west A is up */
    run(&c, &t, 8000);
    CHECK(c.tickets == 0);
    CHECK(count_notify(&c, LG_SEV_URGENT) == 0);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE);
    /* West A was fine once and now goes silent: the one and only ticket. */
    g_mute[W][A] = 1;
    run(&c, &t, 400);
    CHECK(c.tickets == 1);
    CHECK(c.ticket_open[W][A] == 1);
    CHECK(c.ticket_open[W][B] == 0 && c.ticket_open[E][I] == 0);
    CHECK(g_wrong_side == 0);
}

static void test_restarted_controller_raises_no_early_tickets(void) {
    lg_ctl_t c;
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    uint32_t t = 0;
    uint8_t west_b = (uint8_t)(LG_PROC_SENSOR_BASE + W * LG_SENSOR_ID_COUNT + B);
    int d;
    int i;

    reset_world();
    cfg.restarted = 1;
    lg_ctl_init(&c, &cfg, t);
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            g_mute[d][i] = 1;
        }
    }
    CHECK(lg_ctl_on_proc_status(&c, west_b, LG_PROC_RESTARTING, 1, t) == 0);
    run(&c, &t, 1000);
    CHECK(c.tickets == 0);
    CHECK(lg_ctl_on_proc_status(&c, west_b, LG_PROC_RUNNING, 1, t) == 0);
    memset(g_mute, 0, sizeof g_mute);
    run(&c, &t, 10000);
    CHECK(c.tickets == 0);
    CHECK(g_last.level == LG_LEVEL_FAILSAFE); /* no boot release: left only through a guard ack */
    CHECK(g_gate == LG_GATE_DOWN);
    /* Everything was fine once; now an island goes silent: a ticket. */
    g_mute[E][I] = 1;
    run(&c, &t, 400);
    CHECK(c.tickets == 1);
    CHECK(g_gate == LG_GATE_DOWN);
    CHECK(g_wrong_side == 0);
}

int main(void) {
    test_time_wrap_in_stuck_rules();
    test_flicker_run_is_replaced_before_judgement();
    test_first_diagnosis_is_kept();
    test_one_run_releases_both_latches();
    test_island_never_gets_a_stuck_diagnosis();
    test_silent_island_raises_urgent_ticket();
    test_stale_sensor_ticket();
    test_blind_sensor_ticket_and_no_stuck_judgement();
    test_single_blind_sample_raises_no_ticket();
    test_event_seq_is_continuous();
    test_event_text_unknown_values();
    test_no_ticket_before_a_sensor_was_fine();
    test_restarted_controller_raises_no_early_tickets();
    LG_TEST_DONE("test_step09_extra");
}
