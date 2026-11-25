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
#define LG_SUP_STALL_MS 50              /* initial: a supervisor tick later than this means the whole system stalled */
#define LG_SUP_START_GRACE_MS 1500      /* initial: a new process may take this long to send its first beat */
#define LG_SUP_STATUS_REFRESH_MS 1000   /* initial: the supervisor repeats the whole table this often */
#define LG_CTL_SUP_SILENCE_MS 3500      /* initial: supervisor silent this long after it was heard = DEAD */
#define LG_CTL_STALL_MS 50              /* initial: a controller step this late means the controller itself stalled */
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

/* Level 3 timetable windows (Step 12). */
#define LG_WINDOW_LEAD_MS 60000
#define LG_WINDOW_TOLERANCE_MS 180000
#define LG_SCHED_PUSH_MS 500       /* lg-schedule sends every direction's reply this often */
#define LG_SCHED_SEND_TIMEOUT_MS 5 /* bound for its MsgSend to the controller (and lg-bridge's to lg-schedule) */

/* Health scoring. Verdict: OK >= OK_MIN, WARNING >= WARNING_MIN, otherwise FAILED. */
#define LG_SCORE_OK_MIN 70
#define LG_SCORE_WARNING_MIN 40
#define LG_T_DISAGREE_MS 500 /* initial: A/B disagreement tolerated this long */
/*
 * Interim stuck rules (Step 9, replaced by lg-health in Step 11): an approach sensor occupied without
 * a break for longer than this is not a passing train. The slowest normal_day train occupies the
 * 2 m sensor zone for about 7.6 s (150 m at 20 m/s).
 */
#define LG_SENSOR_MAX_OCCUPIED_MS 20000 /* initial */

/*
 * lg-health (Step 11, README 8.3). All "initial"; config/thresholds.json overrides them. Noise is in
 * noise units (LG_SENSOR_BASE_NOISE scale), stored in milli units in core/health.c.
 */
#define LG_HEALTH_TICK_MS 50          /* lg-health timer period */
#define LG_HEALTH_POLL_MS 200         /* a component with verdict OK is read this often */
#define LG_HEALTH_POLL_FAST_MS 50     /* WARNING or FAILED: read this often (README 3: level 1 samples faster) */
#define LG_HEALTH_REFRESH_MS 500      /* every verdict is sent again at least this often */
#define LG_HEALTH_LEARN_MS 10000      /* the noise baseline is learned over this time after a (re)start */
#define LG_HEALTH_NOISE_LEARN_MAX 2   /* a learned baseline is at most base noise + this */
#define LG_HEALTH_NOISE_FAIL 30       /* noise units: FAILED (amplitude 240 counts, below the 300 that reads a phantom train) */
#define LG_HEALTH_CUSUM_K_MILLI 1000  /* CUSUM slack: noise this far (milli units) above the baseline is tolerated */
#define LG_HEALTH_CUSUM_H_MILLI 2000  /* CUSUM alarm: milli unit-seconds above baseline + k */
#define LG_HEALTH_DROPOUT_WARN_PM 50  /* per mille of ticks without a usable sample: WARNING */
#define LG_HEALTH_DROPOUT_FAIL_PM 300 /* per mille: FAILED */
#define LG_HEALTH_DELAY_ZERO_MS 50    /* a rise this much later than the partner is still "together" */
#define LG_HEALTH_DELAY_FAIL_MS 400   /* a rise this much later than the partner: score 40 */
#define LG_HEALTH_PAIR_WINDOW_MS 2000 /* rises of A and B this close belong to the same train */
#define LG_HEALTH_DISAGREE_CLEAR 1    /* agreeing trains needed to forget a disagreement (as the controller's stuck latch) */
#define LG_HEALTH_GATE_TRAVEL_FAIL_PCT 175 /* travel time vs nominal: FAILED (5 s prewarning + 7 s < 13.3 s fastest train) */
#define LG_HEALTH_GATE_CURRENT_FAIL_PCT 200 /* peak motor current vs nominal: FAILED */
#define LG_HEALTH_GATE_CUSUM_K_PCT 5  /* gate CUSUM slack, percent of nominal per movement */
#define LG_HEALTH_GATE_CUSUM_H_PCT 30 /* gate CUSUM alarm, percent-movements */
#define LG_HEALTH_OK_SCORE 75         /* back to OK only at this score or better (hysteresis) */
#define LG_HEALTH_OK_HOLD_MS 2000     /* ... held this long */
#define LG_HEALTH_NODATA_POLLS 3      /* failed reads in a row before the verdict says NO_DATA */
#define LG_CTL_HEALTH_SILENCE_MS 2000 /* controller: no verdict for this long after the first = health lost */

/* Backup battery (simulated in lg-gate). Millivolts, simulated seconds. */
#define LG_BATTERY_FULL_MV 13200
#define LG_BATTERY_EMPTY_MV 10500
#define LG_BATTERY_CHARGE_MV_S 20     /* charger: mV per second back towards full */
#define LG_BATTERY_WARN_MV 12000
#define LG_BATTERY_FAIL_MV 11000
#define LG_BATTERY_TTE_WARN_S 600     /* time to empty below this: WARNING */
#define LG_BATTERY_TTE_FAIL_S 60      /* time to empty below this: FAILED */

/* Sample freshness: a sample older than this is not used (README 10.3). */
#define LG_SAMPLE_MAX_AGE_MS 20

/* Host link. */
#define LG_BRIDGE_PORT 5555

#endif /* LG_CONFIG_H */
