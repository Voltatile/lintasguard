#ifndef LG_CONFIG_H
#define LG_CONFIG_H

/*
 * Compile-time defaults (README 3, 8). Runtime values come from the JSON files in config/
 * and override these. Units are in the macro name: _MS, _S, _MM, _PCT.
 * Values marked "initial" are not fixed by the README and are tuned later;
 * each is recorded in docs/ARCHITECTURE.md (Decisions log).
 */

/* Controller and supervision. */
#define LG_TICK_MS 10
#define LG_TICK_TOLERANCE_MS 2 /* initial: a tick later than LG_TICK_MS + this is a deadline miss */
#define LG_HEARTBEAT_PERIOD_MS 50
#define LG_HEARTBEAT_TIMEOUT_MS 200
#define LG_RESTART_BUDGET 3
#define LG_RESTART_WINDOW_MS 10000
#define LG_GATE_WATCHDOG_MS 100
#define LG_CTL_IPC_TIMEOUT_MS 5        /* initial: bound for one MsgSend from the controller to the gate */
#define LG_CTL_CONNECTOR_PERIOD_MS 200 /* initial: how often the connector thread looks for the gate */
#define LG_CTL_LIGHTS_REFRESH_TICKS 50 /* initial: lights and bell commands are repeated this often */
#define LG_CTL_GATE_BOOT_GRACE_MS 2000 /* initial: a gate never heard yet is a WARNING this long, then silent */
#define LG_CTL_BURST_GUARD_MS 5        /* initial: a tick pulse closer than this to the last one is ignored */

/* Gate motor simulation (lg-gate). */
#define LG_GATE_TRAVEL_MS 4000        /* initial: nominal lowering or raising time */
#define LG_GATE_CURRENT_MOVING_MA 800 /* initial: motor current while moving */
#define LG_GATE_CURRENT_HOLD_MA 50    /* initial: holding current at the end position */
#define LG_GATE_FAULT_TIMEOUT_FACTOR 3 /* initial: a movement taking this many times nominal is a FAULT */

/* Sensors (lg-sensor) and plant (lg-plant). */
#define LG_SENSOR_PERIOD_MS 10       /* one sample per tick */
#define LG_SENSOR_CLEAR_LEVEL 200    /* initial: raw level with the track clear, ADC counts */
#define LG_SENSOR_OCCUPIED_LEVEL 800 /* initial: raw level with a train over the sensor */
#define LG_SENSOR_THRESHOLD 500      /* raw level above this reads as occupied */
#define LG_SENSOR_BASE_NOISE 3       /* initial: noise units of a healthy sensor */
#define LG_SENSOR_QUALITY_USABLE_MIN 1 /* quality 0 means the sensor sees nothing: not usable */
#define LG_SENSOR_COUNTS_PER_NOISE 8 /* initial: raw level amplitude per noise unit (+- counts) */
#define LG_SENSOR_DELAY_MAX_MS 2000  /* longest injectable delay */
#define LG_SENSOR_IPC_TIMEOUT_MS 5   /* bound for the blocking calls inside the sensor tick */
#define LG_SENSOR_CONNECTOR_PERIOD_MS 200 /* how often a connector thread looks for a missing peer */
#define LG_PLANT_EVENT_PERIOD_MS 50  /* how often the plant checks for train events to log */

/* Mode manager. Simulated time. */
#define LG_T_STABLE_MS 30000      /* de-escalation needs the condition clear this long */
#define LG_T_BOOT_STABLE_MS 5000  /* initial */
#define LG_T_INFO_TIMEOUT_MS 60000 /* initial: level 3 to 4 when no schedule info */

/* Activation and clearing. */
#define LG_EXTRA_MARGIN_MS 5000 /* extra pre-warning in level 2 */
#define LG_PREWARNING_MS 5000   /* initial: lights/bell on before the gate lowers; prewarning + gate travel must stay below the fastest train time to the island */
#define LG_T_CLEAR_MS 2000      /* island clear this long before the gate may rise */
#define LG_MIN_GATE_DOWN_MS 3000 /* initial */

/* Level 3 timetable windows. */
#define LG_WINDOW_LEAD_MS 60000
#define LG_WINDOW_TOLERANCE_MS 180000

/* Health scoring. Verdict: OK >= OK_MIN, WARNING >= WARNING_MIN, otherwise FAILED. */
#define LG_SCORE_OK_MIN 70
#define LG_SCORE_WARNING_MIN 40
#define LG_T_DISAGREE_MS 500 /* initial: A/B disagreement tolerated this long */

/* Sample freshness: a sample older than this is not used (README 10.3). */
#define LG_SAMPLE_MAX_AGE_MS 20

/* Host link. */
#define LG_BRIDGE_PORT 5555

#endif /* LG_CONFIG_H */
