#include "lg_health.h"
#include "lg_metrics.h"
#include "lg_test.h"
#include "lg_timetable.h"

static void test_normal_cycle(void) {
    lg_metrics_t m;

    lg_metrics_init(&m, 0);
    lg_metrics_tick(&m, 0, LG_GATE_UP, 0, 0, 0);
    lg_metrics_tick(&m, 1000, LG_GATE_LOWERING, 1, 0, 0);  /* train detected */
    lg_metrics_tick(&m, 4000, LG_GATE_DOWN, 1, 0, 0);
    lg_metrics_tick(&m, 11000, LG_GATE_DOWN, 1, 1, 0);     /* train reaches the island */
    lg_metrics_tick(&m, 13000, LG_GATE_DOWN, 1, 0, 0);     /* island clear */
    lg_metrics_tick(&m, 14000, LG_GATE_RAISING, 1, 0, 0);
    lg_metrics_tick(&m, 16000, LG_GATE_UP, 0, 0, 0);

    CHECK(m.trains == 1);
    CHECK(m.warning.count == 1 && m.warning.last_ms == 10000);
    CHECK(m.closure.count == 1 && m.closure.last_ms == 10000); /* DOWN at 4000, rising at 14000 */
    CHECK(m.closed_ms == 15000);                               /* not UP from 1000 to 16000 */
    CHECK(lg_metrics_closure_ms_per_hour(&m) == 3375000u);
    CHECK(m.trains_without_warning == 0);
    CHECK(m.wrong_side_events == 0 && m.wrong_side_ticks == 0);
    CHECK(lg_metrics_wrong_side_ok(&m));
}

static void test_two_trains_statistics(void) {
    lg_metrics_t m;

    lg_metrics_init(&m, 0);
    lg_metrics_tick(&m, 0, LG_GATE_UP, 0, 0, 0);
    /* Train 1: lights on 1000, island 9000 (warning 8000); DOWN 2000, rising 10000 (closure 8000). */
    lg_metrics_tick(&m, 1000, LG_GATE_LOWERING, 1, 0, 0);
    lg_metrics_tick(&m, 2000, LG_GATE_DOWN, 1, 0, 0);
    lg_metrics_tick(&m, 9000, LG_GATE_DOWN, 1, 1, 0);
    lg_metrics_tick(&m, 9500, LG_GATE_DOWN, 1, 0, 0);
    lg_metrics_tick(&m, 10000, LG_GATE_RAISING, 1, 0, 0);
    lg_metrics_tick(&m, 11000, LG_GATE_UP, 0, 0, 0);
    /* Train 2: lights on 20000, island 24000 (warning 4000); DOWN 21000, rising 26000 (closure 5000). */
    lg_metrics_tick(&m, 20000, LG_GATE_LOWERING, 1, 0, 0);
    lg_metrics_tick(&m, 21000, LG_GATE_DOWN, 1, 0, 0);
    lg_metrics_tick(&m, 24000, LG_GATE_DOWN, 1, 1, 0);
    lg_metrics_tick(&m, 25000, LG_GATE_DOWN, 1, 0, 0);
    lg_metrics_tick(&m, 26000, LG_GATE_RAISING, 1, 0, 0);
    lg_metrics_tick(&m, 27000, LG_GATE_UP, 0, 0, 0);

    CHECK(m.trains == 2);
    CHECK(m.warning.count == 2 && m.warning.last_ms == 4000 && m.warning.max_ms == 8000 &&
          m.warning.sum_ms == 12000);
    CHECK(m.closure.count == 2 && m.closure.last_ms == 5000 && m.closure.max_ms == 8000);
    CHECK(lg_metrics_wrong_side_ok(&m));
}

static void test_wrong_side_detected(void) {
    lg_metrics_t m;

    /* The detector must fire: a train on the island while the gate is not DOWN. */
    lg_metrics_init(&m, 0);
    lg_metrics_tick(&m, 0, LG_GATE_UP, 0, 0, 0);
    lg_metrics_tick(&m, 100, LG_GATE_UP, 0, 1, 0);
    lg_metrics_tick(&m, 200, LG_GATE_UP, 0, 1, 0);
    CHECK(m.wrong_side_events == 1 && m.wrong_side_ticks == 2);
    CHECK(!lg_metrics_wrong_side_ok(&m));

    /* Gate still lowering counts as wrong side too; the same train, different state, no new edge. */
    lg_metrics_tick(&m, 300, LG_GATE_LOWERING, 1, 1, 0);
    CHECK(m.wrong_side_events == 1 && m.wrong_side_ticks == 3);

    /* Gate DOWN with the train on the island is fine, and a later episode is a new event. */
    lg_metrics_tick(&m, 400, LG_GATE_DOWN, 1, 1, 0);
    CHECK(m.wrong_side_ticks == 3);
    lg_metrics_tick(&m, 500, LG_GATE_FAULT, 1, 1, 0);
    CHECK(m.wrong_side_events == 2 && m.wrong_side_ticks == 4);
    lg_metrics_tick(&m, 600, LG_GATE_RAISING, 1, 1, 0);
    CHECK(m.wrong_side_ticks == 5); /* RAISING with a train on the island is wrong side */
}

static void test_no_warning(void) {
    lg_metrics_t m;

    lg_metrics_init(&m, 0);
    lg_metrics_tick(&m, 0, LG_GATE_UP, 0, 0, 0);
    lg_metrics_tick(&m, 100, LG_GATE_DOWN, 0, 1, 0); /* lights off, train arrives */
    CHECK(m.trains == 1 && m.trains_without_warning == 1 && m.warning.count == 0);
}

static void test_boot_closure_not_counted(void) {
    lg_metrics_t m;

    /* The gate boots DOWN; opening it for the first time is not a per-train closure. */
    lg_metrics_init(&m, 0);
    lg_metrics_tick(&m, 0, LG_GATE_DOWN, 1, 0, 0);
    lg_metrics_tick(&m, 5000, LG_GATE_RAISING, 0, 0, 0);
    lg_metrics_tick(&m, 6000, LG_GATE_UP, 0, 0, 0);
    CHECK(m.closure.count == 0);
    CHECK(m.closed_ms == 6000); /* but the road was blocked, and that is counted */
}

static void test_deadlines(void) {
    lg_metrics_t m;
    uint64_t ms = 1000000u;

    CHECK(!lg_tick_late(0, 10 * ms));
    CHECK(!lg_tick_late(0, (LG_TICK_MS + LG_TICK_TOLERANCE_MS) * ms));      /* exactly at the limit */
    CHECK(lg_tick_late(0, (LG_TICK_MS + LG_TICK_TOLERANCE_MS) * ms + 1));   /* one ns later */
    CHECK(lg_tick_late(5 * ms, 25 * ms));

    lg_metrics_init(&m, 0);
    lg_metrics_tick(&m, 10, LG_GATE_UP, 0, 0, 0);
    lg_metrics_tick(&m, 20, LG_GATE_UP, 0, 0, 1);
    lg_metrics_tick(&m, 40, LG_GATE_UP, 0, 0, 1);
    CHECK(m.deadline_misses == 2);
}

static void test_recovery(void) {
    lg_metrics_t m;

    lg_metrics_init(&m, 0);
    lg_metrics_fault_end(&m, 100); /* no open fault: ignored */
    CHECK(m.recovery.count == 0);

    lg_metrics_fault_begin(&m, 1000);
    lg_metrics_fault_begin(&m, 1200); /* already open: the first start stays */
    lg_metrics_fault_end(&m, 1300);
    CHECK(m.recovery.count == 1 && m.recovery.last_ms == 300);

    lg_metrics_fault_begin(&m, 5000);
    lg_metrics_fault_end(&m, 5800);
    CHECK(m.recovery.count == 2 && m.recovery.last_ms == 800 && m.recovery.max_ms == 800 &&
          m.recovery.sum_ms == 1100);
}

static void test_edges(void) {
    lg_metrics_t m;
    uint32_t t0 = 0xFFFFFFFFu - 500u;

    lg_metrics_init(&m, 0);
    CHECK(lg_metrics_closure_ms_per_hour(&m) == 0); /* nothing observed */

    /* The 32-bit millisecond counter wraps; closure time must still add up. */
    lg_metrics_init(&m, t0);
    lg_metrics_tick(&m, t0, LG_GATE_UP, 0, 0, 0);
    lg_metrics_tick(&m, t0 + 100, LG_GATE_DOWN, 1, 0, 0);
    lg_metrics_tick(&m, t0 + 1100, LG_GATE_UP, 0, 0, 0); /* crosses zero */
    CHECK(m.closed_ms == 1000); /* DOWN from t0+100 to t0+1100 */
    CHECK(m.closure.count == 1 && m.closure.last_ms == 1000);
}

static void test_health_and_timetable_stubs(void) {
    lg_tt_window_t w;

    CHECK(lg_health_verdict_from_score(100) == LG_VERDICT_OK);
    CHECK(lg_health_verdict_from_score(LG_SCORE_OK_MIN) == LG_VERDICT_OK);
    CHECK(lg_health_verdict_from_score(LG_SCORE_OK_MIN - 1) == LG_VERDICT_WARNING);
    CHECK(lg_health_verdict_from_score(LG_SCORE_WARNING_MIN) == LG_VERDICT_WARNING);
    CHECK(lg_health_verdict_from_score(LG_SCORE_WARNING_MIN - 1) == LG_VERDICT_FAILED);
    CHECK(lg_health_verdict_from_score(0) == LG_VERDICT_FAILED);

    w = lg_timetable_window(LG_DIR_WEST, 1234);
    CHECK(!w.valid);
}

int main(void) {
    test_normal_cycle();
    test_two_trains_statistics();
    test_wrong_side_detected();
    test_no_warning();
    test_boot_closure_not_counted();
    test_deadlines();
    test_recovery();
    test_edges();
    test_health_and_timetable_stubs();
    LG_TEST_DONE("test_metrics");
}
