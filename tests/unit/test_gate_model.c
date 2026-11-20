#include <string.h>

#include "lg_gate_model.h"
#include "lg_test.h"

static lg_gate_cfg_t cfg;

/* Step the model every ms from t_from (exclusive) up to t_to (inclusive). */
static void run_ms(lg_gate_model_t *m, uint32_t t_from, uint32_t t_to) {
    uint32_t t;

    for (t = t_from + 1; t != t_to + 1; t++) {
        lg_gate_model_step(m, t);
    }
}

/* Raise the gate with a keepalive every 50 ms; returns the time at which it is UP (or 0). */
static uint32_t raise_fully(lg_gate_model_t *m, uint32_t t0) {
    uint32_t t;

    CHECK(lg_gate_model_command(m, LG_GATE_CMD_RAISE, t0) == 0);
    for (t = t0 + 1; t <= t0 + 3 * cfg.travel_ms; t++) {
        if ((t - t0) % 50 == 0) {
            lg_gate_model_feed(m, t);
        }
        lg_gate_model_step(m, t);
        if (m->state == LG_GATE_UP) {
            return t;
        }
    }
    return 0;
}

static void test_init(void) {
    lg_gate_model_t m;

    lg_gate_model_init(&m, &cfg, 1000);
    CHECK(m.state == LG_GATE_DOWN);
    CHECK(m.position_mdeg == LG_GATE_POS_DOWN_MDEG);
    CHECK(m.target == LG_GATE_TARGET_DOWN);
    CHECK(m.lights == 1 && m.bell == 1); /* lights flashing, bell on at boot */
    CHECK(m.fault == LG_GFAULT_NONE);
    CHECK(m.watchdog_trips == 0 && m.rejected == 0);

    /* Idle at the end position: stays DOWN, no watchdog trip, holding current only. */
    run_ms(&m, 1000, 1000 + 10 * cfg.watchdog_ms);
    CHECK(m.state == LG_GATE_DOWN && m.watchdog_trips == 0);
    CHECK(m.current_ma == cfg.current_hold_ma);
}

static void test_raise_and_lower_timing(void) {
    lg_gate_model_t m;
    uint32_t t_up;
    uint32_t t = 0;
    int32_t last_pos;

    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 100) == 0);
    CHECK(m.state == LG_GATE_DOWN || m.state == LG_GATE_RAISING);

    /* Position decreases monotonically, state RAISING, moving current. */
    last_pos = m.position_mdeg;
    for (t = 101; t < 100 + cfg.travel_ms; t++) {
        if ((t - 100) % 50 == 0) {
            lg_gate_model_feed(&m, t);
        }
        lg_gate_model_step(&m, t);
        CHECK(m.position_mdeg <= last_pos);
        last_pos = m.position_mdeg;
        if (m.state == LG_GATE_UP) {
            break;
        }
    }
    CHECK(m.state == LG_GATE_RAISING);
    CHECK(m.current_ma == cfg.current_moving_ma);
    CHECK(m.position_mdeg > LG_GATE_POS_UP_MDEG);

    lg_gate_model_feed(&m, 100 + cfg.travel_ms);
    lg_gate_model_step(&m, 100 + cfg.travel_ms);
    CHECK(m.state == LG_GATE_UP);
    CHECK(m.position_mdeg == LG_GATE_POS_UP_MDEG);
    CHECK(m.last_travel_ms == cfg.travel_ms);
    CHECK(m.current_ma == cfg.current_hold_ma);
    t_up = 100 + cfg.travel_ms;

    /* Lower again: DOWN exactly after one travel time, state LOWERING in between. */
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_LOWER, t_up + 10) == 0);
    for (t = t_up + 11; t < t_up + 10 + cfg.travel_ms; t++) {
        lg_gate_model_step(&m, t);
        CHECK(m.state == LG_GATE_LOWERING);
        if (m.state != LG_GATE_LOWERING) {
            break;
        }
    }
    lg_gate_model_step(&m, t_up + 10 + cfg.travel_ms);
    CHECK(m.state == LG_GATE_DOWN && m.position_mdeg == LG_GATE_POS_DOWN_MDEG);
    CHECK(m.last_travel_ms == cfg.travel_ms);
}

static void test_coarse_steps_keep_time(void) {
    lg_gate_model_t m;
    uint32_t t;

    /* A 10 ms step period (the real tick) still completes in nominal time, within one tick. */
    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    for (t = 10; t <= cfg.travel_ms + 10; t += 10) {
        lg_gate_model_feed(&m, t);
        lg_gate_model_step(&m, t);
    }
    CHECK(m.state == LG_GATE_UP);
    CHECK(m.last_travel_ms >= cfg.travel_ms && m.last_travel_ms <= cfg.travel_ms + 10);
}

static void test_watchdog(void) {
    lg_gate_model_t m;
    uint32_t t_up;
    uint32_t t;

    lg_gate_model_init(&m, &cfg, 0);
    t_up = raise_fully(&m, 0);
    CHECK(t_up != 0);

    /* Keepalives every 50 ms keep it up for a long time. */
    for (t = t_up + 1; t <= t_up + 5000; t++) {
        if ((t - t_up) % 50 == 0) {
            lg_gate_model_feed(&m, t);
        }
        lg_gate_model_step(&m, t);
    }
    CHECK(m.state == LG_GATE_UP && m.watchdog_trips == 0);

    /* Last keepalive at T. No trip at T + watchdog_ms, trip one ms later. */
    {
        uint32_t T = t_up + 5000;
        lg_gate_model_feed(&m, T);
        lg_gate_model_command(&m, LG_GATE_CMD_LIGHTS_OFF, T);
        lg_gate_model_command(&m, LG_GATE_CMD_BELL_OFF, T);
        run_ms(&m, T, T + cfg.watchdog_ms);
        CHECK(m.watchdog_trips == 0 && m.state == LG_GATE_UP && m.lights == 0 && m.bell == 0);
        lg_gate_model_step(&m, T + cfg.watchdog_ms + 1);
        CHECK(m.watchdog_trips == 1);
        CHECK(m.target == LG_GATE_TARGET_DOWN);
        CHECK(m.lights == 1 && m.bell == 1);
        CHECK(m.state == LG_GATE_UP || m.state == LG_GATE_LOWERING);
        run_ms(&m, T + cfg.watchdog_ms + 1, T + cfg.watchdog_ms + 1 + cfg.travel_ms + 5);
        CHECK(m.state == LG_GATE_DOWN);
        CHECK(m.watchdog_trips == 1); /* one trip, not one per tick */
    }

    /* Once DOWN it stays down without a controller, and does not trip again. */
    run_ms(&m, t_up + 5000 + cfg.watchdog_ms + cfg.travel_ms + 10,
           t_up + 5000 + 10 * cfg.travel_ms);
    CHECK(m.state == LG_GATE_DOWN && m.watchdog_trips == 1);
}

static void test_watchdog_command_also_feeds(void) {
    lg_gate_model_t m;
    uint32_t t_up;
    uint32_t t;

    lg_gate_model_init(&m, &cfg, 0);
    t_up = raise_fully(&m, 0);
    /* A repeated command every 20 ms (no separate heartbeat) is enough. */
    for (t = t_up + 1; t <= t_up + 3000; t++) {
        if ((t - t_up) % 20 == 0) {
            CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, t) == 0);
        }
        lg_gate_model_step(&m, t);
    }
    CHECK(m.state == LG_GATE_UP && m.watchdog_trips == 0);
}

static void test_watchdog_during_raise(void) {
    lg_gate_model_t m;
    uint32_t t;

    /* The controller dies halfway up: the gate reverses and goes down. */
    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    for (t = 1; t <= cfg.travel_ms / 2; t++) {
        if (t % 50 == 0) {
            lg_gate_model_feed(&m, t);
        }
        lg_gate_model_step(&m, t);
    }
    CHECK(m.state == LG_GATE_RAISING && m.position_mdeg < LG_GATE_POS_DOWN_MDEG);
    run_ms(&m, cfg.travel_ms / 2, cfg.travel_ms / 2 + cfg.watchdog_ms + 2);
    CHECK(m.watchdog_trips == 1);
    CHECK(m.state == LG_GATE_LOWERING);
    run_ms(&m, cfg.travel_ms / 2 + cfg.watchdog_ms + 2, cfg.travel_ms * 3);
    CHECK(m.state == LG_GATE_DOWN);
}

static void test_raise_after_trip(void) {
    lg_gate_model_t m;
    uint32_t t_up;
    uint32_t t;

    /* Controller silent, gate lowered by itself, controller comes back and raises it. */
    lg_gate_model_init(&m, &cfg, 0);
    t_up = raise_fully(&m, 0);
    run_ms(&m, t_up, t_up + 2 * cfg.watchdog_ms + cfg.travel_ms);
    CHECK(m.state == LG_GATE_DOWN && m.watchdog_trips == 1);
    t = t_up + 2 * cfg.watchdog_ms + cfg.travel_ms + 10;
    CHECK(raise_fully(&m, t) != 0);
    CHECK(m.state == LG_GATE_UP);
}

static void test_trip_does_not_credit_time_before_trip(void) {
    lg_gate_model_t m;
    uint32_t t_up;
    uint32_t t;

    /* Real tick period: 10 ms steps. After the trip the lowering must take exactly one travel
     * time; time that passed before the trip must not count as movement. */
    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    for (t = 10; t <= cfg.travel_ms + 10; t += 10) {
        lg_gate_model_feed(&m, t);
        lg_gate_model_step(&m, t);
    }
    CHECK(m.state == LG_GATE_UP);
    t_up = t - 10;
    for (t = t_up + 10; m.watchdog_trips == 0 && t < t_up + 1000; t += 10) {
        lg_gate_model_step(&m, t);
    }
    CHECK(m.watchdog_trips == 1);
    CHECK(m.position_mdeg == LG_GATE_POS_UP_MDEG); /* no movement in the trip step itself */
    while (m.state != LG_GATE_DOWN && t < t_up + 10 * cfg.travel_ms) {
        lg_gate_model_step(&m, t);
        t += 10;
    }
    CHECK(m.state == LG_GATE_DOWN);
    CHECK(m.last_travel_ms == cfg.travel_ms || m.last_travel_ms == cfg.travel_ms + 10);
}

static void test_invalid_commands(void) {
    lg_gate_model_t m;

    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_command(&m, 0, 10) == -1);
    CHECK(lg_gate_model_command(&m, 7, 10) == -1);
    CHECK(lg_gate_model_command(&m, 255, 10) == -1);
    CHECK(m.rejected == 3);
    /* An invalid command does not feed the watchdog and does not move the gate. */
    CHECK(m.last_feed_ms == 0 && m.target == LG_GATE_TARGET_DOWN && m.state == LG_GATE_DOWN);

    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_LIGHTS_OFF, 20) == 0 && m.lights == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_LIGHTS_ON, 30) == 0 && m.lights == 1);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_BELL_OFF, 40) == 0 && m.bell == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_BELL_ON, 50) == 0 && m.bell == 1);
}

static void test_slow_fault(void) {
    lg_gate_model_t m;
    uint32_t t;

    /* SLOW 150 percent: raising takes 1.5x nominal and is not a FAULT. */
    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_SLOW, 150, 0) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    for (t = 1; t <= 3 * cfg.travel_ms; t++) {
        if (t % 50 == 0) {
            lg_gate_model_feed(&m, t);
        }
        lg_gate_model_step(&m, t);
        CHECK(m.state != LG_GATE_FAULT);
        if (m.state == LG_GATE_UP) {
            break;
        }
    }
    CHECK(m.state == LG_GATE_UP);
    CHECK(m.last_travel_ms == cfg.travel_ms * 3 / 2);

    /* Cleared: back to nominal travel time. */
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_NONE, 0, t) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_LOWER, t) == 0);
    run_ms(&m, t, t + cfg.travel_ms);
    CHECK(m.state == LG_GATE_DOWN && m.last_travel_ms == cfg.travel_ms);
}

static void test_very_slow_becomes_fault_but_completes(void) {
    lg_gate_model_t m;
    uint32_t t;
    int saw_fault = 0;

    /* SLOW 400 percent exceeds the fault timeout (3x): FAULT is reported, the gate still arrives. */
    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_SLOW, 400, 0) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    for (t = 1; t <= 5 * cfg.travel_ms; t++) {
        if (t % 50 == 0) {
            lg_gate_model_feed(&m, t);
        }
        lg_gate_model_step(&m, t);
        if (m.state == LG_GATE_FAULT) {
            saw_fault = 1;
        }
        if (m.state == LG_GATE_UP) {
            break;
        }
    }
    CHECK(saw_fault);
    CHECK(m.state == LG_GATE_UP);
    CHECK(m.last_travel_ms == cfg.travel_ms * 4);
}

static void test_high_current(void) {
    lg_gate_model_t m;
    uint32_t t;

    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_HIGH_CURRENT, 250, 0) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    for (t = 1; t <= 100; t++) {
        lg_gate_model_step(&m, t);
    }
    CHECK(m.state == LG_GATE_RAISING);
    CHECK(m.current_ma == cfg.current_moving_ma * 250 / 100);
    CHECK(m.last_travel_ms == 0 || m.last_travel_ms == cfg.travel_ms);
}

static void test_stuck_up_blocks_lowering(void) {
    lg_gate_model_t m;
    uint32_t t_up;
    uint32_t t;
    uint32_t t_end;
    int32_t pos;

    lg_gate_model_init(&m, &cfg, 0);
    t_up = raise_fully(&m, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_STUCK_UP, 0, t_up) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_LOWER, t_up) == 0);
    pos = m.position_mdeg;
    for (t = t_up + 1; t <= t_up + cfg.fault_timeout_factor * cfg.travel_ms + 100; t++) {
        if ((t - t_up) % 50 == 0) {
            lg_gate_model_feed(&m, t);
        }
        lg_gate_model_step(&m, t);
    }
    CHECK(m.position_mdeg == pos); /* did not move */
    CHECK(m.state == LG_GATE_FAULT);

    /* Clearing the fault lets the movement complete. */
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_NONE, 0, t) == 0);
    t_end = t + 2 * cfg.travel_ms;
    for (; t <= t_end; t++) {
        if (t % 50 == 0) {
            lg_gate_model_feed(&m, t);
        }
        lg_gate_model_step(&m, t);
        if (m.state == LG_GATE_DOWN) {
            break;
        }
    }
    CHECK(m.state == LG_GATE_DOWN);
}

static void test_stuck_down_blocks_raising(void) {
    lg_gate_model_t m;
    uint32_t t;

    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_STUCK_DOWN, 0, 0) == 0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 0) == 0);
    for (t = 1; t <= cfg.fault_timeout_factor * cfg.travel_ms + 100; t++) {
        if (t % 50 == 0) {
            lg_gate_model_feed(&m, t);
        }
        lg_gate_model_step(&m, t);
    }
    CHECK(m.position_mdeg == LG_GATE_POS_DOWN_MDEG);
    CHECK(m.state == LG_GATE_FAULT);

    /* A STUCK_DOWN gate can still be lowered (it is already down). */
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_LOWER, t) == 0);
    lg_gate_model_step(&m, t + 1);
    CHECK(m.state == LG_GATE_DOWN);
}

static void test_fault_validation(void) {
    lg_gate_model_t m;

    lg_gate_model_init(&m, &cfg, 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_CRASH, 100, 0) == -1);
    CHECK(lg_gate_model_set_fault(&m, 9, 100, 0) == -1);
    CHECK(lg_gate_model_set_fault(&m, 200, 100, 0) == -1);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_SLOW, 99, 0) == -1);   /* faster than nominal */
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_SLOW, 0, 0) == -1);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_SLOW, LG_GATE_FACTOR_MAX_PCT + 1, 0) == -1);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_HIGH_CURRENT, 99, 0) == -1);
    CHECK(m.fault == LG_GFAULT_NONE); /* rejected requests change nothing */
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_SLOW, LG_GATE_FACTOR_MAX_PCT, 0) == 0);
    CHECK(lg_gate_model_set_fault(&m, LG_GFAULT_STUCK_UP, 12345, 0) == 0); /* factor unused */
}

static void test_time_wrap(void) {
    lg_gate_model_t m;
    uint32_t t0 = 0xFFFFFFFFu - 1500u;
    uint32_t t;

    lg_gate_model_init(&m, &cfg, t0);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, t0) == 0);
    for (t = t0 + 1; t != t0 + 1 + cfg.travel_ms; t++) { /* crosses zero */
        if ((t - t0) % 50 == 0) {
            lg_gate_model_feed(&m, t);
        }
        lg_gate_model_step(&m, t);
    }
    CHECK(m.state == LG_GATE_UP && m.last_travel_ms == cfg.travel_ms);
    CHECK(m.watchdog_trips == 0);

    /* Watchdog across the wrap. */
    t = t0 + cfg.travel_ms;
    for (t = t + 1; t != t0 + cfg.travel_ms + 200; t++) {
        lg_gate_model_step(&m, t);
    }
    CHECK(m.watchdog_trips == 1);
}

static void test_clock_going_backwards(void) {
    lg_gate_model_t m;

    lg_gate_model_init(&m, &cfg, 10000);
    CHECK(lg_gate_model_command(&m, LG_GATE_CMD_RAISE, 10000) == 0);
    {
        uint32_t t;
        for (t = 10001; t <= 10500; t++) {
            if (t % 50 == 0) {
                lg_gate_model_feed(&m, t);
            }
            lg_gate_model_step(&m, t);
        }
    }
    CHECK(m.state == LG_GATE_RAISING);
    {
        int32_t pos = m.position_mdeg;
        uint32_t trips = m.watchdog_trips;
        lg_gate_model_step(&m, 10400); /* clock stepped back: ignored, no movement, no trip */
        CHECK(m.position_mdeg == pos);
        CHECK(m.watchdog_trips == trips);
        lg_gate_model_step(&m, 10501); /* and time continues from the last good step */
        CHECK(m.position_mdeg != pos);
    }
}

static void test_text_and_state_message(void) {
    lg_gate_model_t m;
    char buf[200];
    char small[8];
    lg_gate_state_msg_t msg;
    size_t n;

    lg_gate_model_init(&m, &cfg, 0);
    n = lg_gate_model_text(&m, buf, sizeof buf);
    CHECK(n == strlen(buf) && n > 0 && buf[n - 1] == '\n');
    CHECK(strstr(buf, "state=DOWN") != NULL);
    CHECK(strstr(buf, "position=90") != NULL);
    CHECK(strstr(buf, "fault=NONE") != NULL);
    CHECK(strstr(buf, "watchdog_trips=0") != NULL);

    /* A buffer that is too small is truncated and still NUL terminated. */
    n = lg_gate_model_text(&m, small, sizeof small);
    CHECK(n < sizeof small && small[n] == '\0');

    memset(&msg, 0, sizeof msg);
    lg_gate_model_fill_state(&m, &msg);
    CHECK(msg.state == LG_GATE_DOWN && msg.position_deg == 90);
    CHECK(msg.current_ma == cfg.current_hold_ma);
    CHECK(strcmp(lg_gate_state_name(LG_GATE_FAULT), "FAULT") == 0);
    CHECK(strcmp(lg_gate_state_name(99), "UNKNOWN") == 0);
    CHECK(strcmp(lg_gate_fault_name(LG_GFAULT_STUCK_UP), "STUCK_UP") == 0);
    CHECK(strcmp(lg_gate_fault_name(99), "UNKNOWN") == 0);
}

int main(void) {
    cfg = lg_gate_cfg_default();
    test_init();
    test_raise_and_lower_timing();
    test_coarse_steps_keep_time();
    test_watchdog();
    test_watchdog_command_also_feeds();
    test_watchdog_during_raise();
    test_raise_after_trip();
    test_trip_does_not_credit_time_before_trip();
    test_invalid_commands();
    test_slow_fault();
    test_very_slow_becomes_fault_but_completes();
    test_high_current();
    test_stuck_up_blocks_lowering();
    test_stuck_down_blocks_raising();
    test_fault_validation();
    test_time_wrap();
    test_clock_going_backwards();
    test_text_and_state_message();
    LG_TEST_DONE("test_gate_model");
}
