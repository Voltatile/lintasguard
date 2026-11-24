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

    CHECK(LG_PROTO_VERSION == 5);
    CHECK(offsetof(lg_health_verdict_t, trend_s_to_failure) == 24);
    CHECK(LG_DIAG_COUNT < 0x10000); /* diagnosis_code is 16 bits */

    /* The heartbeat value is the process id: it must fit what a pulse value can hold. */
    CHECK(LG_PROC_COUNT < 256);
    CHECK(LG_PULSE_CHILD_EXIT != LG_PULSE_TICK && LG_PULSE_CHILD_EXIT != LG_PULSE_HEARTBEAT &&
          LG_PULSE_CHILD_EXIT != LG_PULSE_SHUTDOWN && LG_PULSE_CHILD_EXIT != LG_PULSE_HEALTH_RECHECK);
    CHECK(LG_SUP_START_GRACE_MS > LG_HEARTBEAT_TIMEOUT_MS);
    CHECK(LG_CTL_SUP_SILENCE_MS >= 3 * LG_SUP_STATUS_REFRESH_MS);

    /* Types must stay above the QNX reserved range (_IO_MAX = 0x1FF in sys/iomsg.h). */
    CHECK(LG_MSG_SENSOR_SAMPLE > 0x1FF);
    CHECK(LG_MSG_ACK > LG_MSG_SENSOR_SAMPLE && LG_MSG_ACK <= 0xFFFF);
    CHECK(LG_MSG_GATE_CMD != LG_MSG_GATE_STATE);
    CHECK(LG_MSG_TRUTH_QUERY > LG_MSG_ACK && LG_MSG_TRUTH_REPLY == LG_MSG_TRUTH_QUERY + 1);
    CHECK(LG_MSG_TRUTH_REPLY <= 0xFFFF);
    CHECK(offsetof(lg_truth_query_t, hdr) == 0 && offsetof(lg_truth_reply_t, hdr) == 0);
    CHECK(LG_SENSOR_THRESHOLD > LG_SENSOR_CLEAR_LEVEL && LG_SENSOR_THRESHOLD < LG_SENSOR_OCCUPIED_LEVEL);

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
