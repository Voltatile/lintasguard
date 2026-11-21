#ifndef LG_SUPERVISOR_LOGIC_H
#define LG_SUPERVISOR_LOGIC_H

/*
 * Supervisor logic (README 8.5, 8.6). Portable and pure: the caller reports spawns, exits and
 * heartbeats with the time, calls lg_sup_step() regularly and carries out the actions it returns
 * (spawn, kill, report status). Nothing is read from the system, nothing is allocated.
 *
 * Per process (lg_proc_id_t):
 *   IDLE      must be (re)spawned: the action SPAWN is returned
 *   STARTING  spawned, no heartbeat yet; a first beat is expected within start_grace_ms
 *   RUNNING   beating; no beat for heartbeat_timeout_ms gives KILL
 *   KILLING   SIGKILL requested, waiting for the exit (the KILL action is repeated)
 *   DEAD      the restart budget is used up; absorbing, nothing is spawned any more
 * A process that exits or is killed counts as one restart. Budget: at most `budget` restarts in
 * any window of window_ms; the failure after that makes the process DEAD (README 8.5).
 *
 * Stall rule: when the supervisor's own step comes later than stall_ms after the previous one, the whole
 * system stood still (a VM pause, a long interrupt), and it cannot tell a hung process from a paused
 * one. The heartbeat and start clocks of the processes that are STARTING or RUNNING are then restarted
 * (counted in `stalls`) instead of killing everything that was quiet for 200 ms. A process that stays
 * quiet after that is still killed. stall_ms 0 turns the rule off.
 *
 * Boot order (README 8.6): the first spawn of a process waits until every process of a lower stage
 * has beaten (or is DEAD). A restart never waits. A process that is not installed is never
 * spawned and is reported RUNNING (it is not part of this system yet).
 *
 * Reported state (lg_proc_state_t): STARTING, KILLING and IDLE are reported RESTARTING (the mode
 * manager treats a process that is not beating as not usable), RUNNING as RUNNING, DEAD as DEAD.
 */

#include <stddef.h>
#include <stdint.h>

#include "lg_config.h"
#include "lg_proto.h"

#define LG_SUP_BUDGET_MAX 8

typedef enum {
    LG_SUP_IDLE = 0,
    LG_SUP_STARTING = 1,
    LG_SUP_RUNNING = 2,
    LG_SUP_KILLING = 3,
    LG_SUP_DEAD = 4
} lg_sup_phase_t;

typedef enum {
    LG_SUP_WHY_NONE = 0,
    LG_SUP_WHY_EXIT = 1,  /* the process exited or crashed */
    LG_SUP_WHY_HANG = 2,  /* no heartbeat: killed by the supervisor */
    LG_SUP_WHY_SPAWN = 3  /* the spawn call failed */
} lg_sup_why_t;

typedef enum {
    LG_SUP_ACT_SPAWN = 1,  /* start process `proc`, then call lg_sup_on_spawn */
    LG_SUP_ACT_KILL = 2,   /* SIGKILL `pid` (`proc` is its process id) */
    LG_SUP_ACT_STATUS = 3  /* the reported state of `proc` changed: send PROC_STATUS */
} lg_sup_act_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t proc;
    uint16_t reserved;
    uint32_t pid;
} lg_sup_action_t;

/* An action list of one step never holds more than this many entries. */
#define LG_SUP_MAX_ACTIONS (2 * LG_PROC_COUNT)

typedef struct {
    uint32_t heartbeat_timeout_ms;
    uint32_t start_grace_ms;
    uint32_t window_ms;
    uint32_t kill_retry_ms; /* a KILL that did not end in an exit is repeated this often */
    uint32_t stall_ms;      /* see the stall rule above, 0 = off */
    uint8_t budget;         /* restarts allowed per window (1..LG_SUP_BUDGET_MAX) */
    uint8_t installed[LG_PROC_COUNT];
    uint8_t stage[LG_PROC_COUNT]; /* boot stage of the first spawn, 0 first */
} lg_sup_cfg_t;

/* Gate (stage 0); plant and the six sensors (stage 1); controller (stage 2). The rest is not installed. */
lg_sup_cfg_t lg_sup_cfg_default(void);

typedef struct {
    uint8_t phase;       /* lg_sup_phase_t */
    uint8_t ever_started;
    uint8_t why;         /* lg_sup_why_t of the last failure */
    uint8_t recovery_pending; /* a failure was detected, the new process has not beaten yet */
    uint8_t restart_valid[LG_SUP_BUDGET_MAX];
    uint32_t pid;
    uint32_t since_ms;       /* spawn time while STARTING */
    uint32_t last_hb_ms;
    uint32_t kill_ms;        /* last KILL action */
    uint32_t detect_ms;      /* when the last failure was detected */
    uint32_t restart_ms[LG_SUP_BUDGET_MAX];
    uint32_t recovery_ms;    /* detection of the last failure to the first beat of the new process */
    uint8_t last_reported;   /* lg_proc_state_t of the last STATUS action, 255 = none yet */
    uint8_t last_reported_restarts;
    uint8_t reserved[2];
    uint32_t total_restarts;
    uint32_t hb_rx;
    uint32_t hb_ignored;
} lg_sup_proc_t;

typedef struct {
    lg_sup_cfg_t cfg;
    lg_sup_proc_t proc[LG_PROC_COUNT];
    uint32_t last_step_ms;
    uint32_t stalls;        /* steps that came later than stall_ms after the previous one */
    uint8_t stepped;        /* lg_sup_step ran at least once */
    uint8_t reserved[3];
    uint32_t exits_ignored; /* exit of a pid that is not a live child of the table */
    uint32_t hb_unknown;    /* beat with a process id outside the table */
} lg_sup_t;

void lg_sup_init(lg_sup_t *s, const lg_sup_cfg_t *cfg);

/* The process was started with this pid. A pid of 0 is refused (-1). */
int lg_sup_on_spawn(lg_sup_t *s, uint8_t proc, uint32_t pid, uint32_t now_ms);
/* The spawn call failed: counts as a failure of that process. */
void lg_sup_on_spawn_failed(lg_sup_t *s, uint8_t proc, uint32_t now_ms);
/* Returns 1 when the beat was accepted, 0 when it was ignored (unknown process, wrong pid, wrong phase). */
int lg_sup_on_heartbeat(lg_sup_t *s, uint8_t proc, uint32_t pid, uint32_t now_ms);
/* A child with this pid ended (exit, crash or kill). Returns 1 when it belonged to the table. */
int lg_sup_on_exit(lg_sup_t *s, uint32_t pid, uint32_t now_ms);

/* Time-driven part. Writes at most LG_SUP_MAX_ACTIONS actions into `out` and returns their number. */
size_t lg_sup_step(lg_sup_t *s, uint32_t now_ms, lg_sup_action_t *out, size_t max);

/* Reported state (lg_proc_state_t) and restarts in the current window of one process. */
uint8_t lg_sup_reported_state(const lg_sup_t *s, uint8_t proc);
uint8_t lg_sup_restarts_in_window(const lg_sup_t *s, uint8_t proc, uint32_t now_ms);

/* Name of a process for the status text: "gate", "controller", "plant", "sensor/west/a", ... */
const char *lg_sup_proc_name(uint8_t proc);

/*
 * Text for /dev/lg/supervisor: a header line, then one line per installed process, for example
 *   proc=sensor/west/a id=3 state=RUNNING pid=123 restarts=0/3 total=0 why=none recovery_ms=0 hb_rx=40 hb_ignored=0
 * Returns the length written (always NUL terminated, cut at a line end when `len` is too small).
 */
size_t lg_sup_text(const lg_sup_t *s, uint32_t now_ms, char *buf, size_t len);

#endif
