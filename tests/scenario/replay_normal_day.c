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
#include "lg_supervisor_logic.h"
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
    uint32_t above0_after_boot; /* ticks after the boot release with the level above 0 */
    uint32_t level2_after_boot; /* ticks after the boot release with the level exactly 2 */
    uint8_t gate_state_end;
    uint8_t lights_end;
} result_t;

/* Simulated time at which the controller is re-initialised (a supervisor restart); 0 = never. */
static uint32_t g_restart_ms;

/*
 * A process killed under the supervisor (Step 8): when g_kill_proc is a process id, the real supervisor
 * logic runs next to the controller. All processes beat every 50 ms; at g_kill_ms the process dies (a
 * sensor stops sending), the supervisor starts a new one, and it is back g_kill_delay_ms later (first
 * heartbeat and first sample, a fresh sensor model). The supervisor's PROC_STATUS reach the controller
 * at once, repeated every LG_SUP_STATUS_REFRESH_MS like the real one.
 */
static int g_kill_proc = -1;
static uint32_t g_kill_ms;
static uint32_t g_kill_delay_ms;

typedef struct {
    lg_sup_t sup;
    uint32_t pid[LG_PROC_COUNT];
    uint8_t alive[LG_PROC_COUNT];     /* the process sends beats and samples */
    uint32_t up_at_ms[LG_PROC_COUNT]; /* a spawned process comes alive at this time, 0 = none pending */
    uint32_t next_pid;
    uint32_t last_refresh_ms;
} supsim_t;

static int sup_reportable(const supsim_t *s, int p) {
    return s->sup.cfg.installed[p] && p != LG_PROC_CONTROLLER && p != LG_PROC_SUPERVISOR;
}

static void supsim_init(supsim_t *s) {
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    int p;

    memset(s, 0, sizeof *s);
    lg_sup_init(&s->sup, &cfg);
    s->next_pid = 5000;
    for (p = 0; p < LG_PROC_COUNT; p++) {
        if (cfg.installed[p]) {
            s->pid[p] = ++s->next_pid;
            (void)lg_sup_on_spawn(&s->sup, (uint8_t)p, s->pid[p], 0);
            (void)lg_sup_on_heartbeat(&s->sup, (uint8_t)p, s->pid[p], 0); /* the day starts with everything up */
            s->alive[p] = 1;
        }
    }
}

/* One tick of the supervisor and of the world around it. */
static void supsim_tick(supsim_t *s, lg_ctl_t *ctl, lg_sensor_model_t sm[LG_DIR_COUNT][LG_SENSOR_ID_COUNT],
                        uint32_t t) {
    lg_sup_action_t act[LG_SUP_MAX_ACTIONS];
    size_t n;
    size_t i;
    int p;

    if (t == g_kill_ms && g_kill_proc >= 0 && s->alive[g_kill_proc]) {
        s->alive[g_kill_proc] = 0;
        (void)lg_sup_on_exit(&s->sup, s->pid[g_kill_proc], t);
    }
    for (p = 0; p < LG_PROC_COUNT; p++) {
        if (s->up_at_ms[p] != 0 && t >= s->up_at_ms[p]) {
            s->up_at_ms[p] = 0;
            s->alive[p] = 1;
            if (p >= LG_PROC_SENSOR_BASE && p < LG_PROC_PLANT) {
                int idx = p - LG_PROC_SENSOR_BASE;

                lg_sensor_model_init(&sm[idx / LG_SENSOR_ID_COUNT][idx % LG_SENSOR_ID_COUNT],
                                     SEED + 77u * (uint32_t)p + t, t);
            }
        }
        if (s->alive[p] && (t % LG_HEARTBEAT_PERIOD_MS) == 0) {
            (void)lg_sup_on_heartbeat(&s->sup, (uint8_t)p, s->pid[p], t);
        }
    }
    n = lg_sup_step(&s->sup, t, act, LG_SUP_MAX_ACTIONS);
    for (i = 0; i < n; i++) {
        if (act[i].kind == LG_SUP_ACT_SPAWN) {
            s->pid[act[i].proc] = ++s->next_pid;
            (void)lg_sup_on_spawn(&s->sup, act[i].proc, s->pid[act[i].proc], t);
            s->up_at_ms[act[i].proc] = t + g_kill_delay_ms;
        } else if (act[i].kind == LG_SUP_ACT_STATUS && sup_reportable(s, act[i].proc)) {
            (void)lg_ctl_on_proc_status(ctl, act[i].proc, lg_sup_reported_state(&s->sup, act[i].proc),
                                        lg_sup_restarts_in_window(&s->sup, act[i].proc, t), t);
        }
    }
    if (t - s->last_refresh_ms >= LG_SUP_STATUS_REFRESH_MS) {
        s->last_refresh_ms = t;
        for (p = 0; p < LG_PROC_COUNT; p++) {
            if (sup_reportable(s, p)) {
                (void)lg_ctl_on_proc_status(ctl, (uint8_t)p, lg_sup_reported_state(&s->sup, (uint8_t)p),
                                            lg_sup_restarts_in_window(&s->sup, (uint8_t)p, t), t);
            }
        }
    }
}

static void run(const lg_crossing_cfg_t *cc, const lg_scenario_t *sc, const lg_ctl_cfg_t *cfg,
                uint32_t end_ms, result_t *r) {
    lg_plant_t plant;
    lg_sensor_model_t sm[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    lg_gate_model_t gate;
    lg_gate_cfg_t gcfg = lg_gate_cfg_default();
    supsim_t sim;
    uint32_t t;
    int d;
    int i;

    memset(r, 0, sizeof *r);
    memset(&sim, 0, sizeof sim);
    if (g_kill_proc >= 0) {
        supsim_init(&sim);
    }
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
        if (g_kill_proc >= 0) {
            supsim_tick(&sim, &r->ctl, sm, t);
        }
        for (d = 0; d < LG_DIR_COUNT; d++) {
            for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                lg_truth_t tr = lg_plant_truth(&plant, t, (uint8_t)d, (uint8_t)i);
                lg_sensor_out_t so;

                if (g_kill_proc >= 0 && !sim.alive[LG_PROC_SENSOR_BASE + d * LG_SENSOR_ID_COUNT + i]) {
                    continue; /* the process is gone: no sample */
                }
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
        if (r->boot_release_ms != 0 && r->ctl.mode.level > LG_LEVEL_NORMAL) {
            r->above0_after_boot++;
        }
        if (r->boot_release_ms != 0 && r->ctl.mode.level == LG_LEVEL_DEGRADED) {
            r->level2_after_boot++;
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

/*
 * Every sensor process is killed at any moment of the day (every 3989 ms), restarted by the real
 * supervisor logic and back after 300 ms or after 2 s: no wrong-side event, ever. Killing an island
 * sensor locks level 4 (no island, no timetable, no guard in this replay), which keeps the gate DOWN:
 * the road stays closed, never open. The wrong-side assertion is the same one as everywhere else.
 */
static void test_sensor_kill_sweep(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    static const uint32_t delays[2] = {300, 2000};
    result_t r;
    uint32_t runs = 0;
    uint32_t bad = 0;
    uint32_t first_bad = 0;
    uint32_t reacted_runs = 0;    /* runs in which the controller left level 0 after the boot release */
    uint32_t level2_ticks_max = 0; /* longest time at level 2 after killing an approach sensor, fast restart */
    uint32_t gave_up_runs = 0;     /* slow restart (2 s > start grace 1.5 s): the supervisor gives up, DEAD */
    uint32_t k;
    int p;

    for (k = 0; k < 2; k++) {
        for (p = LG_PROC_SENSOR_BASE; p < LG_PROC_PLANT; p++) {
            uint32_t at;

            for (at = 5000; at <= 285000; at += 3989) {
                g_kill_proc = p;
                g_kill_ms = at - (at % LG_TICK_MS) + LG_TICK_MS; /* on a tick */
                g_kill_delay_ms = delays[k];
                run(&g_cc, &g_sc, &cfg, 300000, &r);
                runs++;
                if (!lg_metrics_wrong_side_ok(&r.metrics) || r.metrics.wrong_side_ticks != 0) {
                    if (bad == 0) {
                        first_bad = at;
                    }
                    bad++;
                }
                if (r.above0_after_boot > 0) {
                    reacted_runs++;
                }
                /* approach sensors only (a, b): the redundant one keeps the direction at level 2 */
                if ((p - LG_PROC_SENSOR_BASE) % LG_SENSOR_ID_COUNT != LG_SENSOR_ISLAND) {
                    if (k == 0 && r.level2_after_boot > level2_ticks_max) {
                        level2_ticks_max = r.level2_after_boot;
                    }
                    /* the new process never beats within the grace period: 3 restarts, then DEAD for good */
                    if (k == 1 && r.level2_after_boot * LG_TICK_MS > 100000) {
                        gave_up_runs++;
                    }
                }
            }
        }
    }
    g_kill_proc = -1;
    printf("sensor kill sweep: %u runs (6 sensors, 2 restart times), %u with wrong-side events (first at %u ms), "
           "%u runs left level 0 after the boot release (longest %u ms)\n",
           (unsigned)runs, (unsigned)bad, (unsigned)first_bad, (unsigned)reacted_runs,
           (unsigned)(level2_ticks_max * LG_TICK_MS));
    CHECK(runs > 700);
    CHECK(bad == 0);
    /* The kills really reach the controller: a restarting approach sensor means level 2 for T_stable. */
    CHECK(reacted_runs > runs / 2);
    CHECK(level2_ticks_max * LG_TICK_MS >= LG_T_STABLE_MS);
    printf("longest level 2 after a fast restart of an approach sensor: %u ms; slow restarts that ended DEAD "
           "(level 2 for the rest of the day): %u runs\n",
           (unsigned)(level2_ticks_max * LG_TICK_MS), (unsigned)gave_up_runs);
    CHECK(level2_ticks_max * LG_TICK_MS < 2 * LG_T_STABLE_MS + 4000); /* a fast restart does not stay at level 2 */
    CHECK(gave_up_runs > 0); /* a process that cannot start in time is given up, not restarted for ever */
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
    test_sensor_kill_sweep();
    LG_TEST_DONE("replay_normal_day");
}
