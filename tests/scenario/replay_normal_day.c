/*
 * Host scenario replay (README 11): plant model + six sensor models + gate model + controller logic,
 * stepped every 10 ms with a fixed seed. Everything is simulated time and portable core code: this is
 * "host only, not verified on QNX". The wrong-side assertion is present in every run.
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

#define SEED 20261120u

static lg_json_doc_t g_doc;
static char g_text[16384];

static int load(const char *crossing, const char *scenario, lg_crossing_cfg_t *cc, lg_scenario_t *sc) {
    char err[LG_ERR_LEN];
    size_t n;

    if (lg_read_file(crossing, g_text, sizeof g_text, &n) != 0 || lg_json_parse(&g_doc, g_text, n) != 0 ||
        lg_crossing_from_json(&g_doc, cc, err, sizeof err) != 0) {
        return -1;
    }
    if (lg_read_file(scenario, g_text, sizeof g_text, &n) != 0 || lg_json_parse(&g_doc, g_text, n) != 0 ||
        lg_scenario_from_json(&g_doc, cc, sc, err, sizeof err) != 0) {
        return -1;
    }
    return 0;
}

typedef struct {
    lg_metrics_t metrics;
    lg_ctl_t ctl;
    uint32_t boot_release_ms;
    uint32_t max_level;
    uint32_t level4_after_boot;
    uint8_t gate_state_end;
    uint8_t lights_end;
} result_t;

/* Simulated time at which the controller is re-initialised (a supervisor restart); 0 = never. */
static uint32_t g_restart_ms;

static void run(const lg_crossing_cfg_t *cc, const lg_scenario_t *sc, const lg_ctl_cfg_t *cfg,
                uint32_t end_ms, result_t *r) {
    lg_plant_t plant;
    lg_sensor_model_t sm[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    lg_gate_model_t gate;
    lg_gate_cfg_t gcfg = lg_gate_cfg_default();
    uint32_t t;
    int d;
    int i;

    memset(r, 0, sizeof *r);
    lg_plant_init(&plant, cc, sc);
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            lg_sensor_model_init(&sm[d][i], SEED + (uint32_t)(d * 3 + i), 0);
        }
    }
    lg_gate_model_init(&gate, &gcfg, 0);
    lg_ctl_init(&r->ctl, cfg, 0);
    lg_metrics_init(&r->metrics, 0);

    for (t = 10; t <= end_ms; t += LG_TICK_MS) {
        lg_ctl_out_t out;
        lg_truth_t island_truth;
        uint8_t prev_level = r->ctl.mode.level;

        if (g_restart_ms != 0 && t == g_restart_ms) {
            lg_ctl_init(&r->ctl, cfg, t); /* a new controller: boots in level 4, knows nothing */
            prev_level = LG_LEVEL_FAILSAFE;
        }
        for (d = 0; d < LG_DIR_COUNT; d++) {
            for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                lg_truth_t tr = lg_plant_truth(&plant, t, (uint8_t)d, (uint8_t)i);
                lg_sensor_out_t so;

                if (lg_sensor_model_step(&sm[d][i], t, tr.occupied, tr.axles, &so)) {
                    (void)lg_ctl_on_sample(&r->ctl, (uint8_t)d, (uint8_t)i, so.occupied, so.quality,
                                           so.axle_count, t, t);
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
        if (r->boot_release_ms != 0 && r->ctl.mode.level == LG_LEVEL_FAILSAFE) {
            r->level4_after_boot++;
        }
        if (r->ctl.mode.level > r->max_level) {
            r->max_level = r->ctl.mode.level;
        }
    }
    r->gate_state_end = gate.state;
    r->lights_end = gate.lights;
}

static lg_crossing_cfg_t g_cc;
static lg_scenario_t g_sc;

static void test_normal_day(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    result_t r;

    run(&g_cc, &g_sc, &cfg, 300000, &r);
    printf("seed=%u boot_release_ms=%u trains=%u cycles=%u activations=%u wrong_side=%u "
           "closure_per_hour_ms=%llu\n",
           SEED, (unsigned)r.boot_release_ms, (unsigned)r.metrics.trains, (unsigned)r.ctl.cycles,
           (unsigned)r.ctl.activations, (unsigned)r.metrics.wrong_side_events,
           (unsigned long long)lg_metrics_closure_ms_per_hour(&r.metrics));

    CHECK(lg_metrics_wrong_side_ok(&r.metrics)); /* hard rule: never relaxed */
    CHECK(r.metrics.wrong_side_ticks == 0);
    CHECK(r.boot_release_ms >= LG_T_BOOT_STABLE_MS && r.boot_release_ms < 10000);
    CHECK(r.max_level == LG_LEVEL_FAILSAFE); /* only the boot level 4 */
    CHECK(r.level4_after_boot == 0);
    CHECK(r.ctl.mode.level == LG_LEVEL_NORMAL);
    CHECK(g_sc.n_trains == 6);
    CHECK(r.metrics.trains == 6);
    CHECK(r.metrics.trains_without_warning == 0);
    CHECK(r.ctl.activations == 6);
    CHECK(r.ctl.cycles == 6);
    CHECK(r.gate_state_end == LG_GATE_UP && r.lights_end == 0);
    CHECK(r.metrics.closure.count == 6);
}

/*
 * Negative control for the assertion itself: with the old 10 s pre-warning the fastest train (30 m/s)
 * reaches the island before the gate is down. The metric must see it. If this ever stops failing, the
 * wrong-side check is broken, not the controller.
 */
static void test_old_prewarning_is_caught(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    result_t r;

    cfg.vote.prewarning_ms = 10000;
    run(&g_cc, &g_sc, &cfg, 300000, &r);
    CHECK(r.metrics.wrong_side_events > 0);
    CHECK(!lg_metrics_wrong_side_ok(&r.metrics));
}

/*
 * The safety budget per train: the gate must be down before the train reaches the island. The
 * controller does not know the speed, so the configuration has to hold for the fastest train of
 * every scenario: pre-warning delay + nominal gate travel + 1 s margin below the time the train
 * needs from the strike-in point to the island.
 */
static void test_prewarning_budget(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    uint32_t i;

    for (i = 0; i < g_sc.n_trains; i++) {
        const lg_train_t *tr = &g_sc.trains[i];
        uint32_t t_island_ms = (uint32_t)((int64_t)g_cc.strike_in_mm * 1000 / tr->speed_mmps);

        CHECK(lg_lower_delay_ms(&cfg.vote, LG_LEVEL_NORMAL) + LG_GATE_TRAVEL_MS + 1000u <= t_island_ms);
    }
}

/*
 * A controller restart at any moment of the day (also while a train is between the approach zone
 * and the island) must not produce a wrong-side event. Restarts every 997 ms across the whole
 * scenario, each one a full replay.
 */
static void test_restart_sweep(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    result_t r;
    uint32_t restart;
    uint32_t runs = 0;
    uint32_t bad = 0;
    uint32_t first_bad = 0;

    for (restart = 5000; restart <= 285000; restart += 997) {
        g_restart_ms = restart;
        run(&g_cc, &g_sc, &cfg, 300000, &r);
        runs++;
        if (!lg_metrics_wrong_side_ok(&r.metrics)) {
            if (bad == 0) {
                first_bad = restart;
            }
            bad++;
        }
    }
    g_restart_ms = 0;
    printf("restart sweep: %u runs, %u with wrong-side events (first at %u ms)\n", (unsigned)runs,
           (unsigned)bad, (unsigned)first_bad);
    CHECK(runs > 200);
    CHECK(bad == 0);
}

int main(void) {
    if (load("config/crossing.json", "scenarios/normal_day.json", &g_cc, &g_sc) != 0) {
        puts("FAIL: could not load config/crossing.json or scenarios/normal_day.json (run from the repo root)");
        return 1;
    }
    test_normal_day();
    test_old_prewarning_is_caught();
    test_prewarning_budget();
    test_restart_sweep();
    LG_TEST_DONE("replay_normal_day");
}
