#ifndef LG_BRIDGE_PROTO_H
#define LG_BRIDGE_PROTO_H

/*
 * Host protocol of lg-bridge (README 7.6, Step 10): newline-delimited JSON over TCP. Portable, no
 * allocation, fixed buffers.
 *
 * Out (bridge to host): one JSON object per line, "t" in seconds (3 decimals):
 *   {"t":..,"kind":"state",...}   snapshot of level, gate, directions, sensors, trains (every 100 ms)
 *   {"t":..,"kind":"level","from":0,"to":2,"reason":"SENSOR_FAILED"}
 *   {"t":..,"kind":"notify","to":"maintenance","severity":"urgent","sensor":"west/a","diag":"..","text":".."}
 *   {"t":..,"kind":"metric","name":"closure_ms_per_hour","value":123}
 *   {"kind":"ack","what":"subscribe"} / {"kind":"error","what":"rejected","reason":".."}
 *
 * In (host to bridge), a strict whitelist; anything else is rejected and never acted on:
 *   {"kind":"subscribe"}
 *   {"kind":"station_departure","dir":"west"|"east","eta_s":<integer 0..86400>}
 * No inbound message can reach the gate (CLAUDE.md rule 4): there is no such message type, and
 * lg-bridge has no gate connection at all.
 *
 * The bridge builds its state from the text of the resource managers (/dev/lg/controller,
 * /dev/lg/gate, /dev/lg/plant, /dev/lg/events); the parsers for those texts are here too.
 */

#include <stddef.h>
#include <stdint.h>

#include "lg_json_mini.h"

#define LG_BRIDGE_IN_MAX 256    /* longest accepted inbound line (without the newline) */
#define LG_BRIDGE_LINE_MAX 1024 /* every outbound line fits in this */
#define LG_BRIDGE_MAX_TRAINS 8  /* trains in one state line */

typedef struct {
    uint16_t id;
    uint8_t dir; /* lg_dir_t */
    uint8_t reserved;
    int32_t front_mm;
    int32_t rear_mm;
} lg_bridge_train_t;

typedef struct {
    uint32_t t_ms;   /* bridge clock (CLOCK_MONOTONIC ms) of the snapshot */
    uint8_t stale;   /* the device reader has not delivered for a while */
    uint8_t ctl_ok;  /* the controller text was read and parsed */
    uint8_t gate_ok; /* the gate text was read and parsed */
    uint8_t plant_ok;
    uint32_t level;
    uint32_t lights;
    uint32_t bell;
    uint32_t active_west;
    uint32_t active_east;
    uint32_t tickets;
    uint32_t stalls;
    uint32_t sensor_failed; /* bit dir * 3 + id */
    uint32_t events;
    char gate_state[12];
    int32_t gate_position; /* degrees, 0 up, 90 down */
    uint32_t watchdog_trips;
    uint32_t plant_ms;
    uint32_t island;
    uint32_t n_trains;
    lg_bridge_train_t trains[LG_BRIDGE_MAX_TRAINS];
} lg_bridge_state_t;

/* key=value lookups in a device text (keys are whole words: "level" does not match "level_end"). */
int lg_bridge_kv_u32(const char *text, const char *key, uint32_t *out);
int lg_bridge_kv_i32(const char *text, const char *key, int32_t *out);
int lg_bridge_kv_str(const char *text, const char *key, char *out, size_t len);

/* Fill the state from one device text. Return 0 when the main fields were found, -1 otherwise. */
int lg_bridge_from_controller(lg_bridge_state_t *s, const char *text);
int lg_bridge_from_gate(lg_bridge_state_t *s, const char *text);
int lg_bridge_from_plant(lg_bridge_state_t *s, const char *text); /* also the "train ..." lines */

/* One line of /dev/lg/events (lg_ctl_event_text). */
typedef struct {
    uint32_t seq;
    uint32_t t_ms;
    uint8_t kind; /* 1 level change, 2 notify, 3 metric (Step 11) */
    uint8_t has_trend; /* notify: trend_s was given and is >= 0 */
    uint8_t reserved[2];
    uint32_t from;
    uint32_t to;
    uint32_t reason;
    char severity[10];  /* INFO, WARNING, URGENT */
    char sensor[16];    /* west/a ... or none (gate, battery) */
    char diag[20];
    char component[16]; /* west/a ... gate, battery (Step 11); the sensor when the line has none */
    char name[20];      /* metric: lead_time_ms */
    int32_t trend_s;    /* notify: seconds to failure estimated by lg-health */
    int64_t value;      /* metric */
} lg_bridge_event_t;

#define LG_BRIDGE_EV_LEVEL 1
#define LG_BRIDGE_EV_NOTIFY 2
#define LG_BRIDGE_EV_METRIC 3

int lg_bridge_parse_event(const char *line, lg_bridge_event_t *ev); /* 0 ok, -1 not an event line */

/* Which events of one read of /dev/lg/events are new. The ring holds the events oldest first with rising
 * seq; a restarted controller starts again at seq 1 with an empty ring. The tracker remembers the last
 * event sent (seq and t_ms): when the ring still holds that event, only the events after it are new;
 * otherwise (another controller, or the event already left the ring) every event in the ring is new. */
typedef struct {
    uint32_t last_seq;
    uint32_t last_t_ms;
    uint8_t have; /* an event was sent already */
    uint8_t reserved[3];
} lg_bridge_evtrack_t;
void lg_bridge_evtrack_init(lg_bridge_evtrack_t *k);
/* Index of the first new event in ev[0..n-1] (n = none new); remembers ev[n-1] when n > 0. */
uint32_t lg_bridge_evtrack_first_new(lg_bridge_evtrack_t *k, const lg_bridge_event_t *ev, uint32_t n);

/* Outbound lines, newline and NUL terminated. The state line writes null for the level when ctl_ok is 0
 * and for the gate and its position when gate_ok is 0 (never a 0 that reads as level 0 or gate UP). Return the length (0 when it does not fit). */
size_t lg_bridge_state_json(const lg_bridge_state_t *s, char *buf, size_t len);
size_t lg_bridge_event_json(const lg_bridge_event_t *ev, char *buf, size_t len);
size_t lg_bridge_metric_json(uint32_t t_ms, const char *name, int64_t value, char *buf, size_t len);
size_t lg_bridge_reply_json(int ok, const char *what, const char *reason, char *buf, size_t len);

/* Name of a mode-manager reason code (lg_mode_reason_t), "UNKNOWN" outside the table. */
const char *lg_bridge_reason_name(uint32_t reason);

/* Recent event lines (level and notify), replayed to each client right after it connects, so that a
 * dashboard opened or reconnected after a ticket still shows it. Same size as the controller event ring.
 * Lines are whole JSON lines with their newline; a line that does not fit a slot is not stored. */
#define LG_BRIDGE_RECENT 32
typedef struct {
    char line[LG_BRIDGE_RECENT][LG_BRIDGE_LINE_MAX];
    uint32_t count; /* stored lines, at most LG_BRIDGE_RECENT */
    uint32_t next;  /* slot of the next push */
} lg_bridge_recent_t;
void lg_bridge_recent_init(lg_bridge_recent_t *r);
void lg_bridge_recent_push(lg_bridge_recent_t *r, const char *line);
/* How many of the newest stored lines fit in budget bytes together. */
uint32_t lg_bridge_recent_select(const lg_bridge_recent_t *r, size_t budget);
/* Stored line i, 0 = oldest; NULL outside 0..count-1. Replay: i from count - select() to count - 1. */
const char *lg_bridge_recent_get(const lg_bridge_recent_t *r, uint32_t i);

/* Inbound. */
typedef enum {
    LG_BRIDGE_IN_REJECT = 0,
    LG_BRIDGE_IN_SUBSCRIBE = 1,
    LG_BRIDGE_IN_STATION_DEPARTURE = 2
} lg_bridge_in_kind_t;

typedef struct {
    uint8_t kind; /* lg_bridge_in_kind_t */
    uint8_t dir;  /* station_departure: lg_dir_t */
    uint8_t reserved[2];
    uint32_t eta_s;   /* station_departure: seconds */
    char reason[32];  /* reject: why (too_long, not_json, not_object, unknown_kind, bad_field, extra_field) */
} lg_bridge_in_t;

/* Classify one inbound line (without its newline). doc is scratch space owned by the caller. */
void lg_bridge_parse_in(lg_json_doc_t *doc, const char *line, size_t len, lg_bridge_in_t *out);

#endif
