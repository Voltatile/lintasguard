#ifndef LG_PROTO_H
#define LG_PROTO_H

/*
 * LintasGuard shared message definitions (README section 7).
 * Portable C11: no QNX headers. Every struct is fixed-size, has no implicit
 * padding (checked by _Static_assert) and starts with lg_hdr_t.
 * Changing any struct or enum value: bump LG_PROTO_VERSION and update every
 * sender and receiver in the same commit.
 */

#include <stdint.h>

#define LG_PROTO_VERSION 1

/* ---- Identifiers ------------------------------------------------------- */

typedef enum {
    LG_MSG_SENSOR_SAMPLE = 1,
    LG_MSG_GATE_CMD = 2,
    LG_MSG_GATE_STATE = 3,
    LG_MSG_HEALTH_VERDICT = 4,
    LG_MSG_PROC_STATUS = 5,
    LG_MSG_SCHED_QUERY = 6,
    LG_MSG_SCHED_REPLY = 7,
    LG_MSG_EVENT = 8,
    LG_MSG_GUARD_ACK = 9,
    LG_MSG_ACK = 10 /* generic reply to a MsgSend: status 0 = accepted */
} lg_msg_type_t;

/*
 * Pulse codes (8-bit). Values are in the user range; verify against
 * _PULSE_CODE_MINAVAIL/_PULSE_CODE_MAXAVAIL in the SDP 8.0 headers on target.
 */
typedef enum {
    LG_PULSE_TICK = 1,
    LG_PULSE_HEARTBEAT = 2,
    LG_PULSE_HEALTH_RECHECK = 3,
    LG_PULSE_SHUTDOWN = 4
} lg_pulse_code_t;

typedef enum { LG_DIR_WEST = 0, LG_DIR_EAST = 1, LG_DIR_COUNT = 2 } lg_dir_t;

typedef enum {
    LG_SENSOR_A = 0,
    LG_SENSOR_B = 1,
    LG_SENSOR_ISLAND = 2,
    LG_SENSOR_ID_COUNT = 3
} lg_sensor_id_t;

typedef enum {
    LG_LEVEL_NORMAL = 0,
    LG_LEVEL_EARLY_WARNING = 1,
    LG_LEVEL_DEGRADED = 2,
    LG_LEVEL_UNCERTAIN = 3,
    LG_LEVEL_FAILSAFE = 4
} lg_level_t;

typedef enum {
    LG_GATE_CMD_LOWER = 1,
    LG_GATE_CMD_RAISE = 2,
    LG_GATE_CMD_LIGHTS_ON = 3,
    LG_GATE_CMD_LIGHTS_OFF = 4,
    LG_GATE_CMD_BELL_ON = 5,
    LG_GATE_CMD_BELL_OFF = 6
} lg_gate_cmd_t;

typedef enum {
    LG_GATE_UP = 0,
    LG_GATE_LOWERING = 1,
    LG_GATE_DOWN = 2,
    LG_GATE_RAISING = 3,
    LG_GATE_FAULT = 4
} lg_gate_state_t;

typedef enum { LG_VERDICT_OK = 0, LG_VERDICT_WARNING = 1, LG_VERDICT_FAILED = 2 } lg_verdict_t;

/* Components that health verdicts refer to. Sensors: 0..5 = dir * 3 + sensor id. */
typedef enum {
    LG_COMP_SENSOR_BASE = 0,
    LG_COMP_SENSOR_LAST = 5,
    LG_COMP_GATE = 6,
    LG_COMP_BATTERY = 7,
    LG_COMP_COUNT = 8
} lg_component_t;

typedef enum {
    LG_PROC_SUPERVISOR = 0,
    LG_PROC_CONTROLLER = 1,
    LG_PROC_GATE = 2,
    LG_PROC_SENSOR_BASE = 3, /* sensors: 3..8 = 3 + dir * 3 + sensor id */
    LG_PROC_PLANT = 9,
    LG_PROC_HEALTH = 10,
    LG_PROC_SCHEDULE = 11,
    LG_PROC_BRIDGE = 12,
    LG_PROC_COUNT = 13
} lg_proc_id_t;

typedef enum { LG_PROC_RUNNING = 0, LG_PROC_RESTARTING = 1, LG_PROC_DEAD = 2 } lg_proc_state_t;

typedef enum { LG_SRC_TIMETABLE = 0, LG_SRC_STATION = 1 } lg_sched_source_t;

typedef enum {
    LG_EVT_LEVEL_CHANGE = 1,
    LG_EVT_ACTIVATION = 2,
    LG_EVT_CLEAR = 3,
    LG_EVT_NOTIFY = 4,
    LG_EVT_METRIC = 5
} lg_event_kind_t;

typedef enum { LG_SEV_INFO = 0, LG_SEV_WARNING = 1, LG_SEV_URGENT = 2 } lg_severity_t;

/* Guard actions requested through lgctl guard-ack. */
typedef enum { LG_GUARD_ACTION_ACK = 1, LG_GUARD_ACTION_CONFIRM_CLEAR = 2 } lg_guard_action_t;

/* Fault modes (README 7.4 and 7.5), set through devctl. */
typedef enum {
    LG_SFAULT_NONE = 0,
    LG_SFAULT_NOISE = 1,
    LG_SFAULT_DROPOUT = 2,
    LG_SFAULT_STUCK_OCCUPIED = 3,
    LG_SFAULT_STUCK_CLEAR = 4,
    LG_SFAULT_DELAY = 5,
    LG_SFAULT_DRIFT = 6,
    LG_SFAULT_CRASH = 7,
    LG_SFAULT_HANG = 8
} lg_sensor_fault_t;

typedef enum {
    LG_GFAULT_NONE = 0,
    LG_GFAULT_SLOW = 1,
    LG_GFAULT_HIGH_CURRENT = 2,
    LG_GFAULT_STUCK_UP = 3,
    LG_GFAULT_STUCK_DOWN = 4,
    LG_GFAULT_CRASH = 5
} lg_gate_fault_t;

/* ---- Messages ---------------------------------------------------------- */

#define LG_EVENT_TEXT_LEN 64
#define LG_GUARD_ID_LEN 8
#define LG_PIN_HASH_LEN 32

typedef struct {
    uint16_t type;    /* lg_msg_type_t */
    uint16_t version; /* LG_PROTO_VERSION */
    uint32_t seq;     /* per-sender sequence number, detects loss */
    uint64_t t_ns;    /* CLOCK_MONOTONIC timestamp at send, ns */
} lg_hdr_t;

typedef struct {
    lg_hdr_t hdr;
    uint8_t dir;      /* lg_dir_t */
    uint8_t id;       /* lg_sensor_id_t */
    uint8_t occupied; /* 0 or 1 */
    uint8_t quality;  /* 0..100 */
    uint16_t axle_count;
    uint16_t raw_level; /* raw analogue level, ADC counts */
} lg_sensor_sample_t;

typedef struct {
    lg_hdr_t hdr;
    uint8_t cmd; /* lg_gate_cmd_t */
    uint8_t reserved[3];
    uint32_t deadline_ms; /* gate must reach the commanded state within this time */
} lg_gate_cmd_msg_t;

typedef struct {
    lg_hdr_t hdr;
    uint8_t state; /* lg_gate_state_t */
    uint8_t reserved;
    int16_t position_deg; /* 0 = up, 90 = down */
    uint16_t current_ma;
    uint16_t reserved2;
    uint32_t travel_ms; /* duration of the last completed movement */
    uint32_t reserved3;
} lg_gate_state_msg_t;

typedef struct {
    lg_hdr_t hdr;
    uint8_t component_id; /* lg_component_t */
    uint8_t score;        /* 0..100 */
    uint8_t verdict;      /* lg_verdict_t */
    uint8_t reserved;
    uint16_t diagnosis_code;
    uint16_t reserved2;
    int32_t trend_days_to_failure; /* estimated days, -1 = unknown */
    uint32_t reserved3;
} lg_health_verdict_t;

typedef struct {
    lg_hdr_t hdr;
    uint8_t proc_id; /* lg_proc_id_t */
    uint8_t state;   /* lg_proc_state_t */
    uint8_t restarts_in_window;
    uint8_t reserved;
    uint32_t reserved2;
} lg_proc_status_t;

typedef struct {
    lg_hdr_t hdr;
    uint8_t dir; /* lg_dir_t */
    uint8_t reserved[3];
    uint32_t reserved2;
} lg_sched_query_t;

typedef struct {
    lg_hdr_t hdr;
    uint8_t dir;        /* lg_dir_t */
    uint8_t source;     /* lg_sched_source_t */
    uint8_t confidence; /* 0..100, 0 = no information */
    uint8_t reserved;
    uint32_t reserved2;
    uint64_t eta_ns; /* CLOCK_MONOTONIC time of the expected arrival, ns */
} lg_sched_reply_t;

typedef struct {
    lg_hdr_t hdr;
    uint8_t kind;     /* lg_event_kind_t */
    uint8_t severity; /* lg_severity_t */
    uint8_t level_from;
    uint8_t level_to;
    uint32_t code;
    int64_t value_milli; /* metric value times 1000 */
    char text[LG_EVENT_TEXT_LEN]; /* NUL-terminated */
} lg_event_t;

typedef struct {
    lg_hdr_t hdr;
    char guard_id[LG_GUARD_ID_LEN]; /* NUL-terminated */
    uint8_t pin_hash[LG_PIN_HASH_LEN];
    uint8_t action; /* lg_guard_action_t */
    uint8_t reserved[7];
} lg_guard_ack_t;

typedef struct {
    lg_hdr_t hdr;
    int32_t status; /* 0 = accepted, otherwise an errno value */
    uint32_t reserved;
} lg_ack_t;

_Static_assert(sizeof(lg_hdr_t) == 16, "lg_hdr_t size");
_Static_assert(sizeof(lg_sensor_sample_t) == 24, "lg_sensor_sample_t size");
_Static_assert(sizeof(lg_gate_cmd_msg_t) == 24, "lg_gate_cmd_msg_t size");
_Static_assert(sizeof(lg_gate_state_msg_t) == 32, "lg_gate_state_msg_t size");
_Static_assert(sizeof(lg_health_verdict_t) == 32, "lg_health_verdict_t size");
_Static_assert(sizeof(lg_proc_status_t) == 24, "lg_proc_status_t size");
_Static_assert(sizeof(lg_sched_query_t) == 24, "lg_sched_query_t size");
_Static_assert(sizeof(lg_sched_reply_t) == 32, "lg_sched_reply_t size");
_Static_assert(sizeof(lg_event_t) == 96, "lg_event_t size");
_Static_assert(sizeof(lg_guard_ack_t) == 64, "lg_guard_ack_t size");
_Static_assert(sizeof(lg_ack_t) == 24, "lg_ack_t size");

#endif /* LG_PROTO_H */
