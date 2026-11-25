/* Step 12: station, guard and road sign notifications through /dev/lg/events and the bridge JSON. */

#include <string.h>

#include "lg_bridge_proto.h"
#include "lg_controller_logic.h"
#include "lg_test.h"

static void event_line(uint8_t target, uint8_t code, uint8_t dir, uint8_t sev, char *buf, size_t len) {
    lg_ctl_event_t e;

    memset(&e, 0, sizeof e);
    e.seq = 7;
    e.t_ms = 123456;
    e.kind = LG_EVT_NOTIFY;
    e.severity = sev;
    e.level_from = LG_LEVEL_UNCERTAIN;
    e.level_to = LG_LEVEL_UNCERTAIN;
    e.dir = dir;
    e.id = LG_CTL_EVENT_NO_SENSOR;
    e.comp = LG_CTL_EVENT_NO_SENSOR;
    e.code = code;
    e.value = -1;
    e.target = target;
    (void)lg_ctl_event_text(&e, buf, len);
}

static int json_of(const char *line, char *json, size_t len) {
    lg_bridge_event_t ev;

    if (lg_bridge_parse_event(line, &ev) != 0) {
        return -1;
    }
    return lg_bridge_event_json(&ev, json, len) > 0 ? 0 : -1;
}

static void test_targets(void) {
    char line[LG_CTL_EVENT_TEXT_MAX];
    char json[512];

    event_line(LG_CTL_TO_STATION, LG_CTL_DIAG_DETECTION_LOST, LG_DIR_WEST, LG_SEV_URGENT, line, sizeof line);
    CHECK(strstr(line, "kind=NOTIFY severity=URGENT to=station sensor=none diag=DETECTION_LOST level=3") != NULL);
    CHECK(json_of(line, json, sizeof json) == 0);
    CHECK(strstr(json, "\"kind\":\"notify\",\"to\":\"station\",\"severity\":\"urgent\",\"dir\":\"west\","
                       "\"diag\":\"DETECTION_LOST\"") != NULL);
    CHECK(strstr(json, "west detection lost: drivers slow down and sound the horn") != NULL);
    CHECK(strstr(json, "\"seq\":7") != NULL);

    event_line(LG_CTL_TO_GUARD, LG_CTL_DIAG_DETECTION_LOST, LG_DIR_EAST, LG_SEV_URGENT, line, sizeof line);
    CHECK(json_of(line, json, sizeof json) == 0 && strstr(json, "\"to\":\"guard\"") != NULL &&
          strstr(json, "east detection lost: guard needed") != NULL);

    event_line(LG_CTL_TO_ROAD_SIGN, LG_CTL_DIAG_DETECTION_LOST, LG_DIR_WEST, LG_SEV_WARNING, line, sizeof line);
    CHECK(json_of(line, json, sizeof json) == 0 && strstr(json, "\"text\":\"CROSSING FAULT - TAKE CARE\"") != NULL);

    event_line(LG_CTL_TO_ROAD_SIGN, LG_CTL_DIAG_NO_SCHEDULE, LG_DIR_WEST, LG_SEV_URGENT, line, sizeof line);
    CHECK(json_of(line, json, sizeof json) == 0 && strstr(json, "\"text\":\"CROSSING CLOSED\"") != NULL);

    event_line(LG_CTL_TO_STATION, LG_CTL_DIAG_NO_SCHEDULE, LG_DIR_WEST, LG_SEV_URGENT, line, sizeof line);
    CHECK(json_of(line, json, sizeof json) == 0 && strstr(json, "no schedule information") != NULL);

    event_line(LG_CTL_TO_GUARD, LG_CTL_DIAG_TRAIN_OVERDUE, LG_DIR_WEST, LG_SEV_WARNING, line, sizeof line);
    CHECK(json_of(line, json, sizeof json) == 0 && strstr(json, "Expected west train overdue") != NULL);

    event_line(LG_CTL_TO_ROAD_SIGN, LG_CTL_DIAG_NONE, LG_DIR_WEST, LG_SEV_INFO, line, sizeof line);
    CHECK(json_of(line, json, sizeof json) == 0 && strstr(json, "\"text\":\"sign off\"") != NULL);

    event_line(LG_CTL_TO_STATION, LG_CTL_DIAG_DETECTION_LOST, LG_CTL_EVENT_NO_SENSOR, LG_SEV_URGENT, line,
               sizeof line);
    CHECK(strstr(line, " dir=none\n") != NULL);
    CHECK(json_of(line, json, sizeof json) == 0 && strstr(json, "\"dir\":\"none\"") != NULL &&
          strstr(json, "one direction detection lost") != NULL);
}

static void test_compat_and_reject(void) {
    lg_bridge_event_t ev;
    char json[512];

    /* A maintenance line keeps its old JSON shape. */
    CHECK(lg_bridge_parse_event("seq=2 t_ms=10 kind=NOTIFY severity=URGENT to=maintenance sensor=west/a "
                                "diag=MISSED_TRAIN level=0 component=west/a trend_s=-1",
                                &ev) == 0);
    CHECK(strcmp(ev.target, "maintenance") == 0);
    CHECK(lg_bridge_event_json(&ev, json, sizeof json) > 0);
    CHECK(strstr(json, "\"to\":\"maintenance\",\"severity\":\"urgent\",\"sensor\":\"west/a\"") != NULL);
    /* A Step 9 line without to= is maintenance. */
    CHECK(lg_bridge_parse_event("seq=2 t_ms=10 kind=NOTIFY severity=URGENT sensor=west/a diag=SILENT level=0", &ev) ==
          0);
    CHECK(strcmp(ev.target, "maintenance") == 0);
    /* An unknown target is not an event line. */
    CHECK(lg_bridge_parse_event("seq=2 t_ms=10 kind=NOTIFY severity=URGENT to=gate sensor=none diag=OK level=0",
                                &ev) == -1);
    /* A level line still reads to= as the new level. */
    CHECK(lg_bridge_parse_event("seq=3 t_ms=10 kind=LEVEL severity=WARNING from=2 to=3 reason=7", &ev) == 0);
    CHECK(ev.kind == LG_BRIDGE_EV_LEVEL && ev.to == 3 && ev.reason == 7);
    CHECK(lg_bridge_event_json(&ev, json, sizeof json) > 0 && strstr(json, "DETECT_LOST_SCHEDULE") != NULL);
}

int main(void) {
    test_targets();
    test_compat_and_reject();
    LG_TEST_DONE("test_bridge_level3");
}
