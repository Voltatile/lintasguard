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

/* Gate motor simulation (lg-gate). */
#define LG_GATE_TRAVEL_MS 4000        /* initial: nominal lowering or raising time */
#define LG_GATE_CURRENT_MOVING_MA 800 /* initial: motor current while moving */
#define LG_GATE_CURRENT_HOLD_MA 50    /* initial: holding current at the end position */
#define LG_GATE_FAULT_TIMEOUT_FACTOR 3 /* initial: a movement taking this many times nominal is a FAULT */

/* Mode manager. Simulated time. */
#define LG_T_STABLE_MS 30000      /* de-escalation needs the condition clear this long */
#define LG_T_BOOT_STABLE_MS 5000  /* initial */
#define LG_T_INFO_TIMEOUT_MS 60000 /* initial: level 3 to 4 when no schedule info */

/* Activation and clearing. */
#define LG_EXTRA_MARGIN_MS 5000 /* extra pre-warning in level 2 */
#define LG_PREWARNING_MS 10000  /* initial: lights/bell on before the gate lowers */
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
