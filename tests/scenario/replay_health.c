/*
 * Host scenario replay for Step 11 (README 9, level 1 early warning): plant model + six sensor models
 * (with their statistics) + gate and battery models + the health core read like lg-health reads the
 * devices + controller logic with health_required, stepped every 10 ms with fixed seeds.
 * Host only, not verified on QNX. The wrong-side assertion is present in every run.
 *
 * Cases: a healthy baseline (no warning at all), DRIFT on west/a (WARNING and a maintenance
 * notification before FAILED, the gate cycle unchanged until FAILED, lead time recorded), and a gate
 * wearing out in steps (SLOW 125, 150, 180 %: WARNING before FAILED).
 */
#include <stdio.h>
#include <string.h>

#include "lg_battery_model.h"
#include "lg_config.h"
#include "lg_controller_logic.h"
#include "lg_gate_model.h"
#include "lg_health.h"
#include "lg_json_mini.h"
#include "lg_metrics.h"
#include "lg_plant_model.h"
#include "lg_scenario.h"
#include "lg_sensor_model.h"
#include "lg_time.h"
#include "../unit/lg_test.h"

#define SEED 20261124u
#define END_MS 340000u
#define TRACE_LEN (END_MS / LG_TICK_MS + 1)

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
    uint32_t t_ms;
    uint8_t sensor_fault; /* on west/a */
    uint32_t sensor_param;
    uint8_t gate_fault;
    uint32_t gate_pct;
} step_t;

typedef struct {
    const char *name;
    const step_t *steps;
    uint32_t n_steps;
} hcase_t;

typedef struct {
    lg_metrics_t metrics;
    lg_ctl_t ctl;
    lg_health_t health;
    uint32_t boot_release_ms;
    uint32_t max_level_after_boot;
    uint32_t level1_ticks;
    uint8_t trace[TRACE_LEN]; /* gate target, lights, bell per tick */
} hres_t;

static uint32_t g_seed;

static void run_case(const hcase_t *hc, hres_t *r) {
    static lg_sensor_model_t sm[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    static lg_sensor_acc_t acc[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_health_cfg_t hcfg = lg_health_cfg_default();
    lg_plant_t plant;
    lg_gate_model_t gate;
    lg_gate_cfg_t gcfg = lg_gate_cfg_default();
    lg_battery_model_t batt;
    uint32_t next_poll[LG_COMP_COUNT];
    uint32_t last_sent[LG_COMP_COUNT];
    lg_health_out_t sent[LG_COMP_COUNT];
    uint32_t t;
    uint32_t s = 0;
    int d;
    int i;

    memset(r, 0, sizeof *r);
    memset(acc, 0, sizeof acc);
    memset(sent, 0, sizeof sent);
    lg_plant_init(&plant, &g_cc, &g_sc);
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            lg_sensor_model_init(&sm[d][i], g_seed + (uint32_t)(d * 3 + i), 0);
        }
    }
    lg_gate_model_init(&gate, &gcfg, 0);
    lg_battery_model_init(&batt, 0);
    cfg.health_required = 1;
    lg_ctl_init(&r->ctl, &cfg, 0);
    lg_health_init(&r->health, &hcfg, 0);
    lg_metrics_init(&r->metrics, 0);
    for (i = 0; i < LG_COMP_COUNT; i++) {
        next_poll[i] = 100;
        last_sent[i] = 0;
        sent[i].verdict = 0xFF; /* nothing sent yet */
    }

    for (t = 10; t <= END_MS; t += LG_TICK_MS) {
        lg_ctl_out_t out;
        lg_truth_t island_truth;
        uint8_t prev_level = r->ctl.mode.level;

        for (; s < hc->n_steps && hc->steps[s].t_ms == t; s++) {
            const step_t *st = &hc->steps[s];

            CHECK(lg_sensor_model_set_fault(&sm[LG_DIR_WEST][LG_SENSOR_A], st->sensor_fault, st->sensor_param, t) ==
                  0);
            CHECK(lg_gate_model_set_fault(&gate, st->gate_fault, st->gate_pct, t) == 0);
        }
        for (d = 0; d < LG_DIR_COUNT; d++) {
            for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                lg_truth_t tr = lg_plant_truth(&plant, t, (uint8_t)d, (uint8_t)i);
                lg_sensor_out_t so;
                int have = lg_sensor_model_step(&sm[d][i], t, tr.occupied, tr.axles, &so);

                lg_sensor_acc_step(&acc[d][i], t, have, &so);
                if (have) {
                    (void)lg_ctl_on_sample(&r->ctl, (uint8_t)d, (uint8_t)i, so.occupied, so.quality,
                                           so.axle_count, t, t);
                }
            }
        }

        /* lg-health: read what is due, send what changed and refresh every 500 ms. */
        for (i = 0; i < LG_COMP_COUNT; i++) {
            if (i == LG_COMP_BATTERY || (int32_t)(t - next_poll[i]) < 0) {
                continue;
            }
            if (i == LG_COMP_GATE) {
                lg_health_gate_in_t gin;

                lg_battery_model_step(&batt, t);
                gin.moves_done = gate.moves_done;
                gin.last_travel_ms = gate.last_travel_ms;
                gin.last_peak_ma = gate.last_peak_ma;
                gin.battery_mv = lg_battery_model_mv(&batt);
                lg_health_on_gate(&r->health, &gin, t);
            } else {
                const lg_sensor_acc_t *a = &acc[i / LG_SENSOR_ID_COUNT][i % LG_SENSOR_ID_COUNT];
                lg_health_sensor_in_t sin;

                sin.ticks = a->ticks;
                sin.no_sample = a->no_sample;
                sin.blind = a->blind;
                sin.dev_count = a->dev_count;
                sin.dev_sum = a->dev_sum;
                sin.rise_count = a->rise_count;
                sin.last_rise_ms = a->last_rise_ms;
                sin.last_fall_ms = a->last_fall_ms;
                (void)lg_health_on_sensor(&r->health, (uint8_t)i, &sin, t);
            }
            next_poll[i] = t + lg_health_poll_period_ms(&r->health, (uint8_t)i);
        }
        for (i = 0; i < LG_COMP_COUNT; i++) {
            lg_health_out_t o = lg_health_get(&r->health, (uint8_t)i);

            if (o.verdict != sent[i].verdict || o.diag != sent[i].diag || o.assessed != sent[i].assessed ||
                lg_elapsed_ge(t, last_sent[i], LG_HEALTH_REFRESH_MS)) {
                CHECK(lg_ctl_on_health_verdict_flags(&r->ctl, (uint8_t)i, o.score, o.verdict, o.diag, o.trend_s,
                                                     o.assessed ? (uint8_t)LG_HV_ASSESSED : (uint8_t)0, t) == 0);
                sent[i] = o;
                last_sent[i] = t;
            }
        }

        out = lg_ctl_step(&r->ctl, t);
        (void)lg_gate_model_command(&gate, out.gate_target == LG_GATE_TARGET_UP ? LG_GATE_CMD_RAISE : LG_GATE_CMD_LOWER,
                                    t);
        (void)lg_gate_model_command(&gate, out.lights ? LG_GATE_CMD_LIGHTS_ON : LG_GATE_CMD_LIGHTS_OFF, t);
        (void)lg_gate_model_command(&gate, out.bell ? LG_GATE_CMD_BELL_ON : LG_GATE_CMD_BELL_OFF, t);
        lg_gate_model_step(&gate, t);
        (void)lg_ctl_on_gate_reply(&r->ctl, gate.state, t);
        r->trace[t / LG_TICK_MS] = (uint8_t)(out.gate_target | (out.lights << 1) | (out.bell << 2));

        island_truth = lg_plant_truth(&plant, t, LG_DIR_WEST, LG_SENSOR_ISLAND);
        lg_metrics_tick(&r->metrics, t, gate.state, gate.lights, island_truth.island_any, 0);

        if (prev_level == LG_LEVEL_FAILSAFE && r->ctl.mode.level != LG_LEVEL_FAILSAFE && r->boot_release_ms == 0) {
            r->boot_release_ms = t;
        }
        if (r->boot_release_ms != 0 && t > r->boot_release_ms) {
            if (r->ctl.mode.level > r->max_level_after_boot) {
                r->max_level_after_boot = r->ctl.mode.level;
            }
            if (r->ctl.mode.level == LG_LEVEL_EARLY_WARNING) {
                r->level1_ticks++;
            }
        }
    }
}

/* First event matching kind/severity/comp (0 when none). */
static lg_ctl_event_t find_event(const lg_ctl_t *c, uint8_t kind, uint8_t sev, uint8_t comp) {
    lg_ctl_event_t ev[LG_CTL_EVENT_RING];
    lg_ctl_event_t none;
    uint32_t n = lg_ctl_events_since(c, 0, ev, LG_CTL_EVENT_RING);
    uint32_t k;

    memset(&none, 0, sizeof none);
    for (k = 0; k < n; k++) {
        if (ev[k].kind == kind && (sev == 0xFF || ev[k].severity == sev) && ev[k].comp == comp) {
            return ev[k];
        }
    }
    return none;
}

static void check_protected(const hres_t *r) {
    CHECK(lg_metrics_wrong_side_ok(&r->metrics)); /* hard rule: never relaxed */
    CHECK(r->metrics.wrong_side_ticks == 0);
    CHECK(r->metrics.trains == g_sc.n_trains);
    CHECK(r->metrics.trains_without_warning == 0);
    CHECK(r->boot_release_ms > 0);
}

static hres_t g_base;
static hres_t g_res;

static void test_baseline(void) {
    static const hcase_t base = {"baseline", NULL, 0};
    uint32_t k;

    for (k = 0; k < 4; k++) {
        g_seed = SEED + 1000u * k;
        run_case(&base, &g_res);
        printf("baseline seed=%u trains=%u cycles=%u warnings=%u tickets=%u max_level=%u wrong_side=%u\n",
               (unsigned)g_seed, (unsigned)g_res.metrics.trains, (unsigned)g_res.ctl.cycles,
               (unsigned)g_res.ctl.warnings, (unsigned)g_res.ctl.tickets, (unsigned)g_res.max_level_after_boot,
               (unsigned)g_res.metrics.wrong_side_events);
        check_protected(&g_res);
        CHECK(g_res.ctl.warnings == 0);
        CHECK(g_res.ctl.tickets == 0);
        CHECK(g_res.max_level_after_boot == LG_LEVEL_NORMAL);
        CHECK(g_res.ctl.cycles == g_sc.n_trains);
    }
    g_seed = SEED;
    run_case(&base, &g_base);
}

/* DRIFT 1 unit/s on west/a from 62 s: train 2 (east, 60 s) and its whole gate cycle run in level 1. */
static void test_drift(void) {
    static const step_t steps[] = {{62000, LG_SFAULT_DRIFT, 1, LG_GFAULT_NONE, 100}};
    static const hcase_t hc = {"drift_west_a", steps, 1};
    lg_ctl_event_t warn, fail, lead;
    uint32_t k;
    uint32_t same = 1;

    g_seed = SEED;
    run_case(&hc, &g_res);
    warn = find_event(&g_res.ctl, LG_EVT_NOTIFY, LG_SEV_WARNING, 0);
    fail = find_event(&g_res.ctl, LG_EVT_NOTIFY, LG_SEV_URGENT, 0);
    lead = find_event(&g_res.ctl, LG_EVT_METRIC, 0xFF, 0);
    printf("drift: warning at %u ms (%s, trend %d s), failed at %u ms (%s), lead_time_ms=%d, cycles=%u, "
           "level1 ticks=%u\n",
           (unsigned)warn.t_ms, lg_ctl_diag_name(warn.code), (int)warn.value, (unsigned)fail.t_ms,
           lg_ctl_diag_name(fail.code), (int)lead.value, (unsigned)g_res.ctl.cycles, (unsigned)g_res.level1_ticks);
    check_protected(&g_res);
    CHECK(warn.seq != 0 && warn.code == LG_CTL_DIAG_HEALTH_BASE + LG_DIAG_NOISE_RISING);
    CHECK(fail.seq > warn.seq);
    CHECK(fail.code == LG_CTL_DIAG_HEALTH_BASE + LG_DIAG_NOISE_HIGH); /* past the threshold: high, not rising */
    CHECK(fail.value == -1);
    CHECK(warn.t_ms > 62000u && fail.t_ms > warn.t_ms);
    CHECK(lead.seq != 0 && lead.code == LG_CTL_METRIC_LEAD_TIME_MS);
    CHECK((uint32_t)lead.value == fail.t_ms - warn.t_ms);
    CHECK(lead.value >= 10000); /* 1 unit/s from 3 to 30 units: a warning well ahead */
    CHECK(g_res.level1_ticks > 0);
    CHECK(g_res.ctl.cycles == g_sc.n_trains);
    /* Traffic unaffected in level 1: the gate, lights and bell did exactly what they did without the fault. */
    for (k = 0; k < fail.t_ms / LG_TICK_MS; k++) {
        if (g_res.trace[k] != g_base.trace[k]) {
            same = 0;
            printf("drift: first difference to the baseline at %u ms\n", (unsigned)(k * LG_TICK_MS));
            break;
        }
    }
    CHECK(same);
    /* The healthy partner is never blamed. */
    CHECK(find_event(&g_res.ctl, LG_EVT_NOTIFY, 0xFF, 1).seq == 0);
}

/* Mechanical wear in steps between train cycles: 125 % at 50 s, 150 % at 95 s, 180 % at 135 s. */
static void test_gate_wear(void) {
    static const step_t steps[] = {{50000, LG_SFAULT_NONE, 0, LG_GFAULT_SLOW, 125},
                                   {95000, LG_SFAULT_NONE, 0, LG_GFAULT_SLOW, 150},
                                   {135000, LG_SFAULT_NONE, 0, LG_GFAULT_SLOW, 180}};
    static const hcase_t hc = {"gate_wear", steps, 3};
    lg_ctl_event_t warn, fail, lead;

    g_seed = SEED;
    run_case(&hc, &g_res);
    warn = find_event(&g_res.ctl, LG_EVT_NOTIFY, LG_SEV_WARNING, LG_COMP_GATE);
    fail = find_event(&g_res.ctl, LG_EVT_NOTIFY, LG_SEV_URGENT, LG_COMP_GATE);
    lead = find_event(&g_res.ctl, LG_EVT_METRIC, 0xFF, LG_COMP_GATE);
    printf("gate wear: warning at %u ms (%s, trend %d s), failed at %u ms (%s), lead_time_ms=%d, cycles=%u\n",
           (unsigned)warn.t_ms, lg_ctl_diag_name(warn.code), (int)warn.value, (unsigned)fail.t_ms,
           lg_ctl_diag_name(fail.code), (int)lead.value, (unsigned)g_res.ctl.cycles);
    check_protected(&g_res);
    CHECK(warn.seq != 0 && warn.code == LG_CTL_DIAG_HEALTH_BASE + LG_DIAG_GATE_SLOW);
    CHECK(fail.seq > warn.seq && fail.code == LG_CTL_DIAG_HEALTH_BASE + LG_DIAG_GATE_SLOW);
    CHECK(warn.t_ms < 95000u); /* the 125 % step alone was enough (train 2 lowers at 65 s, is up at 91 s) */
    CHECK((uint32_t)lead.value == fail.t_ms - warn.t_ms);
    CHECK(lead.value > 0);
    CHECK(g_res.ctl.mode.level == LG_LEVEL_FAILSAFE); /* gate failed: level 4, gate held down */
    /* Trains 2 and 3 crossed in level 1 with the slower gate and were protected (cycles counted). */
    CHECK(g_res.ctl.cycles >= 3);
}

int main(void) {
    if (load("config/crossing.json", "scenarios/health_faults.json") != 0) {
        printf("FAIL: replay_health (cannot load config/crossing.json or scenarios/health_faults.json)\n");
        return 1;
    }
    test_baseline();
    test_drift();
    test_gate_wear();
    LG_TEST_DONE("replay_health");
}
