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

#endif
