/*
 * Extra tests for the decision logic of lg-controller (core/controller_logic.c, lg_lower_delay_ms).
 * Host only. Complements test_controller_logic.c (basic cases) and replay_normal_day.c (full replay).
 *
 * Time in the unit tests: h_tick() advances 10 ms and feeds samples with sample_ms = rx_ms = now. A
 * tiny scripted gate (4000 ms travel, replies one tick behind the command) stands in for lg-gate, and a
 * test can override what the controller hears from the gate. The wrong-side assertion is never relaxed:
 * test 8 uses the real gate model and plant and requires wrong_side_events == 0 and wrong_side_ticks == 0.
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
#include "lg_time.h"
#include "lg_voting.h"
#include "lg_test.h"

#define GATE_SILENT 255 /* gate_override value: no reply at all */
#define SIM_TRAVEL_MS 4000u

typedef struct {
    lg_ctl_t c;
    uint32_t base; /* time origin (a test may start close to the 32-bit wrap) */
    uint32_t o;    /* offset from base in ms, 10 ms steps */
    uint8_t occ[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    uint8_t qual[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    uint8_t mute[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];  /* no sample this tick */
    uint8_t bad[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];   /* sample with occupied = 2 (rejected) */
    uint32_t stale[LG_DIR_COUNT][LG_SENSOR_ID_COUNT]; /* sample_ms is this many ms older than rx */
    int gate_override; /* -1: tell the truth of the scripted gate; else the state to report */
    uint8_t g;         /* scripted gate state */
    uint32_t gs;       /* when it started moving */
    lg_ctl_out_t out;
} H;

static void h_init(H *h, const lg_ctl_cfg_t *cfg, uint32_t base) {
    int d;
    int i;

    memset(h, 0, sizeof *h);
    h->base = base;
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            h->qual[d][i] = 97;
        }
    }
    h->gate_override = -1;
    h->g = LG_GATE_DOWN; /* the gate boots DOWN */
    lg_ctl_init(&h->c, cfg, base);
}

static void sim_gate(H *h, uint32_t t) {
    if (h->out.gate_target == LG_GATE_TARGET_DOWN) {
        if (h->g == LG_GATE_UP || h->g == LG_GATE_RAISING) {
            h->g = LG_GATE_LOWERING;
            h->gs = t;
        }
    } else if (h->g == LG_GATE_DOWN || h->g == LG_GATE_LOWERING) {
        h->g = LG_GATE_RAISING;
        h->gs = t;
    }
    if ((h->g == LG_GATE_LOWERING || h->g == LG_GATE_RAISING) && lg_elapsed_ge(t, h->gs, SIM_TRAVEL_MS)) {
        h->g = h->g == LG_GATE_LOWERING ? (uint8_t)LG_GATE_DOWN : (uint8_t)LG_GATE_UP;
    }
}

static lg_ctl_out_t h_tick(H *h) {
    uint32_t t;
    int d;
    int i;

    h->o += 10;
    t = h->base + h->o; /* wraps on purpose */
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            if (h->mute[d][i]) {
                continue;
            }
            if (h->bad[d][i]) {
                (void)lg_ctl_on_sample(&h->c, (uint8_t)d, (uint8_t)i, 2, h->qual[d][i], 0, t, t);
                continue;
            }
            (void)lg_ctl_on_sample(&h->c, (uint8_t)d, (uint8_t)i, h->occ[d][i], h->qual[d][i], 0,
                                   t - h->stale[d][i], t);
        }
    }
    if (h->gate_override != GATE_SILENT) {
        (void)lg_ctl_on_gate_reply(&h->c, h->gate_override >= 0 ? (uint8_t)h->gate_override : h->g, t);
    }
    h->out = lg_ctl_step(&h->c, t);
    sim_gate(h, t);
    return h->out;
}

static void run_to(H *h, uint32_t o_end) {
    while (h->o < o_end) {
        (void)h_tick(h);
    }
}

/* Boot release at o = 5010, the gate is UP from o = 9010. Returns at o = 10000, level 0, gate UP. */
static void boot_to_10000(H *h) {
    run_to(h, 10000);
    CHECK(h->out.level == LG_LEVEL_NORMAL);
    CHECK(h->g == LG_GATE_UP);
    CHECK(h->out.gate_target == LG_GATE_TARGET_UP);
}

/* ---- 1. Same-direction following train and approach flicker ---- */

static void follow_case(uint32_t flicker_o, uint32_t len_ticks, uint32_t expect_clear_o) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    H h;
    uint32_t cleared_o = 0;

    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.occ[LG_DIR_WEST][LG_SENSOR_A] = 1; /* the first train: one tick is enough to activate */
    (void)h_tick(&h);
    h.occ[LG_DIR_WEST][LG_SENSOR_A] = 0;
    CHECK(h.o == 10010 && h.c.active[LG_DIR_WEST] == 1);

    while (h.o < 30000 && cleared_o == 0) {
        uint32_t next = h.o + 10;
        lg_ctl_out_t o;

        h.occ[LG_DIR_WEST][LG_SENSOR_A] =
            (uint8_t)(len_ticks > 0 && next >= flicker_o && next < flicker_o + len_ticks * 10u);
        o = h_tick(&h);
        if (o.cleared_mask != 0) {
            cleared_o = h.o;
            CHECK(o.cleared_mask == (1u << LG_DIR_WEST));
            break;
        }
        /* The direction is active and not cleared: from the end of the pre-warning the target is DOWN. */
        CHECK(h.c.active[LG_DIR_WEST] == 1);
        if (h.o >= 15010) {
            CHECK(o.gate_target == LG_GATE_TARGET_DOWN);
        }
        if (len_ticks > 0 && h.o >= flicker_o && h.o < expect_clear_o) {
            CHECK((h.c.last_unmet[LG_DIR_WEST] & LG_CTL_UNMET_SAME_DIR) != 0);
        }
    }
    CHECK(cleared_o == expect_clear_o);
    CHECK(h.out.gate_target == LG_GATE_TARGET_UP); /* released on the clearing tick, not before */
    CHECK(h.c.activations == 1); /* the following train did not count as a new activation */
}

static void test_same_direction_following_train(void) {
    follow_case(0, 0, 22020);       /* control: nothing follows, clears when the 3 s down time ended */
    follow_case(21000, 1, 23000);   /* one tick of A occupied during the wait */
    follow_case(22010, 1, 24010);   /* flicker one tick before the clearing tick */
    follow_case(22020, 1, 24020);   /* flicker exactly on the clearing tick */
    follow_case(21000, 300, 25990); /* a following train occupying A for 3 s: last tick at 23990 */
}

/* ---- 2. Opposite directions overlap ---- */

static void test_opposite_directions_overlap(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    H h;
    uint32_t west_cleared = 0;
    uint32_t east_cleared = 0;

    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.occ[LG_DIR_WEST][LG_SENSOR_A] = 1;
    (void)h_tick(&h);
    h.occ[LG_DIR_WEST][LG_SENSOR_A] = 0;

    while (h.o < 30000) {
        uint32_t next = h.o + 10;
        lg_ctl_out_t o;

        h.occ[LG_DIR_EAST][LG_SENSOR_A] = (uint8_t)(next >= 20000 && next < 21000); /* east arrives later */
        h.occ[LG_DIR_EAST][LG_SENSOR_B] = h.occ[LG_DIR_EAST][LG_SENSOR_A]; /* a healthy pair (Step 9) */
        o = h_tick(&h);
        if (o.cleared_mask & (1u << LG_DIR_WEST)) {
            west_cleared = h.o;
        }
        if (o.cleared_mask & (1u << LG_DIR_EAST)) {
            east_cleared = h.o;
        }
        if (h.c.active[LG_DIR_WEST] || h.c.active[LG_DIR_EAST]) {
            /* Invariant: no direction cleared yet means the gate is not released once it is due. */
            if (h.o >= 15010) {
                CHECK(o.gate_target == LG_GATE_TARGET_DOWN);
            }
        }
        if (west_cleared != 0 && east_cleared == 0) {
            CHECK(h.c.active[LG_DIR_EAST] == 1);
            CHECK(o.gate_target == LG_GATE_TARGET_DOWN); /* west is clear, east still holds the gate */
            CHECK(o.lights && o.bell);
        }
    }
    CHECK(west_cleared == 22020);
    CHECK(east_cleared == 22990); /* last east approach at 20990 + T_clear */
    CHECK(west_cleared < east_cleared);
    CHECK(h.out.gate_target == LG_GATE_TARGET_UP);
    CHECK(h.c.activations == 2 && h.c.cycles == 1);
}

/* ---- 3. Lowering delay ---- */

static void test_lower_delay_function(void) {
    static const uint32_t pw[] = {0, 1, 4999, 5000, 5001, 8000, 0xFFFFFFFFu};
    static const uint32_t mg[] = {0, 1, 5000, 5001, 0xFFFFFFFFu};
    size_t a;
    size_t b;
    int lv;

    for (a = 0; a < sizeof pw / sizeof pw[0]; a++) {
        for (b = 0; b < sizeof mg / sizeof mg[0]; b++) {
            lg_vote_cfg_t cfg;
            uint32_t l0;

            cfg.prewarning_ms = pw[a];
            cfg.extra_margin_ms = mg[b];
            l0 = lg_lower_delay_ms(&cfg, LG_LEVEL_NORMAL);
            CHECK(l0 == pw[a]);
            CHECK(lg_lower_delay_ms(&cfg, LG_LEVEL_EARLY_WARNING) == l0);
            for (lv = LG_LEVEL_DEGRADED; lv <= LG_LEVEL_FAILSAFE + 3; lv++) {
                uint32_t d = lg_lower_delay_ms(&cfg, (uint8_t)lv);

                CHECK(d <= l0); /* never longer than level 0 */
                CHECK(d == (pw[a] > mg[b] ? pw[a] - mg[b] : 0));
            }
        }
    }
}

/* Activation tick with the gate reporting `gate` (level 0). Returns the output of the activation tick. */
static lg_ctl_out_t activate_with_gate(uint8_t gate) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    H h;

    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.gate_override = gate;
    h.occ[LG_DIR_WEST][LG_SENSOR_A] = 1;
    return h_tick(&h);
}

static void test_gate_not_up_at_activation_lowers_at_once(void) {
    lg_ctl_out_t o;

    o = activate_with_gate(LG_GATE_RAISING);
    CHECK(o.activated_mask == 1u << LG_DIR_WEST && o.level == LG_LEVEL_NORMAL);
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights && o.bell);
    o = activate_with_gate(LG_GATE_LOWERING);
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights && o.bell);
    o = activate_with_gate(LG_GATE_DOWN);
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights && o.bell);
    o = activate_with_gate(LG_GATE_FAULT);
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights && o.bell && o.level == LG_LEVEL_FAILSAFE);
    /* Control: a gate that is UP waits for the pre-warning delay. */
    o = activate_with_gate(LG_GATE_UP);
    CHECK(o.activated_mask == 1u << LG_DIR_WEST);
    CHECK(o.gate_target == LG_GATE_TARGET_UP && o.lights && o.bell);
}

/* First o (after activation at 10010) where the gate target is DOWN, or 0 when never within 20 s. */
static uint32_t first_down_o(const lg_ctl_cfg_t *cfg, int make_level2) {
    H h;
    uint32_t t0;

    h_init(&h, cfg, 0);
    boot_to_10000(&h);
    if (make_level2) {
        h.mute[LG_DIR_WEST][LG_SENSOR_B] = 1; /* west/b silent for 250 ms: level 2, stays 2 for T_stable */
        run_to(&h, 10260);
        h.mute[LG_DIR_WEST][LG_SENSOR_B] = 0;
        run_to(&h, 11000);
        CHECK(h.out.level == LG_LEVEL_DEGRADED);
    }
    h.occ[LG_DIR_WEST][LG_SENSOR_A] = 1;
    (void)h_tick(&h);
    t0 = h.o;
    h.occ[LG_DIR_WEST][LG_SENSOR_A] = 0;
    CHECK(h.out.activated_mask == 1u << LG_DIR_WEST);
    if (h.out.gate_target == LG_GATE_TARGET_DOWN) {
        return 0 + (h.o - t0);
    }
    while (h.o < t0 + 20000) {
        if (h_tick(&h).gate_target == LG_GATE_TARGET_DOWN) {
            return h.o - t0;
        }
    }
    return 0xFFFFFFFFu;
}

static void test_level2_delay_is_shorter(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();

    /* Defaults: prewarning 5000, margin 5000: level 2 lowers at once, level 0 after 5 s. */
    CHECK(first_down_o(&cfg, 0) == 5000);
    CHECK(first_down_o(&cfg, 1) == 0);
    /* Non-default: 8000 and 3000: 8 s at level 0, 5 s at level 2; exact boundary, one tick earlier is UP. */
    cfg.vote.prewarning_ms = 8000;
    cfg.vote.extra_margin_ms = 3000;
    CHECK(first_down_o(&cfg, 0) == 8000);
    CHECK(first_down_o(&cfg, 1) == 5000);
    /* Margin larger than the pre-warning: delay floors at 0, never wraps to a huge value. */
    cfg.vote.prewarning_ms = 1000;
    cfg.vote.extra_margin_ms = 4000;
    CHECK(first_down_o(&cfg, 0) == 1000);
    CHECK(first_down_o(&cfg, 1) == 0);
}

/* ---- 4. Island sensor ---- */

static void test_island_occupied_holds_gate_down(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    H h;
    lg_ctl_out_t o;
    int k;

    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.occ[LG_DIR_EAST][LG_SENSOR_ISLAND] = 1; /* any island sensor counts, no direction active */
    for (k = 0; k < 50; k++) {
        o = h_tick(&h);
        CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights && o.bell);
        CHECK(o.level == LG_LEVEL_NORMAL && o.activated_mask == 0);
        CHECK(h.c.active[0] == 0 && h.c.active[1] == 0);
    }
    h.occ[LG_DIR_EAST][LG_SENSOR_ISLAND] = 0;
    h.occ[LG_DIR_WEST][LG_SENSOR_ISLAND] = 1;
    o = h_tick(&h);
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN);
    h.occ[LG_DIR_WEST][LG_SENSOR_ISLAND] = 0;
    o = h_tick(&h);
    CHECK(o.gate_target == LG_GATE_TARGET_UP); /* the island alone holds no longer than it reports */
}

static void test_unusable_island_fails_the_level(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    H h;
    lg_ctl_out_t o;

    /*
     * Quality 0 with occupied = 1: the island is unusable at once (ignored for that tick, like a stale
     * sample); blind for sensor_silence_ms it is FAILED, no timetable before Step 12 -> level 4.
     * (Step 9, decided with the user: before, one blind sample was level 4 at once.)
     */
    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.occ[LG_DIR_WEST][LG_SENSOR_ISLAND] = 1;
    h.qual[LG_DIR_WEST][LG_SENSOR_ISLAND] = 0;
    o = h_tick(&h);
    CHECK(o.level == LG_LEVEL_NORMAL && o.gate_target == LG_GATE_TARGET_UP);
    while (h.o < 10010 + cfg.sensor_silence_ms - 10) {
        o = h_tick(&h);
        CHECK(o.level == LG_LEVEL_NORMAL);
    }
    o = h_tick(&h);
    CHECK(o.level == LG_LEVEL_FAILSAFE && o.level_changed && o.level_reason == LG_REASON_DETECT_LOST_NO_INFO);
    o = h_tick(&h);
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights && o.bell);

    /* One stale occupied sample is ignored for that tick and changes neither level nor target. */
    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.occ[LG_DIR_WEST][LG_SENSOR_ISLAND] = 1;
    h.stale[LG_DIR_WEST][LG_SENSOR_ISLAND] = LG_SAMPLE_MAX_AGE_MS + 1u;
    o = h_tick(&h);
    CHECK(o.level == LG_LEVEL_NORMAL && o.gate_target == LG_GATE_TARGET_UP);
    /* Freshness boundary: age 20 ms is usable, so the same sample now holds the gate down. */
    h.stale[LG_DIR_WEST][LG_SENSOR_ISLAND] = LG_SAMPLE_MAX_AGE_MS;
    o = h_tick(&h);
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN);
}

static void test_stale_boundary_for_activation(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    H h;
    lg_ctl_out_t o;

    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.occ[LG_DIR_EAST][LG_SENSOR_B] = 1;
    h.stale[LG_DIR_EAST][LG_SENSOR_B] = LG_SAMPLE_MAX_AGE_MS + 1u;
    o = h_tick(&h);
    CHECK(o.activated_mask == 0);
    h.stale[LG_DIR_EAST][LG_SENSOR_B] = LG_SAMPLE_MAX_AGE_MS;
    o = h_tick(&h);
    CHECK(o.activated_mask == 1u << LG_DIR_EAST);
    /* A sample stamped in the future counts as fresh, never as a huge age. */
    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.occ[LG_DIR_WEST][LG_SENSOR_A] = 1;
    h.stale[LG_DIR_WEST][LG_SENSOR_A] = (uint32_t)-5; /* sample_ms = now + 5 */
    o = h_tick(&h);
    CHECK(o.activated_mask == 1u << LG_DIR_WEST);
}

/* ---- 5. Level 3 and 4 ---- */

/*
 * Step 12 replaced the interim rule "level 3 holds the gate down" (Steps 7 to 11, when level 3 could not be
 * reached) by timetable windows: level 3 itself does not hold the gate; without an OPEN window and with no
 * train the gate stays UP (test_controller_level3 covers the windows).
 */
static void test_level3_holds_gate_down(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    H h;

    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.c.mode.level = LG_LEVEL_UNCERTAIN; /* set directly: everything healthy, gate reports UP */
    while (h.o < 10000 + LG_T_STABLE_MS - 1000) {
        lg_ctl_out_t o = h_tick(&h);

        CHECK(o.level == LG_LEVEL_UNCERTAIN);
        CHECK(o.gate_target == LG_GATE_TARGET_UP && !o.lights && !o.bell);
        CHECK(h.c.active[0] == 0 && h.c.active[1] == 0);
    }
}

static void unhealthy_west_approach(H *h, int on) {
    h->mute[LG_DIR_WEST][LG_SENSOR_A] = (uint8_t)on;
    h->mute[LG_DIR_WEST][LG_SENSOR_B] = (uint8_t)on;
}

static void test_level4_needs_guard_ack(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    H h;
    lg_ctl_out_t o;
    uint32_t end;

    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    /* Both west approach sensors silent: no detection and no timetable -> level 4 (README 3.1). */
    unhealthy_west_approach(&h, 1);
    run_to(&h, 10400);
    CHECK(h.out.level == LG_LEVEL_FAILSAFE && h.out.gate_target == LG_GATE_TARGET_DOWN);
    CHECK(h.out.lights && h.out.bell);

    /* An ack given while the sensors are still unhealthy is void, also for the time after they heal. */
    lg_ctl_on_guard_ack(&h.c);
    o = h_tick(&h);
    CHECK(o.level == LG_LEVEL_FAILSAFE);
    run_to(&h, 10400 + 2000);
    lg_ctl_on_guard_ack(&h.c);
    o = h_tick(&h); /* consumed while still unhealthy */
    CHECK(o.level == LG_LEVEL_FAILSAFE);
    unhealthy_west_approach(&h, 0); /* healed from now on, no new ack */
    end = h.o + LG_T_STABLE_MS + 30000;
    while (h.o < end) {
        o = h_tick(&h);
        CHECK(o.level == LG_LEVEL_FAILSAFE);
        CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights && o.bell);
    }

    /* An ack while healthy is accepted, and the level falls after T_stable. */
    lg_ctl_on_guard_ack(&h.c);
    end = h.o + LG_T_STABLE_MS - 1000;
    while (h.o < end) {
        o = h_tick(&h);
        CHECK(o.level == LG_LEVEL_FAILSAFE && o.gate_target == LG_GATE_TARGET_DOWN);
    }
    end = h.o + 3000;
    while (h.o < end) {
        o = h_tick(&h);
    }
    CHECK(o.level == LG_LEVEL_NORMAL);
    CHECK(h.c.mode.level == LG_LEVEL_NORMAL);

    /* An ack that is followed by a new fault before T_stable is voided: the lock stays. */
    unhealthy_west_approach(&h, 1);
    run_to(&h, h.o + 400);
    CHECK(h.out.level == LG_LEVEL_FAILSAFE);
    unhealthy_west_approach(&h, 0);
    run_to(&h, h.o + 100);
    lg_ctl_on_guard_ack(&h.c);
    run_to(&h, h.o + 10000); /* ack latched, 10 s of the 30 s elapsed */
    unhealthy_west_approach(&h, 1);
    run_to(&h, h.o + 400);
    unhealthy_west_approach(&h, 0);
    end = h.o + LG_T_STABLE_MS + 20000;
    while (h.o < end) {
        o = h_tick(&h);
        CHECK(o.level == LG_LEVEL_FAILSAFE);
    }
}

static void test_level4_holds_active_direction(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    H h;
    lg_ctl_out_t o;
    uint32_t end;

    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.occ[LG_DIR_WEST][LG_SENSOR_A] = 1;
    (void)h_tick(&h);
    h.occ[LG_DIR_WEST][LG_SENSOR_A] = 0;
    run_to(&h, 16000);
    h.gate_override = LG_GATE_FAULT; /* gate failure -> level 4 */
    o = h_tick(&h);
    CHECK(o.level == LG_LEVEL_FAILSAFE && o.level_reason == LG_REASON_GATE_FAILED);
    h.gate_override = -1; /* the gate reports again; level 4 is not left by itself */
    end = h.o + 90000;
    while (h.o < end) {
        o = h_tick(&h);
        CHECK(o.level == LG_LEVEL_FAILSAFE);
        CHECK(h.c.active[LG_DIR_WEST] == 1 && o.cleared_mask == 0);
        CHECK((h.c.last_unmet[LG_DIR_WEST] & LG_CLEAR_LEVEL4) != 0);
        CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights && o.bell);
    }
    lg_ctl_on_guard_ack(&h.c);
    run_to(&h, h.o + LG_T_STABLE_MS + 5000);
    CHECK(h.out.level == LG_LEVEL_NORMAL);
    run_to(&h, h.o + 10000);
    CHECK(h.c.active[LG_DIR_WEST] == 0);
    CHECK(h.out.gate_target == LG_GATE_TARGET_UP);
}

/* ---- 6. Gate silent or in FAULT ---- */

static void test_gate_silent(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    H h;
    lg_ctl_out_t o;

    h_init(&h, &cfg, 0);
    boot_to_10000(&h); /* the last reply came at o = 10000 */
    h.gate_override = GATE_SILENT;
    run_to(&h, 10090);
    CHECK(h.out.level == LG_LEVEL_NORMAL && h.out.gate_target == LG_GATE_TARGET_UP && h.out.lights == 0);
    o = h_tick(&h); /* 100 ms without a reply (>= gate_silence_ms): not known to be UP */
    CHECK(o.level == LG_LEVEL_FAILSAFE && o.level_changed);
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights && o.bell);

    /* Silent gate and an active direction: lower is due at once, lights on. */
    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.gate_override = GATE_SILENT;
    h.occ[LG_DIR_EAST][LG_SENSOR_A] = 1;
    run_to(&h, 10200);
    CHECK(h.out.gate_target == LG_GATE_TARGET_DOWN && h.out.lights && h.out.bell);

    /* A gate that never replied since init is silent after gate_silence_ms as well. */
    h_init(&h, &cfg, 0);
    h.gate_override = GATE_SILENT;
    run_to(&h, 20000);
    CHECK(h.out.level == LG_LEVEL_FAILSAFE && h.out.gate_target == LG_GATE_TARGET_DOWN);

    /* Gate answering with a state that is not UP while idle: lights and bell are on. */
    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.gate_override = LG_GATE_RAISING;
    o = h_tick(&h);
    CHECK(o.level == LG_LEVEL_NORMAL && o.gate_target == LG_GATE_TARGET_UP);
    CHECK(o.lights && o.bell);
}

static void test_gate_fault_is_level4(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    H h;
    lg_ctl_out_t o;

    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.gate_override = LG_GATE_FAULT;
    o = h_tick(&h);
    CHECK(o.level == LG_LEVEL_FAILSAFE && o.level_changed && o.level_from == LG_LEVEL_NORMAL);
    CHECK(o.level_reason == LG_REASON_GATE_FAILED);
    CHECK(o.gate_target == LG_GATE_TARGET_DOWN && o.lights && o.bell);
}

/* ---- 7. Invalid input and time wrap ---- */

static void test_invalid_input_is_rejected_and_counted(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t c;

    lg_ctl_init(&c, &cfg, 0);
    CHECK(lg_ctl_on_sample(&c, 255, 0, 0, 90, 0, 0, 0) == -1);
    CHECK(lg_ctl_on_sample(&c, 0, 255, 0, 90, 0, 0, 0) == -1);
    CHECK(lg_ctl_on_sample(&c, 0, 0, 255, 90, 0, 0, 0) == -1);
    CHECK(lg_ctl_on_sample(&c, 0, 0, 0, 255, 0, 0, 0) == -1);
    CHECK(c.samples_rejected == 4 && c.samples == 0);
    CHECK(c.sensor[0][0].seen == 0 && c.sensor[0][0].quality == 0); /* nothing stored */
    /* Bounds that are valid. */
    CHECK(lg_ctl_on_sample(&c, LG_DIR_COUNT - 1, LG_SENSOR_ID_COUNT - 1, 1, 100, 65535, 7, 7) == 0);
    CHECK(lg_ctl_on_sample(&c, 0, 0, 0, 0, 0, 7, 7) == 0);
    CHECK(c.samples == 2 && c.samples_rejected == 4);
    CHECK(c.sensor[LG_DIR_COUNT - 1][LG_SENSOR_ID_COUNT - 1].axles == 65535);
    /* Gate states. */
    CHECK(lg_ctl_on_gate_reply(&c, LG_GATE_FAULT + 1, 5) == -1);
    CHECK(lg_ctl_on_gate_reply(&c, 255, 5) == -1);
    CHECK(c.gate_seen == 0 && c.gate_replies == 0 && c.gate_rx_ms == 0 && c.gate_state == LG_GATE_DOWN);
    CHECK(lg_ctl_on_gate_reply(&c, LG_GATE_FAULT, 5) == 0 && c.gate_state == LG_GATE_FAULT);
}

static void test_rejected_input_is_no_heartbeat(void) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    H h;

    /* west/a sends only invalid samples: it must go silent (FAILED) like a dead sensor. */
    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.bad[LG_DIR_WEST][LG_SENSOR_A] = 1;
    run_to(&h, 10000 + cfg.sensor_silence_ms + 50);
    CHECK(h.out.level >= LG_LEVEL_DEGRADED);
    CHECK(h.c.samples_rejected > 0);

    /* A flood of invalid gate states is no reply either: the gate counts as silent. */
    h_init(&h, &cfg, 0);
    boot_to_10000(&h);
    h.gate_override = 77;
    run_to(&h, 10200);
    CHECK(h.out.level == LG_LEVEL_FAILSAFE && h.out.gate_target == LG_GATE_TARGET_DOWN);
}

typedef struct {
    uint32_t release, activated, down, cleared, up, l2, l4;
    uint32_t activations, cycles;
} wrap_res_t;

/* One scripted day starting at time `base`; results are offsets from base. */
static void wrap_script(uint32_t base, wrap_res_t *r) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    H h;

    memset(r, 0, sizeof *r);
    h_init(&h, &cfg, base);
    while (h.o < 40000) {
        uint32_t next = h.o + 10;
        lg_ctl_out_t o;

        h.occ[LG_DIR_WEST][LG_SENSOR_A] = (uint8_t)(next >= 10010 && next < 10510);
        h.occ[LG_DIR_WEST][LG_SENSOR_B] = h.occ[LG_DIR_WEST][LG_SENSOR_A]; /* a healthy pair (Step 9) */
        h.mute[LG_DIR_WEST][LG_SENSOR_B] = (uint8_t)(next >= 24000 && next < 24250);
        h.gate_override = next >= 30000 ? GATE_SILENT : -1;
        o = h_tick(&h);
        if (r->release == 0 && o.level == LG_LEVEL_NORMAL) {
            r->release = h.o;
        }
        if (r->activated == 0 && o.activated_mask) {
            r->activated = h.o;
        }
        if (r->down == 0 && r->activated != 0 && o.gate_target == LG_GATE_TARGET_DOWN) {
            r->down = h.o;
        }
        if (r->cleared == 0 && o.cleared_mask) {
            r->cleared = h.o;
        }
        if (r->up == 0 && r->cleared != 0 && o.gate_target == LG_GATE_TARGET_UP) {
            r->up = h.o;
        }
        if (r->l2 == 0 && h.o >= 24000 && o.level >= LG_LEVEL_DEGRADED) {
            r->l2 = h.o;
        }
        if (r->l4 == 0 && h.o >= 30000 && o.level == LG_LEVEL_FAILSAFE) {
            r->l4 = h.o;
        }
    }
    r->activations = h.c.activations;
    r->cycles = h.c.cycles;
}

static void test_time_wrap(void) {
    wrap_res_t ref;
    wrap_res_t w;
    uint32_t k;
    uint32_t bases[64];
    size_t n = 0;
    size_t i;

    wrap_script(0, &ref);
    CHECK(ref.release == 5010);
    CHECK(ref.activated == 10010);
    CHECK(ref.down == 15010);
    CHECK(ref.cleared == 22020);
    CHECK(ref.up == 22020);
    CHECK(ref.l2 == 24190);
    CHECK(ref.l4 == 30090);
    CHECK(ref.activations == 1 && ref.cycles == 1);

    for (k = 0; k < 46; k++) { /* the wrap falls on every phase of the day, 997 ms apart */
        bases[n++] = (uint32_t)0 - (k * 997u + 5u);
    }
    bases[n++] = 0xFFFFFFFFu;
    bases[n++] = 0x7FFFFFF0u; /* the signed boundary */
    bases[n++] = 0x80000000u;
    for (i = 0; i < n; i++) {
        wrap_script(bases[i], &w);
        if (memcmp(&w, &ref, sizeof w) != 0) {
            printf("wrap mismatch base=%u: release %u/%u act %u/%u down %u/%u clr %u/%u up %u/%u l2 %u/%u "
                   "l4 %u/%u\n",
                   (unsigned)bases[i], (unsigned)w.release, (unsigned)ref.release, (unsigned)w.activated,
                   (unsigned)ref.activated, (unsigned)w.down, (unsigned)ref.down, (unsigned)w.cleared,
                   (unsigned)ref.cleared, (unsigned)w.up, (unsigned)ref.up, (unsigned)w.l2,
                   (unsigned)ref.l2, (unsigned)w.l4, (unsigned)ref.l4);
        }
        CHECK(memcmp(&w, &ref, sizeof w) == 0);
    }
}

/* ---- 8. Randomised property test with the real gate and plant models ---- */

#define PROP_SEED 20261120u

static uint32_t rnd(uint32_t *s) {
    uint32_t x = *s;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static lg_json_doc_t g_doc;
static char g_text[16384];
static lg_crossing_cfg_t g_cc;
static lg_scenario_t g_normal_day;

static int load_files(void) {
    char err[LG_ERR_LEN];
    size_t n;

    if (lg_read_file("config/crossing.json", g_text, sizeof g_text, &n) != 0 ||
        lg_json_parse(&g_doc, g_text, n) != 0 || lg_crossing_from_json(&g_doc, &g_cc, err, sizeof err) != 0) {
        return -1;
    }
    if (lg_read_file("scenarios/normal_day.json", g_text, sizeof g_text, &n) != 0 ||
        lg_json_parse(&g_doc, g_text, n) != 0 ||
        lg_scenario_from_json(&g_doc, &g_cc, &g_normal_day, err, sizeof err) != 0) {
        return -1;
    }
    return 0;
}

/* Trains every 40 s at 30 m/s (the fastest speed in normal_day.json and in the replay), 100 m, 8 axles. */
static void craft(lg_scenario_t *sc, int alternate) {
    uint32_t k;

    memset(sc, 0, sizeof *sc);
    strcpy(sc->name, alternate ? "crafted_alt_40s" : "crafted_west_40s");
    sc->n_trains = 8;
    for (k = 0; k < sc->n_trains; k++) {
        sc->trains[k].id = (uint16_t)(k + 1);
        sc->trains[k].dir = alternate ? (uint8_t)(k & 1u) : (uint8_t)LG_DIR_WEST;
        sc->trains[k].axles = 8;
        sc->trains[k].t_strike_ms = 10000u + k * 40000u;
        sc->trains[k].speed_mmps = 30000;
        sc->trains[k].length_mm = 100000;
    }
}

typedef struct {
    uint32_t wrong_side_events, wrong_side_ticks, trains, activations, cycles;
    uint32_t inv_lights, inv_target_active, inv_target_level, inv_level4_exit;
    uint32_t first_bad_ms;
    uint32_t level4_ticks, faults, acks, max_level;
} prop_res_t;

static void note_bad(prop_res_t *r, uint32_t t) {
    if (r->first_bad_ms == 0) {
        r->first_bad_ms = t;
    }
}

static void run_property(const lg_scenario_t *sc, uint32_t seed, uint32_t end_ms, prop_res_t *r) {
    lg_plant_t plant;
    lg_sensor_model_t sm[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    uint32_t fault_until[LG_DIR_COUNT][LG_SENSOR_ID_COUNT];
    lg_gate_model_t gate;
    lg_gate_cfg_t gcfg = lg_gate_cfg_default();
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    lg_ctl_t ctl;
    lg_metrics_t met;
    uint32_t rng = seed;
    uint32_t gate_blackout_until = 0;
    uint32_t acks_since_l4 = 0;
    uint32_t t;
    int d;
    int i;

    memset(r, 0, sizeof *r);
    memset(fault_until, 0, sizeof fault_until);
    lg_plant_init(&plant, &g_cc, sc);
    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            lg_sensor_model_init(&sm[d][i], seed + (uint32_t)(d * 3 + i), 0);
        }
    }
    lg_gate_model_init(&gate, &gcfg, 0);
    lg_ctl_init(&ctl, &cfg, 0);
    lg_metrics_init(&met, 0);

    for (t = 10; t <= end_ms; t += LG_TICK_MS) {
        lg_ctl_out_t out;
        lg_truth_t it;
        uint8_t boot_lock_before = ctl.mode.boot_lock;

        for (d = 0; d < LG_DIR_COUNT; d++) {
            for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
                lg_truth_t tr = lg_plant_truth(&plant, t, (uint8_t)d, (uint8_t)i);
                lg_sensor_out_t so;

                /* Random fault episodes. STUCK_CLEAR is not injected: a stuck-clear approach sensor
                 * looks healthy and is not detectable until Step 11 (health scoring), so it would make the
                 * wrong-side invariant fail for a reason that is outside this step. */
                if (t >= fault_until[d][i]) {
                    uint32_t pick = rnd(&rng) % 100u;
                    uint8_t mode = LG_SFAULT_NONE;
                    uint32_t param = 0;

                    if (pick >= 97u) {
                        static const uint8_t modes[] = {LG_SFAULT_NOISE, LG_SFAULT_DROPOUT,
                                                        LG_SFAULT_STUCK_OCCUPIED, LG_SFAULT_DELAY,
                                                        LG_SFAULT_DRIFT};
                        mode = modes[rnd(&rng) % (sizeof modes / sizeof modes[0])];
                        param = mode == LG_SFAULT_DELAY ? rnd(&rng) % 301u
                                : mode == LG_SFAULT_DRIFT ? rnd(&rng) % 20u
                                : mode == LG_SFAULT_STUCK_OCCUPIED ? 0u
                                                                   : rnd(&rng) % 101u;
                        r->faults++;
                    }
                    (void)lg_sensor_model_set_fault(&sm[d][i], mode, param, t);
                    fault_until[d][i] = t + 200u + rnd(&rng) % 2000u;
                }
                if (lg_sensor_model_step(&sm[d][i], t, tr.occupied, tr.axles, &so)) {
                    if (rnd(&rng) % 50u == 0) {
                        continue; /* lost sample */
                    }
                    if (rnd(&rng) % 300u == 0) {
                        so.occupied = (uint8_t)!so.occupied; /* random occupancy glitch */
                    }
                    (void)lg_ctl_on_sample(&ctl, (uint8_t)d, (uint8_t)i, so.occupied, so.quality,
                                           so.axle_count, t, t);
                }
            }
        }
        if (rnd(&rng) % 300u == 0) {
            lg_ctl_on_guard_ack(&ctl);
            r->acks++;
            acks_since_l4++;
        }
        out = lg_ctl_step(&ctl, t);
        r->max_level = out.level > r->max_level ? out.level : r->max_level;
        if (out.level == LG_LEVEL_FAILSAFE) {
            r->level4_ticks++;
        }

        (void)lg_gate_model_command(
            &gate, out.gate_target == LG_GATE_TARGET_UP ? LG_GATE_CMD_RAISE : LG_GATE_CMD_LOWER, t);
        (void)lg_gate_model_command(&gate, out.lights ? LG_GATE_CMD_LIGHTS_ON : LG_GATE_CMD_LIGHTS_OFF, t);
        (void)lg_gate_model_command(&gate, out.bell ? LG_GATE_CMD_BELL_ON : LG_GATE_CMD_BELL_OFF, t);
        lg_gate_model_step(&gate, t);
        if (gate_blackout_until == 0 && rnd(&rng) % 30000u == 0) {
            gate_blackout_until = t + 150u; /* a lost-reply burst, longer than gate_silence_ms */
        }
        if (gate_blackout_until != 0 && t >= gate_blackout_until) {
            gate_blackout_until = 0;
        }
        if (gate_blackout_until == 0 && rnd(&rng) % 100u != 0) {
            (void)lg_ctl_on_gate_reply(&ctl, gate.state, t);
        }

        it = lg_plant_truth(&plant, t, LG_DIR_WEST, LG_SENSOR_ISLAND);
        lg_metrics_tick(&met, t, gate.state, gate.lights, it.island_any, 0);

        /* I2: whenever the gate is not UP its lights are on. */
        if (gate.state != LG_GATE_UP && !gate.lights) {
            r->inv_lights++;
            note_bad(r, t);
        }
        /* I3: target UP while a direction is active only while the controller hears the gate UP
         * (the pre-warning period): a gate that is not UP is never released while a direction is active. */
        if (out.gate_target == LG_GATE_TARGET_UP && (ctl.active[0] || ctl.active[1]) &&
            ctl.gate_state != LG_GATE_UP) {
            r->inv_target_active++;
            note_bad(r, t);
        }
        /* I4: target UP only at level 0 to 2. */
        if (out.gate_target == LG_GATE_TARGET_UP && out.level >= LG_LEVEL_UNCERTAIN) {
            r->inv_target_level++;
            note_bad(r, t);
        }
        /* I5: level 4 is left only through a boot release or after at least one guard ack. */
        if (out.level_changed && out.level == LG_LEVEL_FAILSAFE) {
            acks_since_l4 = 0;
        }
        if (out.level_changed && out.level_from == LG_LEVEL_FAILSAFE && out.level < LG_LEVEL_FAILSAFE &&
            !boot_lock_before && acks_since_l4 == 0) {
            r->inv_level4_exit++;
            note_bad(r, t);
        }
    }
    r->wrong_side_events = met.wrong_side_events;
    r->wrong_side_ticks = met.wrong_side_ticks;
    r->trains = met.trains;
    r->activations = ctl.activations;
    r->cycles = ctl.cycles;
}

static void property_case(const char *label, const lg_scenario_t *sc, uint32_t seed, uint32_t end_ms) {
    prop_res_t r;

    run_property(sc, seed, end_ms, &r);
    printf("property %s seed=%u ticks=%u trains=%u activations=%u cycles=%u faults=%u acks=%u max_level=%u "
           "level4_ticks=%u wrong_side_events=%u wrong_side_ticks=%u inv_lights=%u inv_target_active=%u "
           "inv_target_level=%u inv_level4_exit=%u first_bad_ms=%u\n",
           label, (unsigned)seed, (unsigned)(end_ms / LG_TICK_MS), (unsigned)r.trains,
           (unsigned)r.activations, (unsigned)r.cycles, (unsigned)r.faults, (unsigned)r.acks,
           (unsigned)r.max_level, (unsigned)r.level4_ticks, (unsigned)r.wrong_side_events,
           (unsigned)r.wrong_side_ticks, (unsigned)r.inv_lights, (unsigned)r.inv_target_active,
           (unsigned)r.inv_target_level, (unsigned)r.inv_level4_exit, (unsigned)r.first_bad_ms);
    CHECK(r.wrong_side_events == 0); /* hard rule: never relaxed */
    CHECK(r.wrong_side_ticks == 0);
    CHECK(r.inv_lights == 0);
    CHECK(r.inv_target_active == 0);
    CHECK(r.inv_target_level == 0);
    CHECK(r.inv_level4_exit == 0);
    CHECK(r.trains == sc->n_trains); /* the run is not vacuous: every train reached the island */
    CHECK(r.activations > 0 && r.faults > 0);
}

static void test_property(void) {
    lg_scenario_t alt;
    lg_scenario_t west;
    uint32_t k;

    craft(&alt, 1);
    craft(&west, 0);
    for (k = 0; k < 4; k++) {
        uint32_t seed = PROP_SEED + k;

        property_case("normal_day", &g_normal_day, seed, 300000);
        property_case("crafted_alt_40s_30mps", &alt, seed, 330000);
        property_case("crafted_west_40s_30mps", &west, seed, 330000);
    }
}

int main(void) {
    if (load_files() != 0) {
        puts("FAIL: could not load config/crossing.json or scenarios/normal_day.json (run from the repo root)");
        return 1;
    }
    test_same_direction_following_train();
    test_opposite_directions_overlap();
    test_lower_delay_function();
    test_gate_not_up_at_activation_lowers_at_once();
    test_level2_delay_is_shorter();
    test_island_occupied_holds_gate_down();
    test_unusable_island_fails_the_level();
    test_stale_boundary_for_activation();
    test_level3_holds_gate_down();
    test_level4_needs_guard_ack();
    test_level4_holds_active_direction();
    test_gate_silent();
    test_gate_fault_is_level4();
    test_invalid_input_is_rejected_and_counted();
    test_rejected_input_is_no_heartbeat();
    test_time_wrap();
    test_property();
    LG_TEST_DONE("test_controller_logic_extra");
}
