#include "lg_bridge_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lg_proto.h"

/* ---- key=value lookups ------------------------------------------------------------------------ */

/* Start of the value of key (whole word, followed by '='), or NULL. */
static const char *kv_find(const char *text, const char *key) {
    size_t klen;
    const char *p;

    if (text == NULL || key == NULL || key[0] == '\0') {
        return NULL;
    }
    klen = strlen(key);
    for (p = text; (p = strstr(p, key)) != NULL; p++) {
        int start_ok = (p == text) || p[-1] == ' ' || p[-1] == '\n' || p[-1] == '\t';

        if (start_ok && p[klen] == '=') {
            return p + klen + 1;
        }
    }
    return NULL;
}

static int is_end(char c) {
    return c == '\0' || c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == ',';
}

int lg_bridge_kv_u32(const char *text, const char *key, uint32_t *out) {
    const char *v = kv_find(text, key);
    uint64_t acc = 0;
    int base = 10;
    int digits = 0;

    if (v == NULL || out == NULL) {
        return -1;
    }
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        base = 16;
        v += 2;
    }
    for (; !is_end(*v); v++) {
        int d;

        if (*v >= '0' && *v <= '9') {
            d = *v - '0';
        } else if (base == 16 && *v >= 'a' && *v <= 'f') {
            d = *v - 'a' + 10;
        } else if (base == 16 && *v >= 'A' && *v <= 'F') {
            d = *v - 'A' + 10;
        } else {
            return -1;
        }
        acc = acc * (uint64_t)base + (uint64_t)d;
        if (acc > 0xFFFFFFFFull) {
            return -1;
        }
        digits++;
    }
    if (digits == 0) {
        return -1;
    }
    *out = (uint32_t)acc;
    return 0;
}

int lg_bridge_kv_i32(const char *text, const char *key, int32_t *out) {
    const char *v = kv_find(text, key);
    int64_t acc = 0;
    int neg = 0;
    int digits = 0;

    if (v == NULL || out == NULL) {
        return -1;
    }
    if (*v == '-') {
        neg = 1;
        v++;
    }
    for (; !is_end(*v); v++) {
        if (*v < '0' || *v > '9') {
            return -1;
        }
        acc = acc * 10 + (*v - '0');
        if (acc > 2147483648LL) {
            return -1;
        }
        digits++;
    }
    if (digits == 0 || (!neg && acc > 2147483647LL)) {
        return -1;
    }
    *out = (int32_t)(neg ? -acc : acc);
    return 0;
}

int lg_bridge_kv_str(const char *text, const char *key, char *out, size_t len) {
    const char *v = kv_find(text, key);
    size_t n = 0;

    if (v == NULL || out == NULL || len == 0) {
        return -1;
    }
    /* A string token runs to the next blank or line end (commas belong to it: "active=1,0"). */
    while (v[n] != '\0' && v[n] != ' ' && v[n] != '\n' && v[n] != '\r' && v[n] != '\t') {
        n++;
    }
    if (n == 0 || n >= len) {
        return -1;
    }
    memcpy(out, v, n);
    out[n] = '\0';
    return 0;
}

/* ---- device texts ------------------------------------------------------------------------------ */

int lg_bridge_from_controller(lg_bridge_state_t *s, const char *text) {
    const char *a;

    if (lg_bridge_kv_u32(text, "level", &s->level) != 0) {
        s->ctl_ok = 0;
        return -1;
    }
    (void)lg_bridge_kv_u32(text, "lights", &s->lights);
    (void)lg_bridge_kv_u32(text, "bell", &s->bell);
    (void)lg_bridge_kv_u32(text, "tickets", &s->tickets);
    (void)lg_bridge_kv_u32(text, "stalls", &s->stalls);
    (void)lg_bridge_kv_u32(text, "sensor_failed", &s->sensor_failed);
    (void)lg_bridge_kv_u32(text, "events", &s->events);
    a = kv_find(text, "active");
    if (a != NULL && (a[0] == '0' || a[0] == '1') && a[1] == ',' && (a[2] == '0' || a[2] == '1')) {
        s->active_west = (uint32_t)(a[0] - '0');
        s->active_east = (uint32_t)(a[2] - '0');
    }
    s->ctl_ok = 1;
    return 0;
}

int lg_bridge_from_gate(lg_bridge_state_t *s, const char *text) {
    if (lg_bridge_kv_str(text, "state", s->gate_state, sizeof s->gate_state) != 0 ||
        lg_bridge_kv_i32(text, "position", &s->gate_position) != 0) {
        s->gate_ok = 0;
        return -1;
    }
    (void)lg_bridge_kv_u32(text, "watchdog_trips", &s->watchdog_trips);
    s->gate_ok = 1;
    return 0;
}

int lg_bridge_from_plant(lg_bridge_state_t *s, const char *text) {
    const char *line;

    s->n_trains = 0;
    if (lg_bridge_kv_u32(text, "t_ms", &s->plant_ms) != 0) {
        s->plant_ok = 0;
        return -1;
    }
    (void)lg_bridge_kv_u32(text, "island", &s->island);
    for (line = text; line != NULL && *line != '\0'; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        char one[128];
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        lg_bridge_train_t t;
        uint32_t id = 0;
        char dir[8];

        if (strncmp(line, "train ", 6) != 0 || n >= sizeof one) {
            continue;
        }
        memcpy(one, line, n);
        one[n] = '\0';
        memset(&t, 0, sizeof t);
        if (lg_bridge_kv_u32(one, "id", &id) != 0 || id > 0xFFFFu || lg_bridge_kv_str(one, "dir", dir, sizeof dir) != 0 ||
            lg_bridge_kv_i32(one, "front_mm", &t.front_mm) != 0 || lg_bridge_kv_i32(one, "rear_mm", &t.rear_mm) != 0) {
            continue;
        }
        if (strcmp(dir, "west") == 0) {
            t.dir = LG_DIR_WEST;
        } else if (strcmp(dir, "east") == 0) {
            t.dir = LG_DIR_EAST;
        } else {
            continue;
        }
        t.id = (uint16_t)id;
        if (s->n_trains < LG_BRIDGE_MAX_TRAINS) {
            s->trains[s->n_trains++] = t;
        }
    }
    s->plant_ok = 1;
    return 0;
}

int lg_bridge_parse_event(const char *line, lg_bridge_event_t *ev) {
    char kind[12];

    if (line == NULL || ev == NULL) {
        return -1;
    }
    memset(ev, 0, sizeof *ev);
    if (lg_bridge_kv_u32(line, "seq", &ev->seq) != 0 || lg_bridge_kv_u32(line, "t_ms", &ev->t_ms) != 0 ||
        lg_bridge_kv_str(line, "kind", kind, sizeof kind) != 0) {
        return -1;
    }
    (void)lg_bridge_kv_str(line, "severity", ev->severity, sizeof ev->severity);
    if (strcmp(kind, "LEVEL") == 0) {
        if (lg_bridge_kv_u32(line, "from", &ev->from) != 0 || lg_bridge_kv_u32(line, "to", &ev->to) != 0 ||
            lg_bridge_kv_u32(line, "reason", &ev->reason) != 0) {
            return -1;
        }
        ev->kind = LG_BRIDGE_EV_LEVEL;
        return 0;
    }
    if (strcmp(kind, "NOTIFY") == 0) {
        if (lg_bridge_kv_str(line, "sensor", ev->sensor, sizeof ev->sensor) != 0 ||
            lg_bridge_kv_str(line, "diag", ev->diag, sizeof ev->diag) != 0) {
            return -1;
        }
        (void)lg_bridge_kv_u32(line, "level", &ev->to);
        if (lg_bridge_kv_str(line, "to", ev->target, sizeof ev->target) != 0) {
            memcpy(ev->target, "maintenance", sizeof "maintenance"); /* a line from before Step 12 */
        } else if (strcmp(ev->target, "maintenance") != 0 && strcmp(ev->target, "station") != 0 &&
                   strcmp(ev->target, "guard") != 0 && strcmp(ev->target, "road_sign") != 0) {
            return -1; /* nobody to route it to: not an event line */
        }
        if (lg_bridge_kv_str(line, "dir", ev->dir, sizeof ev->dir) != 0 ||
            (strcmp(ev->dir, "west") != 0 && strcmp(ev->dir, "east") != 0)) {
            memcpy(ev->dir, "none", sizeof "none");
        }
        if (lg_bridge_kv_str(line, "component", ev->component, sizeof ev->component) != 0) {
            memcpy(ev->component, ev->sensor, sizeof ev->component); /* a Step 9/10 line */
        }
        {
            char t[16];
            char *end = NULL;
            long v;

            if (lg_bridge_kv_str(line, "trend_s", t, sizeof t) == 0) {
                v = strtol(t, &end, 10);
                if (end != t && *end == '\0' && v >= 0 && v <= 0x7FFFFFFFL) {
                    ev->trend_s = (int32_t)v;
                    ev->has_trend = 1;
                }
            }
        }
        ev->kind = LG_BRIDGE_EV_NOTIFY;
        return 0;
    }
    if (strcmp(kind, "METRIC") == 0) {
        char t[24];
        char *end = NULL;
        long long v;

        if (lg_bridge_kv_str(line, "name", ev->name, sizeof ev->name) != 0 ||
            lg_bridge_kv_str(line, "component", ev->component, sizeof ev->component) != 0 ||
            lg_bridge_kv_str(line, "value", t, sizeof t) != 0) {
            return -1;
        }
        v = strtoll(t, &end, 10);
        if (end == t || *end != '\0') {
            return -1;
        }
        ev->value = (int64_t)v;
        ev->kind = LG_BRIDGE_EV_METRIC;
        return 0;
    }
    return -1;
}

/* ---- JSON output ------------------------------------------------------------------------------- */

/* The text of a station, guard or road sign notification (Step 12, README 3 level 3 actions). */
static void people_text(const lg_bridge_event_t *ev, char *text, size_t len) {
    const char *d = ev->dir[0] != '\0' && strcmp(ev->dir, "none") != 0 ? ev->dir : "one direction";
    int closed = strcmp(ev->diag, "NO_SCHEDULE") == 0 || strcmp(ev->diag, "CROSSING_CLOSED") == 0;

    if (strcmp(ev->target, "road_sign") == 0) {
        (void)snprintf(text, len, "%s", strcmp(ev->diag, "DETECTION_LOST") == 0 ? "CROSSING FAULT - TAKE CARE"
                                        : closed                                ? "CROSSING CLOSED"
                                                                                : "sign off");
    } else if (strcmp(ev->diag, "DETECTION_LOST") == 0) {
        if (strcmp(ev->target, "station") == 0) {
            (void)snprintf(text, len, "Crossing degraded, %s detection lost: drivers slow down and sound the horn", d);
        } else {
            (void)snprintf(text, len, "Crossing degraded, %s detection lost: guard needed at the crossing", d);
        }
    } else if (closed) {
        (void)snprintf(text, len, "Crossing in fail-safe (%s), gate held down: guard acknowledgement needed",
                       strcmp(ev->diag, "NO_SCHEDULE") == 0 ? "no schedule information" : "fault");
    } else if (strcmp(ev->diag, "TRAIN_OVERDUE") == 0) {
        (void)snprintf(text, len, "Expected %s train overdue: confirm the crossing clear", d);
    } else if (strcmp(ev->diag, "OK") == 0) {
        (void)snprintf(text, len, "Crossing detection back to normal");
    } else {
        (void)snprintf(text, len, "Crossing notice: %s", ev->diag);
    }
}

typedef struct {
    char *buf;
    size_t len;
    size_t used;
    int overflow;
} jw_t;

static void jw_raw(jw_t *w, const char *s) {
    size_t n = strlen(s);

    if (w->overflow || w->used + n >= w->len) {
        w->overflow = 1;
        return;
    }
    memcpy(w->buf + w->used, s, n + 1u);
    w->used += n;
}

static void jw_fmt_u(jw_t *w, const char *key, uint32_t v) {
    char t[48];

    (void)snprintf(t, sizeof t, ",\"%s\":%u", key, (unsigned)v);
    jw_raw(w, t);
}

static void jw_fmt_i(jw_t *w, const char *key, int64_t v) {
    char t[48];

    (void)snprintf(t, sizeof t, ",\"%s\":%lld", key, (long long)v);
    jw_raw(w, t);
}

/* A JSON string value: quotes and backslashes escaped, control characters dropped. */
static void jw_str(jw_t *w, const char *key, const char *v) {
    char t[160];
    size_t n = 0;
    size_t i;

    (void)snprintf(t, sizeof t, ",\"%s\":\"", key);
    jw_raw(w, t);
    for (i = 0; v != NULL && v[i] != '\0' && n + 3 < sizeof t; i++) {
        unsigned char c = (unsigned char)v[i];

        if (c < 0x20) {
            continue;
        }
        if (c >= 0x80) {
            c = '?'; /* the line stays ASCII: a raw byte here would not be valid UTF-8 for the host */
        }
        if (c == '"' || c == '\\') {
            t[n++] = '\\';
        }
        t[n++] = (char)c;
    }
    t[n] = '\0';
    jw_raw(w, t);
    jw_raw(w, "\"");
}

static void jw_time(jw_t *w, uint32_t t_ms) {
    char t[40];

    (void)snprintf(t, sizeof t, "{\"t\":%u.%03u", (unsigned)(t_ms / 1000u), (unsigned)(t_ms % 1000u));
    jw_raw(w, t);
}

static size_t jw_end(jw_t *w) {
    jw_raw(w, "}\n");
    if (w->overflow) {
        if (w->len > 0) {
            w->buf[0] = '\0';
        }
        return 0;
    }
    return w->used;
}

static jw_t jw_begin(char *buf, size_t len) {
    jw_t w;

    w.buf = buf;
    w.len = buf == NULL ? 0 : len;
    w.used = 0;
    w.overflow = (buf == NULL || len == 0);
    if (!w.overflow) {
        buf[0] = '\0';
    }
    return w;
}

size_t lg_bridge_state_json(const lg_bridge_state_t *s, char *buf, size_t len) {
    jw_t w = jw_begin(buf, len);
    uint32_t i;
    char t[24];

    jw_time(&w, s->t_ms);
    jw_str(&w, "kind", "state");
    jw_fmt_u(&w, "stale", s->stale);
    jw_fmt_u(&w, "ctl_ok", s->ctl_ok);
    jw_fmt_u(&w, "gate_ok", s->gate_ok);
    jw_fmt_u(&w, "plant_ok", s->plant_ok);
    /* Unknown is null, never 0: 0 would read as level 0 / gate at 0 degrees (UP). Review finding M1. */
    if (s->ctl_ok) {
        jw_fmt_u(&w, "level", s->level);
    } else {
        jw_raw(&w, ",\"level\":null");
    }
    if (s->gate_ok) {
        jw_str(&w, "gate", s->gate_state);
        jw_fmt_i(&w, "gate_position", s->gate_position);
    } else {
        jw_raw(&w, ",\"gate\":null,\"gate_position\":null");
    }
    jw_fmt_u(&w, "lights", s->lights);
    jw_fmt_u(&w, "bell", s->bell);
    (void)snprintf(t, sizeof t, ",\"active\":[%u,%u]", (unsigned)(s->active_west != 0), (unsigned)(s->active_east != 0));
    jw_raw(&w, t);
    jw_fmt_u(&w, "sensor_failed", s->sensor_failed);
    jw_fmt_u(&w, "tickets", s->tickets);
    jw_fmt_u(&w, "events", s->events);
    jw_fmt_u(&w, "stalls", s->stalls);
    jw_fmt_u(&w, "watchdog_trips", s->watchdog_trips);
    jw_fmt_u(&w, "plant_ms", s->plant_ms);
    jw_fmt_u(&w, "island", s->island);
    jw_raw(&w, ",\"trains\":[");
    for (i = 0; i < s->n_trains && i < LG_BRIDGE_MAX_TRAINS; i++) {
        char tr[112];

        (void)snprintf(tr, sizeof tr, "%s{\"id\":%u,\"dir\":\"%s\",\"front_mm\":%ld,\"rear_mm\":%ld}", i ? "," : "",
                       (unsigned)s->trains[i].id, s->trains[i].dir == LG_DIR_WEST ? "west" : "east",
                       (long)s->trains[i].front_mm, (long)s->trains[i].rear_mm);
        jw_raw(&w, tr);
    }
    jw_raw(&w, "]");
    return jw_end(&w);
}

const char *lg_bridge_reason_name(uint32_t reason) {
    static const char *names[] = {"NONE",          "BOOT",          "BOOT_RELEASED",        "WARNING",
                                  "SENSOR_FAILED", "PROC_RESTART",  "PROC_DEAD",            "DETECT_LOST_SCHEDULE",
                                  "DETECT_LOST_NO_INFO", "CRITICAL_PROC_DEAD", "GATE_FAILED", "SELF_CHECK",
                                  "STABLE",        "GUARD_ACK"};

    return reason < sizeof names / sizeof names[0] ? names[reason] : "UNKNOWN";
}

size_t lg_bridge_event_json(const lg_bridge_event_t *ev, char *buf, size_t len) {
    jw_t w = jw_begin(buf, len);

    jw_time(&w, ev->t_ms);
    jw_fmt_u(&w, "seq", ev->seq);
    if (ev->kind == LG_BRIDGE_EV_LEVEL) {
        jw_str(&w, "kind", "level");
        jw_fmt_u(&w, "from", ev->from);
        jw_fmt_u(&w, "to", ev->to);
        jw_str(&w, "reason", lg_bridge_reason_name(ev->reason));
    } else if (ev->kind == LG_BRIDGE_EV_METRIC) {
        jw_str(&w, "kind", "metric");
        jw_str(&w, "name", ev->name);
        jw_str(&w, "component", ev->component);
        jw_fmt_i(&w, "value", ev->value);
    } else {
        char sev[12];
        char text[96];
        size_t i;

        for (i = 0; i + 1 < sizeof sev && ev->severity[i] != '\0'; i++) {
            char c = ev->severity[i];

            sev[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        sev[i] = '\0';
        if (ev->target[0] != '\0' && strcmp(ev->target, "maintenance") != 0) {
            /* Step 12: station, guard, road sign. */
            jw_str(&w, "kind", "notify");
            jw_str(&w, "to", ev->target);
            jw_str(&w, "severity", sev[0] ? sev : "info");
            jw_str(&w, "dir", ev->dir[0] ? ev->dir : "none");
            jw_str(&w, "diag", ev->diag);
            people_text(ev, text, sizeof text);
            jw_str(&w, "text", text);
            return jw_end(&w);
        }
        jw_str(&w, "kind", "notify");
        jw_str(&w, "to", "maintenance");
        jw_str(&w, "severity", sev[0] ? sev : "info");
        jw_str(&w, "sensor", ev->sensor);
        jw_str(&w, "component", ev->component[0] ? ev->component : ev->sensor);
        jw_str(&w, "diag", ev->diag);
        if (ev->has_trend) {
            jw_fmt_i(&w, "trend_s", ev->trend_s);
        }
        {
            const char *who = ev->component[0] ? ev->component : ev->sensor;
            const char *what = strcmp(who, "gate") == 0 || strcmp(who, "battery") == 0 ? "" : "Sensor ";
            char cap[20];

            /* "Gate", "Battery", "Sensor west/a" */
            (void)snprintf(cap, sizeof cap, "%s", who);
            if (what[0] == '\0' && cap[0] >= 'a' && cap[0] <= 'z') {
                cap[0] = (char)(cap[0] - 'a' + 'A');
            }
            if (strcmp(ev->diag, "OK") == 0) {
                (void)snprintf(text, sizeof text, "%s%s is working again", what, cap);
            } else if (ev->has_trend) {
                (void)snprintf(text, sizeof text, "%s%s: %s, %s maintenance, about %d s to failure", what, cap,
                               ev->diag, sev[0] ? sev : "info", (int)ev->trend_s);
            } else {
                (void)snprintf(text, sizeof text, "%s%s: %s, %s maintenance", what, cap, ev->diag,
                               sev[0] ? sev : "info");
            }
        }
        jw_str(&w, "text", text);
    }
    return jw_end(&w);
}

size_t lg_bridge_metric_json(uint32_t t_ms, const char *name, int64_t value, char *buf, size_t len) {
    jw_t w = jw_begin(buf, len);

    jw_time(&w, t_ms);
    jw_str(&w, "kind", "metric");
    jw_str(&w, "name", name);
    jw_fmt_i(&w, "value", value);
    return jw_end(&w);
}

size_t lg_bridge_reply_json(int ok, const char *what, const char *reason, char *buf, size_t len) {
    jw_t w = jw_begin(buf, len);

    jw_raw(&w, ok ? "{\"kind\":\"ack\"" : "{\"kind\":\"error\"");
    jw_str(&w, "what", what);
    if (reason != NULL) {
        jw_str(&w, "reason", reason);
    }
    return jw_end(&w);
}

/* ---- inbound ----------------------------------------------------------------------------------- */

static void reject(lg_bridge_in_t *out, const char *why) {
    out->kind = LG_BRIDGE_IN_REJECT;
    (void)snprintf(out->reason, sizeof out->reason, "%s", why);
}

void lg_bridge_parse_in(lg_json_doc_t *doc, const char *line, size_t len, lg_bridge_in_t *out) {
    const lg_json_node_t *root;
    const char *kind = NULL;
    const char *dir = NULL;
    int64_t eta = 0;

    memset(out, 0, sizeof *out);
    if (line == NULL || doc == NULL) {
        reject(out, "not_json");
        return;
    }
    if (len > LG_BRIDGE_IN_MAX) {
        reject(out, "too_long");
        return;
    }
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }
    if (len == 0 || lg_json_parse(doc, line, len) != LG_JSON_OK || (root = lg_json_root(doc)) == NULL) {
        reject(out, "not_json");
        return;
    }
    if (root->type != LG_JSON_OBJECT) {
        reject(out, "not_object");
        return;
    }
    if (lg_json_get_str(doc, root, "kind", &kind) != 0) {
        reject(out, "unknown_kind");
        return;
    }
    if (strcmp(kind, "subscribe") == 0) {
        if (root->count != 1) {
            reject(out, "extra_field");
            return;
        }
        out->kind = LG_BRIDGE_IN_SUBSCRIBE;
        return;
    }
    if (strcmp(kind, "station_departure") == 0) {
        if (lg_json_get_str(doc, root, "dir", &dir) != 0 || lg_json_get_milli(doc, root, "eta_s", &eta) != 0) {
            reject(out, "bad_field");
            return;
        }
        if (root->count != 3) {
            reject(out, "extra_field");
            return;
        }
        if (strcmp(dir, "west") == 0) {
            out->dir = LG_DIR_WEST;
        } else if (strcmp(dir, "east") == 0) {
            out->dir = LG_DIR_EAST;
        } else {
            reject(out, "bad_field");
            return;
        }
        if (eta < 0 || eta > 86400000LL || eta % 1000 != 0) {
            reject(out, "bad_field");
            return;
        }
        out->eta_s = (uint32_t)(eta / 1000);
        out->kind = LG_BRIDGE_IN_STATION_DEPARTURE;
        return;
    }
    reject(out, "unknown_kind");
}

/* ---- recent event lines ------------------------------------------------------------------------ */

void lg_bridge_recent_init(lg_bridge_recent_t *r) {
    r->count = 0;
    r->next = 0;
}

void lg_bridge_recent_push(lg_bridge_recent_t *r, const char *line) {
    size_t n = strlen(line);

    if (n == 0 || n >= LG_BRIDGE_LINE_MAX) {
        return; /* never store half a line */
    }
    memcpy(r->line[r->next], line, n + 1);
    r->next = (r->next + 1) % LG_BRIDGE_RECENT;
    if (r->count < LG_BRIDGE_RECENT) {
        r->count++;
    }
}

const char *lg_bridge_recent_get(const lg_bridge_recent_t *r, uint32_t i) {
    if (i >= r->count) {
        return NULL;
    }
    /* the oldest stored line sits at next - count (mod size) */
    return r->line[(r->next + LG_BRIDGE_RECENT - r->count + i) % LG_BRIDGE_RECENT];
}

uint32_t lg_bridge_recent_select(const lg_bridge_recent_t *r, size_t budget) {
    uint32_t k = 0;
    size_t used = 0;

    while (k < r->count) {
        size_t n = strlen(lg_bridge_recent_get(r, r->count - 1 - k));

        if (n > budget - used) {
            break;
        }
        used += n;
        k++;
    }
    return k;
}

/* ---- new events of a ring read ----------------------------------------------------------------- */

void lg_bridge_evtrack_init(lg_bridge_evtrack_t *k) {
    memset(k, 0, sizeof *k);
}

uint32_t lg_bridge_evtrack_first_new(lg_bridge_evtrack_t *k, const lg_bridge_event_t *ev, uint32_t n) {
    uint32_t first = 0;
    uint32_t i;

    if (n == 0) {
        return 0; /* an empty ring (a controller just restarted): keep what was sent */
    }
    if (k->have) {
        for (i = 0; i < n; i++) {
            if (ev[i].seq == k->last_seq && ev[i].t_ms == k->last_t_ms) {
                first = i + 1; /* the same controller: everything after the last event sent */
                break;
            }
        }
    }
    k->have = 1;
    k->last_seq = ev[n - 1].seq;
    k->last_t_ms = ev[n - 1].t_ms;
    return first;
}
