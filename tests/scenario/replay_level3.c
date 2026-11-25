/*
 * Host scenario replay for level 3 (Step 12, README 8.4): plant model + six sensor models + gate model +
 * controller logic + the lg-schedule timetable logic (core/timetable.c, replies every LG_SCHED_PUSH_MS, epoch
 * = controller time 0), stepped every 10 ms with fixed seeds. Both West approach sensors fail (DROPOUT 100)
 * at 20 s. Host only, not verified on QNX. The wrong-side assertion is present in every run.
 *
 *  1. level3_day: trains from the West are protected by timetable windows (on time, 60 s late, 30 s early);
 *     closure per hour up to 1000 s is far below "gate down permanently" (3 600 000 ms per hour); the
 *     cancelled entry 7 keeps the gate down past its window until a guard confirms.
 *  2. level3_station: a West train 90 s earlier than planned is protected only by the station message;
 *     a station message that claims a later arrival does not delay a window. Negative control: without the
 *     station messages the early train reaches the island before the gate is down (the metric must see it).
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
#include "lg_timetable.h"
#include "../unit/lg_test.h"

#define SEED 20261125u
#define FAULT_MS 20000u
#define PERMANENT_MS_PER_HOUR 3600000ull
#define CLOSURE_LIMIT_PCT 40 /* "far below permanent closure": at most 40 % (plan, Step 12) */

static lg_json_doc_t g_doc;
static char g_text[16384];

typedef struct {
    uint32_t at_ms;
    uint8_t dir;
    uint32_t eta_rel_ms;
} station_msg_t;

typedef struct {
    lg_metrics_t metrics;      /* the whole run */
    lg_metrics_t metrics_part; /* up to part_end_ms (closure measurement) */
    lg_ctl_t ctl;
    lg_tt_t tt;
    uint32_t max_level;
    uint32_t level3_ms; /* first time in level 3 */
    uint32_t level4_after_boot;
    uint32_t overdue_events;
    uint32_t level3_notify; /* station + guard + road sign on entry */
    uint32_t replies;
    uint32_t rejected;
} result_t;

static int load_files(const char *scenario, const char *timetable, lg_crossing_cfg_t *cc, lg_scenario_t *sc,
                      lg_tt_t *tt) {
    char err[LG_ERR_LEN];
    size_t n;

    if (lg_read_file("config/crossing.json", g_text, sizeof g_text, &n) != 0 ||
        lg_json_parse(&g_doc, g_text, n) != 0 || lg_crossing_from_json(&g_doc, cc, err, sizeof err) != 0) {
        return -1;
    }
    if (lg_read_file(scenario, g_text, sizeof g_text, &n) != 0 || lg_json_parse(&g_doc, g_text, n) != 0 ||
        lg_scenario_from_json(&g_doc, cc, sc, err, sizeof err) != 0) {
        return -1;
    }
    if (lg_read_file(timetable, g_text, sizeof g_text, &n) != 0 || lg_json_parse(&g_doc, g_text, n) != 0 ||
        lg_tt_from_json(&g_doc, tt, err, sizeof err) != 0) {
        printf("timetable %s: %s\n", timetable, err);
        return -1;
    }
    return 0;
}

static void run(const lg_crossing_cfg_t *cc, const lg_scenario_t *sc, const lg_tt_t *tt0, const station_msg_t *st,
                uint32_t n_st, uint32_t guard_at_ms, uint32_t part_end_ms, uint32_t end_ms, result_t *r) {
    static lg_plant_t plant;
    lg_sensor_model_t sm[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    lg_gate_model_t gate;
    lg_gate_cfg_t gcfg = lg_gate_cfg_default();
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    uint32_t t;
    uint32_t seen = 0;
    int d;
    int i;

    memset(r, 0, sizeof *r);
    r->tt = *tt0;
    lg_plant_init(&plant, cc, sc);
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            lg_sensor_model_init(&sm[d][i], SEED + (uint32_t)(d * 3 + i), 0);
        }
    }
    lg_gate_model_init(&gate, &gcfg, 0);
    lg_ctl_init(&r->ctl, &cfg, 0);
    lg_metrics_init(&r->metrics, 0);
    lg_metrics_init(&r->metrics_part, 0);

    for (t = 10; t <= end_ms; t += LG_TICK_MS) {
        lg_ctl_out_t out;
        lg_truth_t island_truth;
        uint32_t k;

        if (t == FAULT_MS) {
            (void)lg_sensor_model_set_fault(&sm[LG_DIR_WEST][LG_SENSOR_A], LG_SFAULT_DROPOUT, 100, t);
            (void)lg_sensor_model_set_fault(&sm[LG_DIR_WEST][LG_SENSOR_B], LG_SFAULT_DROPOUT, 100, t);
        }
        for (k = 0; k < n_st; k++) {
            if (st[k].at_ms == t) {
                (void)lg_tt_station(&r->tt, st[k].dir, t + st[k].eta_rel_ms, t);
            }
        }
        if (t % LG_SCHED_PUSH_MS == 0) {
            for (d = 0; d < LG_DIR_COUNT; d++) {
                lg_sched_reply_t rep;

                memset(&rep, 0, sizeof rep);
                lg_tt_reply(&r->tt, (uint8_t)d, t, 0, &rep);
                r->replies++;
                if (lg_ctl_on_sched_reply(&r->ctl, &rep, t) != 0) {
                    r->rejected++;
                }
            }
        }
        if (guard_at_ms != 0 && t == guard_at_ms) {
            (void)lg_ctl_on_guard_confirm(&r->ctl, LG_DIR_WEST);
        }
        for (d = 0; d < LG_DIR_COUNT; d++) {
            for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                lg_truth_t tr = lg_plant_truth(&plant, t, (uint8_t)d, (uint8_t)i);
                lg_sensor_out_t so;

                if (lg_sensor_model_step(&sm[d][i], t, tr.occupied, tr.axles, &so)) {
                    (void)lg_ctl_on_sample(&r->ctl, (uint8_t)d, (uint8_t)i, so.occupied, so.quality, so.axle_count,
                                           t, t);
                }
            }
        }
        out = lg_ctl_step(&r->ctl, t);
        (void)lg_gate_model_command(&gate, out.gate_target == LG_GATE_TARGET_UP ? LG_GATE_CMD_RAISE : LG_GATE_CMD_LOWER,
                                    t);
        (void)lg_gate_model_command(&gate, out.lights ? LG_GATE_CMD_LIGHTS_ON : LG_GATE_CMD_LIGHTS_OFF, t);
        (void)lg_gate_model_command(&gate, out.bell ? LG_GATE_CMD_BELL_ON : LG_GATE_CMD_BELL_OFF, t);
        lg_gate_model_step(&gate, t);
        (void)lg_ctl_on_gate_reply(&r->ctl, gate.state, t);

        island_truth = lg_plant_truth(&plant, t, LG_DIR_WEST, LG_SENSOR_ISLAND);
        lg_metrics_tick(&r->metrics, t, gate.state, gate.lights, island_truth.island_any, 0);
        if (t <= part_end_ms) {
            lg_metrics_tick(&r->metrics_part, t, gate.state, gate.lights, island_truth.island_any, 0);
        }
        if (r->ctl.mode.level > r->max_level) {
            r->max_level = r->ctl.mode.level;
        }
        if (r->ctl.mode.level == LG_LEVEL_UNCERTAIN && r->level3_ms == 0) {
            r->level3_ms = t;
        }
        if (t > 10000 && r->ctl.mode.level == LG_LEVEL_FAILSAFE) {
            r->level4_after_boot++;
        }
        /* Events of this step. */
        {
            lg_ctl_event_t ev[LG_CTL_EVENT_RING];
            uint32_t n = lg_ctl_events_since(&r->ctl, seen + 1, ev, LG_CTL_EVENT_RING);

            for (k = 0; k < n; k++) {
                if (ev[k].kind == LG_EVT_NOTIFY && ev[k].code == LG_CTL_DIAG_TRAIN_OVERDUE) {
                    r->overdue_events++;
                }
                if (ev[k].kind == LG_EVT_NOTIFY && ev[k].code == LG_CTL_DIAG_DETECTION_LOST) {
                    r->level3_notify++;
                }
                seen = ev[k].seq;
            }
        }
    }
}

static void test_level3_day(void) {
    static lg_crossing_cfg_t cc;
    static lg_scenario_t sc;
    static lg_tt_t tt;
    static result_t r;
    uint64_t per_hour;

    if (load_files("scenarios/level3_day.json", "scenarios/level3_day.timetable.json", &cc, &sc, &tt) != 0) {
        CHECK(0 && "could not load level3_day (run from the repo root)");
        return;
    }
    /* Guard confirms the cancelled entry 7 (window 1040 .. 1280 s) after its end. */
    run(&cc, &sc, &tt, NULL, 0, 1290000, 1000000, 1300000, &r);
    per_hour = lg_metrics_closure_ms_per_hour(&r.metrics_part);
    printf("level3_day seed=%u level3_ms=%u trains=%u activations=%u cycles=%u win_west opened=%u island=%u "
           "guard=%u overdue=%u wrong_side=%u closure_ms_per_hour(0..1000 s)=%llu permanent=%llu (%llu %%)\n",
           SEED, (unsigned)r.level3_ms, (unsigned)r.metrics.trains, (unsigned)r.ctl.activations,
           (unsigned)r.ctl.cycles, (unsigned)r.ctl.win[LG_DIR_WEST].opened,
           (unsigned)r.ctl.win[LG_DIR_WEST].confirmed_island, (unsigned)r.ctl.win[LG_DIR_WEST].confirmed_guard,
           (unsigned)r.overdue_events, (unsigned)r.metrics.wrong_side_events, (unsigned long long)per_hour,
           PERMANENT_MS_PER_HOUR, (unsigned long long)(per_hour * 100ull / PERMANENT_MS_PER_HOUR));

    CHECK(lg_metrics_wrong_side_ok(&r.metrics)); /* hard rule: never relaxed */
    CHECK(r.metrics.wrong_side_ticks == 0);
    CHECK(r.metrics.trains == 6 && r.metrics.trains_without_warning == 0);
    CHECK(r.level3_ms > FAULT_MS && r.level3_ms < FAULT_MS + 1000);
    CHECK(r.max_level == LG_LEVEL_FAILSAFE && r.level4_after_boot == 0); /* level 4 only at boot */
    CHECK(r.level3_notify == 3);
    CHECK(r.rejected == 0);
    CHECK(r.ctl.win[LG_DIR_WEST].opened == 4); /* trains 1, 3, 5 and the cancelled entry 7 */
    CHECK(r.ctl.win[LG_DIR_WEST].confirmed_island == 3);
    CHECK(r.ctl.win[LG_DIR_WEST].confirmed_guard == 1 && r.overdue_events == 1);
    CHECK(r.ctl.mode.level == LG_LEVEL_UNCERTAIN && !lg_win_open(&r.ctl.win[LG_DIR_WEST]));
    CHECK(per_hour * 100ull <= PERMANENT_MS_PER_HOUR * CLOSURE_LIMIT_PCT);
}

static void test_level3_station(void) {
    static lg_crossing_cfg_t cc;
    static lg_scenario_t sc;
    static lg_tt_t tt;
    static result_t r;
    const station_msg_t st[] = {{150000, LG_DIR_WEST, 60000}, {330000, LG_DIR_WEST, 190000}};

    if (load_files("scenarios/level3_station.json", "scenarios/level3_station.timetable.json", &cc, &sc, &tt) != 0) {
        CHECK(0 && "could not load level3_station (run from the repo root)");
        return;
    }
    run(&cc, &sc, &tt, st, 2, 0, 450000, 450000, &r);
    printf("level3_station seed=%u station rx=%u moved=%u ignored=%u win_west opened=%u island=%u wrong_side=%u\n",
           SEED, (unsigned)r.tt.station_rx, (unsigned)r.tt.station_moved, (unsigned)r.tt.station_ignored,
           (unsigned)r.ctl.win[LG_DIR_WEST].opened, (unsigned)r.ctl.win[LG_DIR_WEST].confirmed_island,
           (unsigned)r.metrics.wrong_side_events);
    CHECK(lg_metrics_wrong_side_ok(&r.metrics)); /* hard rule: never relaxed */
    CHECK(r.metrics.wrong_side_ticks == 0);
    CHECK(r.metrics.trains == 3 && r.metrics.trains_without_warning == 0);
    CHECK(r.tt.station_moved == 1 && r.tt.station_ignored == 1); /* early train moved, "late" claim ignored */
    CHECK(r.ctl.win[LG_DIR_WEST].confirmed_island == 2 && !lg_win_open(&r.ctl.win[LG_DIR_WEST]));
    CHECK(r.level4_after_boot == 0);

    /* Negative control: the same day without the station messages. The early train reaches the island
     * before its timetable window opens; the metric must catch it. If this ever passes, the replay does not
     * exercise what it claims. */
    run(&cc, &sc, &tt, NULL, 0, 0, 450000, 450000, &r);
    printf("level3_station without station messages (negative control): wrong_side=%u\n",
           (unsigned)r.metrics.wrong_side_events);
    CHECK(r.metrics.wrong_side_events > 0);
}

int main(void) {
    test_level3_day();
    test_level3_station();
    LG_TEST_DONE("replay_level3");
}
