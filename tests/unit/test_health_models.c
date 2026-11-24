/* Step 11 additions to the device models: sensor statistics, gate movement figures, battery. */
#include <string.h>

#include "lg_battery_model.h"
#include "lg_gate_model.h"
#include "lg_sensor_model.h"
#include "lg_test.h"

static void test_sensor_acc(void) {
    lg_sensor_acc_t a;
    lg_sensor_out_t clear = {0, 97, 0, LG_SENSOR_CLEAR_LEVEL + 10};
    lg_sensor_out_t occ = {1, 97, 0, LG_SENSOR_OCCUPIED_LEVEL - 20};
    lg_sensor_out_t blind = {0, 0, 0, 0};

    memset(&a, 0, sizeof a);
    lg_sensor_acc_step(&a, 0, 1, &occ); /* first reading: no edge */
    CHECK(a.rise_count == 0 && a.dev_sum == 20 && a.dev_count == 1);
    lg_sensor_acc_step(&a, 10, 1, &clear);
    CHECK(a.last_fall_ms == 10 && a.dev_sum == 30);
    lg_sensor_acc_step(&a, 20, 0, NULL);
    CHECK(a.no_sample == 1 && a.ticks == 3);
    lg_sensor_acc_step(&a, 30, 1, &blind);
    CHECK(a.blind == 1 && a.dev_count == 2); /* blind: no noise figure */
    lg_sensor_acc_step(&a, 40, 1, &occ);
    CHECK(a.rise_count == 1 && a.last_rise_ms == 40);
    lg_sensor_acc_step(&a, 50, 1, &occ);
    CHECK(a.rise_count == 1 && a.ticks == 6);
}

static void test_gate_moves_and_peak(void) {
    lg_gate_model_t g;
    lg_gate_cfg_t cfg = lg_gate_cfg_default();
    uint32_t t;

    lg_gate_model_init(&g, &cfg, 0);
    CHECK(g.moves_done == 0);
    (void)lg_gate_model_set_fault(&g, LG_GFAULT_HIGH_CURRENT, 150, 0);
    (void)lg_gate_model_command(&g, LG_GATE_CMD_RAISE, 0);
    for (t = 10; t <= 5000; t += 10) {
        lg_gate_model_feed(&g, t);
        lg_gate_model_step(&g, t);
    }
    CHECK(g.moves_done == 1);
    CHECK(g.last_peak_ma == LG_GATE_CURRENT_MOVING_MA * 150 / 100);
    CHECK(g.last_travel_ms >= LG_GATE_TRAVEL_MS && g.last_travel_ms <= LG_GATE_TRAVEL_MS + 10);
    (void)lg_gate_model_set_fault(&g, LG_GFAULT_SLOW, 150, t);
    (void)lg_gate_model_command(&g, LG_GATE_CMD_LOWER, t);
    for (; t <= 12000; t += 10) {
        lg_gate_model_feed(&g, t);
        lg_gate_model_step(&g, t);
    }
    CHECK(g.moves_done == 2);
    CHECK(g.last_peak_ma == LG_GATE_CURRENT_MOVING_MA);
    CHECK(g.last_travel_ms >= LG_GATE_TRAVEL_MS * 3 / 2 && g.last_travel_ms <= LG_GATE_TRAVEL_MS * 3 / 2 + 10);
    /* Standing still adds no movement. */
    for (; t <= 20000; t += 10) {
        lg_gate_model_feed(&g, t);
        lg_gate_model_step(&g, t);
    }
    CHECK(g.moves_done == 2);
}

static void test_battery_model(void) {
    lg_battery_model_t b;

    lg_battery_model_init(&b, 0);
    CHECK(lg_battery_model_mv(&b) == LG_BATTERY_FULL_MV);
    lg_battery_model_step(&b, 10000);
    CHECK(lg_battery_model_mv(&b) == LG_BATTERY_FULL_MV); /* the charger holds it full */
    CHECK(lg_battery_model_set_drain(&b, LG_BATTERY_DRAIN_MAX_MV_S + 1, 10000) == -1);
    CHECK(lg_battery_model_set_drain(&b, 5, 10000) == 0);
    lg_battery_model_step(&b, 20000);
    CHECK(lg_battery_model_mv(&b) == LG_BATTERY_FULL_MV - 50);
    lg_battery_model_step(&b, 15000); /* time going back: ignored */
    CHECK(lg_battery_model_mv(&b) == LG_BATTERY_FULL_MV - 50);
    CHECK(lg_battery_model_set_drain(&b, 0, 20000) == 0);
    lg_battery_model_step(&b, 21000);
    CHECK(lg_battery_model_mv(&b) == LG_BATTERY_FULL_MV - 50 + LG_BATTERY_CHARGE_MV_S);
    lg_battery_model_step(&b, 100000);
    CHECK(lg_battery_model_mv(&b) == LG_BATTERY_FULL_MV); /* never above full */
    (void)lg_battery_model_set_drain(&b, 1000, 100000);
    lg_battery_model_step(&b, 200000);
    CHECK(lg_battery_model_mv(&b) == 0); /* never below 0 */
}

/* Review L4: the peak current of a cancelled movement does not carry over into the next one. */
static void test_peak_of_cancelled_movement(void) {
    lg_gate_model_t g;
    lg_gate_cfg_t cfg = lg_gate_cfg_default();
    uint32_t t;

    lg_gate_model_init(&g, &cfg, 0);
    (void)lg_gate_model_command(&g, LG_GATE_CMD_RAISE, 0);
    for (t = 10; t <= 5000; t += 10) {
        lg_gate_model_feed(&g, t);
        lg_gate_model_step(&g, t);
    }
    CHECK(g.moves_done == 1);
    (void)lg_gate_model_set_fault(&g, LG_GFAULT_HIGH_CURRENT, 150, t);
    (void)lg_gate_model_command(&g, LG_GATE_CMD_LOWER, t);
    for (; t <= 6000; t += 10) { /* one second of lowering at 150 % current */
        lg_gate_model_feed(&g, t);
        lg_gate_model_step(&g, t);
    }
    (void)lg_gate_model_set_fault(&g, LG_GFAULT_NONE, 100, t);
    (void)lg_gate_model_command(&g, LG_GATE_CMD_RAISE, t); /* cancelled: back up at nominal current */
    for (; t <= 12000; t += 10) {
        lg_gate_model_feed(&g, t);
        lg_gate_model_step(&g, t);
    }
    CHECK(g.moves_done == 2);
    CHECK(g.last_peak_ma == LG_GATE_CURRENT_MOVING_MA);
}

int main(void) {
    test_peak_of_cancelled_movement();
    test_sensor_acc();
    test_gate_moves_and_peak();
    test_battery_model();
    LG_TEST_DONE("test_health_models");
}
