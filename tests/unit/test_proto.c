#include <stddef.h>
#include <stdio.h>

#include "lg_config.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_test.h"

int main(void) {
    /* Every message starts with the header. */
    CHECK(offsetof(lg_sensor_sample_t, hdr) == 0);
    CHECK(offsetof(lg_gate_cmd_msg_t, hdr) == 0);
    CHECK(offsetof(lg_gate_state_msg_t, hdr) == 0);
    CHECK(offsetof(lg_health_verdict_t, hdr) == 0);
    CHECK(offsetof(lg_proc_status_t, hdr) == 0);
    CHECK(offsetof(lg_sched_query_t, hdr) == 0);
    CHECK(offsetof(lg_sched_reply_t, hdr) == 0);
    CHECK(offsetof(lg_event_t, hdr) == 0);
    CHECK(offsetof(lg_guard_ack_t, hdr) == 0);
    CHECK(offsetof(lg_ack_t, hdr) == 0);

    CHECK(LG_PROTO_VERSION == 1);

    /* Sensor and process numbering used by the README mapping. */
    CHECK(LG_DIR_COUNT * LG_SENSOR_ID_COUNT == LG_COMP_SENSOR_LAST + 1);
    CHECK(LG_PROC_SENSOR_BASE + LG_DIR_COUNT * LG_SENSOR_ID_COUNT == LG_PROC_PLANT);

    /* Priorities keep the order from README 4.2. */
    CHECK(LG_PRIO_SUPERVISOR > LG_PRIO_CONTROLLER);
    CHECK(LG_PRIO_CONTROLLER > LG_PRIO_GATE);
    CHECK(LG_PRIO_GATE > LG_PRIO_SENSOR);
    CHECK(LG_PRIO_SENSOR > LG_PRIO_PLANT);
    CHECK(LG_PRIO_PLANT > LG_PRIO_HEALTH);
    CHECK(LG_PRIO_HEALTH > LG_PRIO_SCHEDULE);
    CHECK(LG_PRIO_SCHEDULE > LG_PRIO_BRIDGE);

    /* Timing relations the safety argument relies on. */
    CHECK(LG_GATE_WATCHDOG_MS == 100);
    CHECK(LG_HEARTBEAT_TIMEOUT_MS > LG_HEARTBEAT_PERIOD_MS);
    CHECK(LG_TICK_MS < LG_GATE_WATCHDOG_MS);
    CHECK(LG_SCORE_OK_MIN > LG_SCORE_WARNING_MIN);

    LG_TEST_DONE("test_proto");
}
