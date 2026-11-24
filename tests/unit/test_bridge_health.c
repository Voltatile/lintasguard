/* Step 11: the controller's early-warning events through lg-bridge (text line -> event -> NDJSON). */
#include <stdio.h>
#include <string.h>

#include "lg_bridge_proto.h"
#include "lg_controller_logic.h"
#include "lg_health.h"
#include "lg_test.h"

static lg_ctl_event_t ev_of(uint8_t kind, uint8_t sev, uint8_t comp, uint8_t code, int32_t value) {
    lg_ctl_event_t e;

    memset(&e, 0, sizeof e);
    e.seq = 41;
    e.t_ms = 123456;
    e.kind = kind;
    e.severity = sev;
    e.level_to = 1;
    e.comp = comp;
    e.dir = comp <= LG_COMP_SENSOR_LAST ? (uint8_t)(comp / 3) : (uint8_t)LG_CTL_EVENT_NO_SENSOR;
    e.id = comp <= LG_COMP_SENSOR_LAST ? (uint8_t)(comp % 3) : (uint8_t)LG_CTL_EVENT_NO_SENSOR;
    e.code = code;
    e.value = value;
    return e;
}

/* Controller event -> bridge JSON line. */
static void through(const lg_ctl_event_t *e, lg_bridge_event_t *bev, char *json, size_t len) {
    char line[LG_CTL_EVENT_TEXT_MAX];
    size_t n = lg_ctl_event_text(e, line, sizeof line);

    CHECK(n > 0);
    line[n - 1] = '\0'; /* the bridge splits at the newline */
    CHECK(lg_bridge_parse_event(line, bev) == 0);
    CHECK(lg_bridge_event_json(bev, json, len) > 0);
}

int main(void) {
    lg_ctl_event_t e;
    lg_bridge_event_t b;
    char json[LG_BRIDGE_LINE_MAX];

    /* A sensor warning with a trend. */
    e = ev_of(LG_EVT_NOTIFY, LG_SEV_WARNING, 0, LG_CTL_DIAG_HEALTH_BASE + LG_DIAG_NOISE_RISING, 31);
    through(&e, &b, json, sizeof json);
    CHECK(b.kind == LG_BRIDGE_EV_NOTIFY && b.has_trend && b.trend_s == 31);
    CHECK(strstr(json, "\"kind\":\"notify\"") != NULL && strstr(json, "\"severity\":\"warning\"") != NULL);
    CHECK(strstr(json, "\"sensor\":\"west/a\"") != NULL && strstr(json, "\"component\":\"west/a\"") != NULL);
    CHECK(strstr(json, "\"diag\":\"NOISE_RISING\"") != NULL && strstr(json, "\"trend_s\":31") != NULL);
    CHECK(strstr(json, "\"text\":\"Sensor west/a: NOISE_RISING, warning maintenance, about 31 s to failure\"") != NULL);

    /* The gate: no sensor, the component carries it. */
    e = ev_of(LG_EVT_NOTIFY, LG_SEV_URGENT, LG_COMP_GATE, LG_CTL_DIAG_HEALTH_BASE + LG_DIAG_GATE_SLOW, -1);
    through(&e, &b, json, sizeof json);
    CHECK(!b.has_trend && strcmp(b.component, "gate") == 0);
    CHECK(strstr(json, "\"sensor\":\"none\"") != NULL && strstr(json, "\"component\":\"gate\"") != NULL);
    CHECK(strstr(json, "trend_s") == NULL);
    CHECK(strstr(json, "\"text\":\"Gate: GATE_SLOW, urgent maintenance\"") != NULL);

    /* Back to OK. */
    e = ev_of(LG_EVT_NOTIFY, LG_SEV_INFO, LG_COMP_BATTERY, LG_CTL_DIAG_NONE, -1);
    through(&e, &b, json, sizeof json);
    CHECK(strstr(json, "\"text\":\"Battery is working again\"") != NULL);

    /* The lead time metric. */
    e = ev_of(LG_EVT_METRIC, LG_SEV_INFO, 0, LG_CTL_METRIC_LEAD_TIME_MS, 20600);
    through(&e, &b, json, sizeof json);
    CHECK(b.kind == LG_BRIDGE_EV_METRIC && b.value == 20600 && strcmp(b.name, "lead_time_ms") == 0);
    CHECK(strstr(json, "\"seq\":41") != NULL && strstr(json, "\"kind\":\"metric\"") != NULL);
    CHECK(strstr(json, "\"name\":\"lead_time_ms\",\"component\":\"west/a\",\"value\":20600") != NULL);

    /* A Step 9/10 line without component or trend still works; a broken metric is refused. */
    CHECK(lg_bridge_parse_event("seq=2 t_ms=5 kind=NOTIFY severity=URGENT to=maintenance sensor=west/b "
                                "diag=MISSED_TRAIN level=0",
                                &b) == 0);
    CHECK(strcmp(b.component, "west/b") == 0 && !b.has_trend);
    CHECK(lg_bridge_parse_event("seq=2 t_ms=5 kind=NOTIFY severity=WARNING to=maintenance sensor=west/b "
                                "diag=X level=1 component=west/b trend_s=-1",
                                &b) == 0);
    CHECK(!b.has_trend); /* -1 = unknown: not written */
    CHECK(lg_bridge_parse_event("seq=2 t_ms=5 kind=METRIC name=lead_time_ms component=gate value=12x", &b) == -1);
    CHECK(lg_bridge_parse_event("seq=2 t_ms=5 kind=METRIC name=lead_time_ms value=12", &b) == -1);
    LG_TEST_DONE("test_bridge_health");
}
