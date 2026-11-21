/*
 * Host scenario replay for Step 9 (README 9, level 2): one approach sensor fails during traffic (stuck
 * occupied, stuck clear, crash/hang as a silent gap followed by a fresh process). Plant model + six
 * sensor models + gate model + controller logic, stepped every 10 ms with fixed seeds. Host only, not
 * verified on QNX. The wrong-side assertion is present in every run.
 */
#include <stdio.h>
#include <string.h>

#include "lg_config.h"
#include "lg_controller_logic.h"
#include "lg_gate_model.h"
#include "lg_json_mini.h"
#include "lg_metrics.h"
#include "lg_plant_model.h"
#include "lg_scenario.h"
#include "lg_sensor_model.h"
#include "../unit/lg_test.h"

#define SEED 20261121u
#define END_MS 300000u
#define FAULT_SILENT 0xFEu /* pseudo fault: the process is gone between inject and clear, then restarts */

static lg_json_doc_t g_doc;
static char g_text[16384];
static lg_crossing_cfg_t g_cc;
static lg_scenario_t g_sc;

static int load(const char *crossing, const char *scenario) {
    char err[LG_ERR_LEN];
    size_t n;

    if (lg_read_file(crossing, g_text, sizeof g_text, &n) != 0 || lg_json_parse(&g_doc, g_text, n) != 0 ||
        lg_crossing_from_json(&g_doc, &g_cc, err, sizeof err) != 0) {
        return -1;
    }
    if (lg_read_file(scenario, g_text, sizeof g_text, &n) != 0 || lg_json_parse(&g_doc, g_text, n) != 0 ||
        lg_scenario_from_json(&g_doc, &g_cc, &g_sc, err, sizeof err) != 0) {
        return -1;
    }
    return 0;
}

typedef struct {
    const char *name;
    uint8_t dir;
    uint8_t id;
    uint8_t fault; /* LG_SFAULT_* or FAULT_SILENT; LG_SFAULT_NONE = baseline */
    uint32_t inject_ms;
    uint32_t clear_ms;
    uint8_t expect_diag; /* lg_ctl_diag_t of the ticket */
} fcase_t;

typedef struct {
    lg_metrics_t metrics;
    lg_ctl_t ctl;
    uint32_t boot_release_ms;
    uint32_t max_level_after_boot;
    uint32_t level4_after_boot;
    uint32_t first_above0_ms; /* first tick after the boot release with level > 0 */
    uint32_t back_to0_ms;     /* first tick with level 0 again after that */
    uint8_t gate_state_end;
} fres_t;

/* 0 = ideal delivery; otherwise every island sample at a multiple of this period arrives stale. */
static uint32_t g_stale_period_ms;

static void run_case(const fcase_t *fc, uint32_t seed, fres_t *r) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_plant_t plant;
    lg_sensor_model_t sm[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    lg_gate_model_t gate;
    lg_gate_cfg_t gcfg = lg_gate_cfg_default();
    uint32_t t;
    int d;
    int i;

    memset(r, 0, sizeof *r);
    lg_plant_init(&plant, &g_cc, &g_sc);
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            lg_sensor_model_init(&sm[d][i], seed + (uint32_t)(d * 3 + i), 0);
        }
    }
    lg_gate_model_init(&gate, &gcfg, 0);
    lg_ctl_init(&r->ctl, &cfg, 0);
    lg_metrics_init(&r->metrics, 0);

    for (t = 10; t <= END_MS; t += LG_TICK_MS) {
        lg_ctl_out_t out;
        lg_truth_t island_truth;
        uint8_t prev_level = r->ctl.mode.level;
        int silent = 0;

        if (fc->fault != LG_SFAULT_NONE) {
            lg_sensor_model_t *m = &sm[fc->dir][fc->id];

            if (fc->fault == FAULT_SILENT) {
                silent = t >= fc->inject_ms && t < fc->clear_ms;
                if (t == fc->clear_ms) {
                    lg_sensor_model_init(m, seed + 977u + t, t); /* a new process */
                }
            } else if (t == fc->inject_ms) {
                CHECK(lg_sensor_model_set_fault(m, fc->fault, 0, t) == 0);
            } else if (t == fc->clear_ms) {
                CHECK(lg_sensor_model_set_fault(m, LG_SFAULT_NONE, 0, t) == 0);
            }
        }
        for (d = 0; d < LG_DIR_COUNT; d++) {
            for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                lg_truth_t tr = lg_plant_truth(&plant, t, (uint8_t)d, (uint8_t)i);
                lg_sensor_out_t so;

                if (silent && d == fc->dir && i == fc->id) {
                    continue;
                }
                if (lg_sensor_model_step(&sm[d][i], t, tr.occupied, tr.axles, &so)) {
                    /* A stalled delivery: the island sample arrives 30 ms old (stale) now and then. */
                    uint32_t age = (g_stale_period_ms != 0 && i == LG_SENSOR_ISLAND &&
                                    (t % g_stale_period_ms) == 0)
                                       ? 30u
                                       : 0u;

                    (void)lg_ctl_on_sample(&r->ctl, (uint8_t)d, (uint8_t)i, so.occupied, so.quality,
                                           so.axle_count, t - age, t);
                }
            }
        }
        out = lg_ctl_step(&r->ctl, t);
        (void)lg_gate_model_command(
            &gate, out.gate_target == LG_GATE_TARGET_UP ? LG_GATE_CMD_RAISE : LG_GATE_CMD_LOWER, t);
        (void)lg_gate_model_command(&gate, out.lights ? LG_GATE_CMD_LIGHTS_ON : LG_GATE_CMD_LIGHTS_OFF, t);
        (void)lg_gate_model_command(&gate, out.bell ? LG_GATE_CMD_BELL_ON : LG_GATE_CMD_BELL_OFF, t);
        lg_gate_model_step(&gate, t);
        (void)lg_ctl_on_gate_reply(&r->ctl, gate.state, t);

        island_truth = lg_plant_truth(&plant, t, LG_DIR_WEST, LG_SENSOR_ISLAND);
        lg_metrics_tick(&r->metrics, t, gate.state, gate.lights, island_truth.island_any, 0);

        if (prev_level == LG_LEVEL_FAILSAFE && r->ctl.mode.level != LG_LEVEL_FAILSAFE &&
            r->boot_release_ms == 0) {
            r->boot_release_ms = t;
        }
        if (r->boot_release_ms != 0 && t > r->boot_release_ms) {
            uint8_t lv = r->ctl.mode.level;

            if (lv > r->max_level_after_boot) {
                r->max_level_after_boot = lv;
            }
            if (lv == LG_LEVEL_FAILSAFE) {
                r->level4_after_boot++;
            }
            if (lv > LG_LEVEL_NORMAL && r->first_above0_ms == 0) {
                r->first_above0_ms = t;
            }
            if (lv == LG_LEVEL_NORMAL && r->first_above0_ms != 0 && r->back_to0_ms == 0) {
                r->back_to0_ms = t;
            }
        }
    }
    r->gate_state_end = gate.state;
}

/* The ticket of the case: URGENT, the faulty sensor, the expected diagnosis. */
static int has_ticket(const lg_ctl_t *c, uint8_t dir, uint8_t id, uint8_t diag) {
    lg_ctl_event_t ev[LG_CTL_EVENT_RING];
    uint32_t n = lg_ctl_events_since(c, 0, ev, LG_CTL_EVENT_RING);
    uint32_t k;

    for (k = 0; k < n; k++) {
        if (ev[k].kind == LG_EVT_NOTIFY && ev[k].severity == LG_SEV_URGENT && ev[k].dir == dir &&
            ev[k].id == id && ev[k].code == diag) {
            return 1;
        }
    }
    return 0;
}

static void check_protected(const fres_t *r) {
    CHECK(lg_metrics_wrong_side_ok(&r->metrics)); /* hard rule: never relaxed */
    CHECK(r->metrics.wrong_side_ticks == 0);
    CHECK(r->metrics.trains == g_sc.n_trains);
    CHECK(r->metrics.trains_without_warning == 0);
    CHECK(r->level4_after_boot == 0);
    CHECK(r->ctl.mode.level == LG_LEVEL_NORMAL);
    CHECK(r->gate_state_end == LG_GATE_UP);
}

static void test_baseline_no_false_tickets(void) {
    static const fcase_t base = {"baseline", 0, 0, LG_SFAULT_NONE, 0, 0, LG_CTL_DIAG_NONE};
    fres_t r;
    uint32_t s;

    for (s = 0; s < 8; s++) { /* different sensor noise each time */
        run_case(&base, SEED + 1000u * s, &r);
        printf("baseline seed=%u trains=%u cycles=%u tickets=%u max_level_after_boot=%u wrong_side=%u\n",
               (unsigned)(SEED + 1000u * s), (unsigned)r.metrics.trains, (unsigned)r.ctl.cycles,
               (unsigned)r.ctl.tickets, (unsigned)r.max_level_after_boot,
               (unsigned)r.metrics.wrong_side_events);
        check_protected(&r);
        CHECK(r.ctl.tickets == 0);
        CHECK(r.max_level_after_boot == LG_LEVEL_NORMAL);
        CHECK(r.ctl.cycles == g_sc.n_trains);
    }
}

/*
 * Times (scenario sensor_faults.json): west trains strike in at 20, 100, 180 s, east at 60, 150, 220 s.
 * A stuck fault is cleared after the train it hurt; the next train of that direction is seen by both
 * sensors, which releases the latch, and level 0 follows T_stable later.
 */
static const fcase_t g_cases[] = {
    {"stuck_occupied west/a before train 1", LG_DIR_WEST, LG_SENSOR_A, LG_SFAULT_STUCK_OCCUPIED, 10000, 50000,
     LG_CTL_DIAG_STUCK_OCCUPIED},
    {"stuck_clear west/a before train 1", LG_DIR_WEST, LG_SENSOR_A, LG_SFAULT_STUCK_CLEAR, 10000, 50000,
     LG_CTL_DIAG_MISSED_TRAIN},
    {"stuck_clear east/b during train 2", LG_DIR_EAST, LG_SENSOR_B, LG_SFAULT_STUCK_CLEAR, 61000, 165000,
     LG_CTL_DIAG_MISSED_TRAIN},
    {"stuck_occupied east/a during train 2", LG_DIR_EAST, LG_SENSOR_A, LG_SFAULT_STUCK_OCCUPIED, 61000, 100000,
     LG_CTL_DIAG_STUCK_OCCUPIED},
    {"crash west/a during train 1 (silent 300 ms)", LG_DIR_WEST, LG_SENSOR_A, FAULT_SILENT, 21000, 21300,
     LG_CTL_DIAG_SILENT},
    {"hang east/a during train 2 (silent 500 ms)", LG_DIR_EAST, LG_SENSOR_A, FAULT_SILENT, 61000, 61500,
     LG_CTL_DIAG_SILENT},
};

static void test_fault_cases(void) {
    size_t k;

    for (k = 0; k < sizeof g_cases / sizeof g_cases[0]; k++) {
        const fcase_t *fc = &g_cases[k];
        fres_t r;
        int ticket;
        uint32_t rec_inject;
        uint32_t rec_clear;

        run_case(fc, SEED, &r);
        ticket = has_ticket(&r.ctl, fc->dir, fc->id, fc->expect_diag);
        rec_inject = r.back_to0_ms > fc->inject_ms ? r.back_to0_ms - fc->inject_ms : 0;
        rec_clear = r.back_to0_ms > fc->clear_ms ? r.back_to0_ms - fc->clear_ms : 0;
        printf("case \"%s\" seed=%u trains=%u cycles=%u tickets=%u ticket_ok=%d max_level_after_boot=%u "
               "level2_from_ms=%u level0_at_ms=%u recovery_inject_ms=%u recovery_clear_ms=%u "
               "closure_per_hour_ms=%llu wrong_side=%u\n",
               fc->name, (unsigned)SEED, (unsigned)r.metrics.trains, (unsigned)r.ctl.cycles,
               (unsigned)r.ctl.tickets, ticket, (unsigned)r.max_level_after_boot,
               (unsigned)r.first_above0_ms, (unsigned)r.back_to0_ms, (unsigned)rec_inject,
               (unsigned)rec_clear, (unsigned long long)lg_metrics_closure_ms_per_hour(&r.metrics),
               (unsigned)r.metrics.wrong_side_events);
        check_protected(&r);
        CHECK(ticket);
        CHECK(r.ctl.tickets == 1);
        CHECK(r.max_level_after_boot == LG_LEVEL_DEGRADED); /* level 2, never 3 or 4 */
        CHECK(r.first_above0_ms >= fc->inject_ms);
        CHECK(r.back_to0_ms > fc->clear_ms);
        CHECK(rec_clear >= LG_T_STABLE_MS); /* de-escalation needs T_stable */
        CHECK(lg_ctl_sensor_diag(&r.ctl, fc->dir, fc->id) == LG_CTL_DIAG_NONE);
    }
}

/*
 * Step 9 review: on the loaded VM single island samples arrived stale often enough that each one
 * restarted the T_clear window, and a direction stayed active (gate DOWN) for good. One stale island
 * sample every 1.9 s (shorter than T_clear 2 s) must not keep the crossing closed; the gate is never
 * released on a stale tick, and the wrong-side check is the same as everywhere.
 */
static void test_stale_island_ticks(void) {
    static const fcase_t base = {"baseline, stale island ticks", 0, 0, LG_SFAULT_NONE, 0, 0, LG_CTL_DIAG_NONE};
    static const uint32_t periods[] = {1900, 990, 470};
    fres_t r;
    size_t k;

    for (k = 0; k < sizeof periods / sizeof periods[0]; k++) {
        g_stale_period_ms = periods[k];
        run_case(&base, SEED, &r);
        printf("stale island every %u ms: trains=%u cycles=%u tickets=%u max_level_after_boot=%u unmet=0x%x,0x%x "
               "wrong_side=%u\n",
               (unsigned)periods[k], (unsigned)r.metrics.trains, (unsigned)r.ctl.cycles, (unsigned)r.ctl.tickets,
               (unsigned)r.max_level_after_boot, (unsigned)r.ctl.last_unmet[0], (unsigned)r.ctl.last_unmet[1],
               (unsigned)r.metrics.wrong_side_events);
        check_protected(&r);
        CHECK(r.ctl.cycles == g_sc.n_trains);
        CHECK(r.ctl.tickets == 0); /* a single stale tick never fails a sensor */
    }
    g_stale_period_ms = 0;
}

int main(void) {
    if (load("config/crossing.json", "scenarios/sensor_faults.json") != 0) {
        puts("FAIL: could not load config/crossing.json or scenarios/sensor_faults.json (run from the repo root)");
        return 1;
    }
    test_baseline_no_false_tickets();
    test_fault_cases();
    test_stale_island_ticks();
    LG_TEST_DONE("replay_sensor_faults");
}
