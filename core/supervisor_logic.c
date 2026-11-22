#include "lg_supervisor_logic.h"

#include <stdio.h>
#include <string.h>

#include "lg_time.h"

lg_sup_cfg_t lg_sup_cfg_default(void) {
    lg_sup_cfg_t c;
    int i;

    memset(&c, 0, sizeof c);
    c.heartbeat_timeout_ms = LG_HEARTBEAT_TIMEOUT_MS;
    c.start_grace_ms = LG_SUP_START_GRACE_MS;
    c.window_ms = LG_RESTART_WINDOW_MS;
    c.kill_retry_ms = 500; /* initial */
    c.stall_ms = LG_SUP_STALL_MS;
    c.budget = LG_RESTART_BUDGET;
    c.installed[LG_PROC_GATE] = 1;
    c.stage[LG_PROC_GATE] = 0;
    c.installed[LG_PROC_PLANT] = 1;
    c.stage[LG_PROC_PLANT] = 1;
    for (i = 0; i < LG_DIR_COUNT * LG_SENSOR_ID_COUNT; i++) {
        c.installed[LG_PROC_SENSOR_BASE + i] = 1;
        c.stage[LG_PROC_SENSOR_BASE + i] = 1;
    }
    c.installed[LG_PROC_CONTROLLER] = 1;
    c.stage[LG_PROC_CONTROLLER] = 2;
    c.installed[LG_PROC_BRIDGE] = 1; /* Step 10: started last, a dead bridge is level 1 */
    c.stage[LG_PROC_BRIDGE] = 3;
    return c;
}

void lg_sup_init(lg_sup_t *s, const lg_sup_cfg_t *cfg) {
    int i;

    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    if (s->cfg.budget < 1) {
        s->cfg.budget = 1;
    }
    if (s->cfg.budget > LG_SUP_BUDGET_MAX) {
        s->cfg.budget = LG_SUP_BUDGET_MAX;
    }
    if (s->cfg.window_ms == 0) {
        s->cfg.window_ms = LG_RESTART_WINDOW_MS; /* a zero window would forget every restart at once */
    }
    for (i = 0; i < LG_PROC_COUNT; i++) {
        s->proc[i].phase = LG_SUP_IDLE;
        s->proc[i].last_reported = 255;
    }
}

static int installed(const lg_sup_t *s, int proc) {
    return proc >= 0 && proc < LG_PROC_COUNT && s->cfg.installed[proc] != 0;
}

static void prune(lg_sup_t *s, lg_sup_proc_t *p, uint32_t now_ms) {
    int i;

    for (i = 0; i < s->cfg.budget; i++) {
        if (p->restart_valid[i] && lg_elapsed_ge(now_ms, p->restart_ms[i], s->cfg.window_ms)) {
            p->restart_valid[i] = 0;
        }
    }
}

static uint8_t in_window(const lg_sup_t *s, const lg_sup_proc_t *p) {
    uint8_t n = 0;
    int i;

    for (i = 0; i < s->cfg.budget; i++) {
        n = (uint8_t)(n + (p->restart_valid[i] ? 1 : 0));
    }
    return n;
}

/* A running or starting process is gone (or being killed). Count the restart or give up. */
static void failure(lg_sup_t *s, lg_sup_proc_t *p, uint32_t now_ms, uint8_t why, int already_detected) {
    int i;

    p->why = why;
    if (!already_detected) {
        p->detect_ms = now_ms;
    }
    p->recovery_pending = 1;
    p->pid = 0;
    prune(s, p, now_ms);
    if (in_window(s, p) >= s->cfg.budget) {
        p->phase = LG_SUP_DEAD;
        p->recovery_pending = 0;
        return;
    }
    for (i = 0; i < s->cfg.budget; i++) {
        if (!p->restart_valid[i]) {
            p->restart_valid[i] = 1;
            p->restart_ms[i] = now_ms;
            break;
        }
    }
    p->total_restarts++;
    p->phase = LG_SUP_IDLE;
}

int lg_sup_on_spawn(lg_sup_t *s, uint8_t proc, uint32_t pid, uint32_t now_ms) {
    lg_sup_proc_t *p;
    int q;

    if (!installed(s, proc) || pid == 0) {
        return -1;
    }
    p = &s->proc[proc];
    if (p->phase != LG_SUP_IDLE) {
        return -1;
    }
    for (q = 0; q < LG_PROC_COUNT; q++) {
        if (q != proc && s->proc[q].pid == pid) {
            return -1; /* a pid belongs to one process: two rows with it would mix up exits and beats */
        }
    }
    p->phase = LG_SUP_STARTING;
    p->pid = pid;
    p->since_ms = now_ms;
    p->last_hb_ms = now_ms;
    p->ever_started = 1;
    return 0;
}

void lg_sup_on_spawn_failed(lg_sup_t *s, uint8_t proc, uint32_t now_ms) {
    lg_sup_proc_t *p;

    if (!installed(s, proc)) {
        return;
    }
    p = &s->proc[proc];
    if (p->phase != LG_SUP_IDLE) {
        return;
    }
    failure(s, p, now_ms, LG_SUP_WHY_SPAWN, 0);
}

int lg_sup_on_heartbeat(lg_sup_t *s, uint8_t proc, uint32_t pid, uint32_t now_ms) {
    lg_sup_proc_t *p;

    if (proc >= LG_PROC_COUNT) {
        s->hb_unknown++;
        return 0;
    }
    p = &s->proc[proc];
    if (!installed(s, proc) || pid == 0 || p->pid != pid ||
        (p->phase != LG_SUP_STARTING && p->phase != LG_SUP_RUNNING)) {
        p->hb_ignored++;
        return 0;
    }
    p->hb_rx++;
    p->last_hb_ms = now_ms;
    if (p->phase == LG_SUP_STARTING) {
        p->phase = LG_SUP_RUNNING;
        if (p->recovery_pending) {
            p->recovery_ms = (uint32_t)(now_ms - p->detect_ms);
            p->recovery_pending = 0;
        }
    }
    return 1;
}

int lg_sup_on_exit(lg_sup_t *s, uint32_t pid, uint32_t now_ms) {
    int i;

    if (pid == 0) {
        s->exits_ignored++;
        return 0;
    }
    for (i = 0; i < LG_PROC_COUNT; i++) {
        lg_sup_proc_t *p = &s->proc[i];

        if (!installed(s, i) || p->pid != pid) {
            continue;
        }
        if (p->phase == LG_SUP_KILLING) {
            failure(s, p, now_ms, LG_SUP_WHY_HANG, 1); /* detected when the kill went out */
            return 1;
        }
        if (p->phase == LG_SUP_STARTING || p->phase == LG_SUP_RUNNING) {
            failure(s, p, now_ms, LG_SUP_WHY_EXIT, 0);
            return 1;
        }
    }
    s->exits_ignored++;
    return 0;
}

/* First spawn only: every process of a lower stage must be RUNNING (or given up). */
static int boot_ready(const lg_sup_t *s, int proc) {
    int q;

    if (s->proc[proc].ever_started) {
        return 1;
    }
    for (q = 0; q < LG_PROC_COUNT; q++) {
        if (installed(s, q) && s->cfg.stage[q] < s->cfg.stage[proc] &&
            s->proc[q].phase != LG_SUP_RUNNING && s->proc[q].phase != LG_SUP_DEAD) {
            return 0;
        }
    }
    return 1;
}

uint8_t lg_sup_reported_state(const lg_sup_t *s, uint8_t proc) {
    if (!installed(s, proc)) {
        return LG_PROC_RUNNING; /* not part of this system yet */
    }
    switch (s->proc[proc].phase) {
    case LG_SUP_RUNNING:
        return LG_PROC_RUNNING;
    case LG_SUP_DEAD:
        return LG_PROC_DEAD;
    default:
        return LG_PROC_RESTARTING;
    }
}

uint8_t lg_sup_restarts_in_window(const lg_sup_t *s, uint8_t proc, uint32_t now_ms) {
    uint8_t n = 0;
    int i;

    if (!installed(s, proc)) {
        return 0;
    }
    for (i = 0; i < s->cfg.budget; i++) {
        if (s->proc[proc].restart_valid[i] &&
            !lg_elapsed_ge(now_ms, s->proc[proc].restart_ms[i], s->cfg.window_ms)) {
            n++;
        }
    }
    return n;
}

/* Returns 1 when the action was stored, 0 when the caller's buffer is full. */
static int push(lg_sup_action_t *out, size_t max, size_t *n, uint8_t kind, uint8_t proc, uint32_t pid) {
    if (*n >= max) {
        return 0;
    }
    out[*n].kind = kind;
    out[*n].proc = proc;
    out[*n].reserved = 0;
    out[*n].pid = pid;
    (*n)++;
    return 1;
}

static void start_kill(lg_sup_proc_t *p, uint8_t why, uint32_t now_ms) {
    p->phase = LG_SUP_KILLING;
    p->why = why;
    p->detect_ms = now_ms;
    p->kill_ms = now_ms;
}

size_t lg_sup_step(lg_sup_t *s, uint32_t now_ms, lg_sup_action_t *out, size_t max) {
    size_t n = 0;
    int i;

    if (s->stepped && s->cfg.stall_ms != 0 && lg_elapsed_ge(now_ms, s->last_step_ms, s->cfg.stall_ms)) {
        /* Everything stood still: do not read the silence of this gap as a hang of each process. */
        s->stalls++;
        for (i = 0; i < LG_PROC_COUNT; i++) {
            if (installed(s, i) && (s->proc[i].phase == LG_SUP_STARTING || s->proc[i].phase == LG_SUP_RUNNING)) {
                s->proc[i].last_hb_ms = now_ms;
                s->proc[i].since_ms = now_ms;
            }
        }
    }
    s->last_step_ms = now_ms;
    s->stepped = 1;

    for (i = 0; i < LG_PROC_COUNT; i++) {
        lg_sup_proc_t *p = &s->proc[i];
        uint8_t rep;
        uint8_t rw;

        if (!installed(s, i)) {
            continue;
        }
        prune(s, p, now_ms);
        switch (p->phase) {
        case LG_SUP_IDLE:
            if (boot_ready(s, i)) {
                push(out, max, &n, LG_SUP_ACT_SPAWN, (uint8_t)i, 0);
            }
            break;
        case LG_SUP_STARTING:
            if (lg_elapsed_ge(now_ms, p->since_ms, s->cfg.start_grace_ms)) {
                start_kill(p, LG_SUP_WHY_HANG, now_ms);
                push(out, max, &n, LG_SUP_ACT_KILL, (uint8_t)i, p->pid);
            }
            break;
        case LG_SUP_RUNNING:
            if (lg_elapsed_ge(now_ms, p->last_hb_ms, s->cfg.heartbeat_timeout_ms)) {
                start_kill(p, LG_SUP_WHY_HANG, now_ms);
                push(out, max, &n, LG_SUP_ACT_KILL, (uint8_t)i, p->pid);
            }
            break;
        case LG_SUP_KILLING:
            if (lg_elapsed_ge(now_ms, p->kill_ms, s->cfg.kill_retry_ms)) {
                p->kill_ms = now_ms;
                push(out, max, &n, LG_SUP_ACT_KILL, (uint8_t)i, p->pid);
            }
            break;
        default:
            break;
        }
        rep = lg_sup_reported_state(s, (uint8_t)i);
        rw = lg_sup_restarts_in_window(s, (uint8_t)i, now_ms);
        if ((rep != p->last_reported || rw != p->last_reported_restarts) &&
            push(out, max, &n, LG_SUP_ACT_STATUS, (uint8_t)i, 0)) {
            /* Only a STATUS that was really handed out counts as reported: a full buffer retries later. */
            p->last_reported = rep;
            p->last_reported_restarts = rw;
        }
    }
    return n;
}

const char *lg_sup_proc_name(uint8_t proc) {
    static const char *const names[LG_PROC_COUNT] = {
        "supervisor",     "controller",     "gate",           "sensor/west/a", "sensor/west/b",
        "sensor/west/island", "sensor/east/a", "sensor/east/b", "sensor/east/island", "plant",
        "health",         "schedule",       "bridge"};

    return proc < LG_PROC_COUNT ? names[proc] : "unknown";
}

static const char *phase_name(uint8_t phase) {
    switch (phase) {
    case LG_SUP_IDLE:
        return "IDLE";
    case LG_SUP_STARTING:
        return "STARTING";
    case LG_SUP_RUNNING:
        return "RUNNING";
    case LG_SUP_KILLING:
        return "KILLING";
    case LG_SUP_DEAD:
        return "DEAD";
    default:
        return "?";
    }
}

static const char *why_name(uint8_t why) {
    switch (why) {
    case LG_SUP_WHY_EXIT:
        return "exit";
    case LG_SUP_WHY_HANG:
        return "hang";
    case LG_SUP_WHY_SPAWN:
        return "spawn";
    default:
        return "none";
    }
}

static const char *state_name(uint8_t st) {
    return st == LG_PROC_RUNNING ? "RUNNING" : (st == LG_PROC_RESTARTING ? "RESTARTING" : "DEAD");
}

size_t lg_sup_text(const lg_sup_t *s, uint32_t now_ms, char *buf, size_t len) {
    size_t used = 0;
    int i;
    int installed_n = 0;

    if (len == 0) {
        return 0;
    }
    buf[0] = '\0';
    for (i = 0; i < LG_PROC_COUNT; i++) {
        installed_n += installed(s, i) ? 1 : 0;
    }
    {
        int w = snprintf(buf, len, "supervisor procs=%d budget=%u window_ms=%u exits_ignored=%u hb_unknown=%u stalls=%u\n",
                         installed_n, (unsigned)s->cfg.budget, (unsigned)s->cfg.window_ms,
                         (unsigned)s->exits_ignored, (unsigned)s->hb_unknown, (unsigned)s->stalls);

        if (w < 0 || (size_t)w >= len) {
            buf[0] = '\0';
            return 0;
        }
        used = (size_t)w;
    }
    for (i = 0; i < LG_PROC_COUNT; i++) {
        const lg_sup_proc_t *p = &s->proc[i];
        char line[220];
        int w;

        if (!installed(s, i)) {
            continue;
        }
        w = snprintf(line, sizeof line,
                     "proc=%s id=%d state=%s phase=%s pid=%u restarts=%u/%u total=%u why=%s recovery_ms=%u "
                     "hb_rx=%u hb_ignored=%u\n",
                     lg_sup_proc_name((uint8_t)i), i, state_name(lg_sup_reported_state(s, (uint8_t)i)),
                     phase_name(p->phase), (unsigned)p->pid,
                     (unsigned)lg_sup_restarts_in_window(s, (uint8_t)i, now_ms), (unsigned)s->cfg.budget,
                     (unsigned)p->total_restarts, why_name(p->why), (unsigned)p->recovery_ms,
                     (unsigned)p->hb_rx, (unsigned)p->hb_ignored);
        if (w < 0 || (size_t)w >= sizeof line || used + (size_t)w >= len) {
            break; /* cut at a line end */
        }
        memcpy(buf + used, line, (size_t)w + 1);
        used += (size_t)w;
    }
    return used;
}
