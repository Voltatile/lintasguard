#ifndef LG_DEVCTL_H
#define LG_DEVCTL_H

/*
 * devctl commands of the LintasGuard resource managers (README 7.3). QNX-only, so this
 * header lives under target/ and not in common/ (common/ must stay portable).
 * Class _DCMD_MISC is the one recommended for non-QNX devices.
 */

#include <devctl.h>
#include <stdint.h>

/* /dev/lg/gate */
typedef struct {
    uint32_t mode;       /* lg_gate_fault_t; LG_GFAULT_CRASH makes lg-gate abort() */
    uint32_t factor_pct; /* SLOW and HIGH_CURRENT: 100 = nominal */
} lg_gate_fault_req_t;

typedef struct {
    uint32_t state; /* lg_gate_state_t */
    uint32_t target;
    int32_t position_mdeg;
    uint32_t current_ma;
    uint32_t last_travel_ms;
    uint32_t fault; /* lg_gate_fault_t */
    uint32_t fault_pct;
    uint32_t watchdog_trips;
    uint32_t rejected;
    uint32_t ticks; /* model steps executed */
    uint32_t sched_policy;   /* policy of the serving thread right now (SCHED_FIFO = 1) */
    uint32_t sched_priority; /* priority of the serving thread right now */
} lg_gate_stats_t;

#define DCMD_LG_GATE_FAULT __DIOT(_DCMD_MISC, 0x40, lg_gate_fault_req_t)
#define DCMD_LG_GATE_STATS __DIOF(_DCMD_MISC, 0x41, lg_gate_stats_t)

/* /dev/lg/sensor/<dir>/<id> */
typedef struct {
    uint32_t mode;  /* lg_sensor_fault_t; LG_SFAULT_CRASH makes lg-sensor abort() */
    uint32_t param; /* NOISE level, DROPOUT percent, DELAY ms, DRIFT units per second */
} lg_sensor_fault_req_t;

typedef struct {
    uint32_t ticks;        /* timer ticks handled */
    uint32_t samples_sent; /* samples the controller accepted */
    uint32_t no_sample;    /* ticks without a sample (DROPOUT, HANG) */
    uint32_t send_fail;    /* sends to the controller that failed or timed out */
    uint32_t no_controller; /* ticks where there was no controller connection */
    uint32_t plant_fail;   /* ticks where the plant did not answer */
    uint32_t overruns;     /* ticks that came more than the tolerance late */
    uint32_t timeouts;     /* bounded sends that hit their time limit (plant or controller) */
    uint32_t connects;     /* connections (re)established by the connector threads */
    uint32_t fault;        /* lg_sensor_fault_t */
    uint32_t fault_param;
    uint32_t seq;          /* sequence number of the latest tick */
    uint32_t sched_policy;   /* of the device thread */
    uint32_t sched_priority;
    uint32_t worker_policy;  /* of the worker thread, as it was at its latest tick */
    uint32_t worker_priority;
} lg_sensor_stats_t;

#define DCMD_LG_SENSOR_FAULT __DIOT(_DCMD_MISC, 0x50, lg_sensor_fault_req_t)
#define DCMD_LG_SENSOR_STATS __DIOF(_DCMD_MISC, 0x51, lg_sensor_stats_t)

#endif
