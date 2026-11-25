/*
 * lg-supervisor (README 4.2, 8.5, 8.6): starts the other processes in order, watches their
 * heartbeats, restarts what dies or hangs within a budget, and tells the controller.
 *
 *   lg-supervisor -b <bindir> -c <crossing.json> -s <scenario.json> [-T <timetable.json>] [-v]
 *
 *   -T  start lg-schedule with this timetable (Step 12, level 3), with the same --epoch-ms as the plant.
 *       Without it lg-schedule is not started and lost detection in a direction is level 4.
 *
 *  - name lg/supervisor      LG_PULSE_HEARTBEAT in, value = process id (lg_heartbeat.h). A beat only
 *                            counts when the pid of the sender (ConnectClientInfo of the pulse's
 *                            scoid) is the current child of that process.
 *  - /dev/lg/supervisor      read: the table (core/supervisor_logic.c, one line per process) and a line
 *                            about the link to the controller. Writes are refused (EPERM). lgctl kill
 *                            reads the pids from here.
 *  - every LG_TICK_MS        lg_sup_step(): spawn, kill (SIGKILL) and status actions.
 *  - LG_MSG_PROC_STATUS      to lg/controller from a sender thread, on every change and the whole table
 *                            every LG_SUP_STATUS_REFRESH_MS (a restarted controller learns the states;
 *                            the refresh is also what the controller reads as "supervisor alive").
 *
 * All decisions are in core/supervisor_logic.c (portable, unit tested); this file only carries them
 * out. Threads: the main thread (one dispatch loop: tick, beats, exits, the status device) never
 * waits for another process; the waiter thread blocks in waitpid(-1) and turns every reaped child
 * into a LG_PULSE_CHILD_EXIT; the sender thread does name_open and MsgSend to the controller and may
 * block when the controller is stuck, which is why it is separate.
 *
 * Boot order: gate, then plant and the six sensors, then the controller (README 8.6). The plant gets
 * the same absolute --epoch-ms on every (re)start, so a restarted plant continues the scenario.
 * When the supervisor dies the children keep running; the controller notices the silence (level 4).
 * Spawned processes inherit the supervisor's descriptors: start it with its output redirected.
 * Heartbeats are checked against the real pid of the sender, exit pulses must come from this process
 * itself. Still open (Step 13): a process that is allowed to open the name can flood it with pulses.
 */

#include <errno.h>
#include <process.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* iofunc.h must come before dispatch.h: it defines RESMGR_OCB_T as the real OCB type. */
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>
#include <sys/siginfo.h>
#include <sys/slog2.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lg_config.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_supervisor_logic.h"
#include "lg_target_util.h"
#include "lg_time.h"

#define PATH_MAX_LG 256
#define BINDIR_MAX 128 /* the directory of the binaries; a path is this plus a short file name */
#define EXIT_LOG_SLOTS 32

static slog2_buffer_t g_log;
static int g_verbose;
static int g_loop_started; /* after this, logging goes to slog2 only */

static lg_sup_t g_sup; /* main thread only */
static char g_bindir[BINDIR_MAX];
static char g_crossing[PATH_MAX_LG];
static char g_scenario[PATH_MAX_LG];
static char g_thresholds[PATH_MAX_LG]; /* thresholds.json next to the crossing config, "" = built-in */
static char g_timetable[PATH_MAX_LG];  /* -T: lg-schedule is started only with a timetable (Step 12) */
static uint32_t g_epoch_ms;
static int g_epoch_set;
static uint32_t g_refresh_ms;
static uint32_t g_status_seq;
static uint32_t g_hb_no_info; /* beats whose sender could not be identified (it had already gone) */
static uint32_t g_foreign_pulses; /* CHILD_EXIT pulses from another process, ignored */

static iofunc_attr_t g_attr;
static resmgr_connect_funcs_t g_connect_funcs;
static resmgr_io_funcs_t g_io_funcs;
static char g_text[3072];

/* How a reaped child ended: written by the waiter, read by the main thread when the pulse arrives. */
static pthread_mutex_t g_exit_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    pid_t pid;
    int status;
} g_exit_log[EXIT_LOG_SLOTS];
static unsigned g_exit_next;

/* Mailbox to the sender thread. */
static pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_out_cv;
static struct {
    uint32_t pending; /* bit per process id */
    uint8_t state[LG_PROC_COUNT];
    uint8_t restarts[LG_PROC_COUNT];
    uint32_t sent;
    uint32_t failed;
    uint32_t rejected;
    uint32_t connects;
    uint8_t linked;
} g_out;

static void logf_(uint8_t severity, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void logf_(uint8_t severity, const char *fmt, ...) {
    char line[200];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_log != NULL) {
        (void)slog2f(g_log, 0, severity, "%s", line);
    }
    /* stderr only while starting up: a stuck terminal must not block the tick. */
    if ((g_verbose || severity <= SLOG2_ERROR) && !g_loop_started) {
        (void)fprintf(stderr, "lg-supervisor: %s\n", line);
    }
}

/* CLOCK_MONOTONIC in ms; a failed read aborts (the controller then sees a silent supervisor). */
static uint32_t now_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        logf_(SLOG2_ERROR, "clock_gettime failed: %s, aborting", strerror(errno));
        abort();
    }
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void fill_hdr(lg_hdr_t *h, uint16_t type, uint32_t seq) {
    h->type = type;
    h->version = LG_PROTO_VERSION;
    h->seq = seq;
    h->t_ns = lg_now_ns();
}

/* ---- what to start ------------------------------------------------------------------------- */

static const char *const k_dir_names[LG_DIR_COUNT] = {LG_DIR_NAME_WEST, LG_DIR_NAME_EAST};
static const char *const k_id_names[LG_SENSOR_ID_COUNT] = {LG_SENSOR_NAME_A, LG_SENSOR_NAME_B,
                                                           LG_SENSOR_NAME_ISLAND};

/* File name of the binary of a process, NULL when the supervisor does not start it. */
static const char *binary_of(int proc) {
    if (proc == LG_PROC_GATE) {
        return "lg-gate";
    }
    if (proc == LG_PROC_PLANT) {
        return "lg-plant";
    }
    if (proc == LG_PROC_CONTROLLER) {
        return "lg-controller";
    }
    if (proc == LG_PROC_BRIDGE) {
        return "lg-bridge";
    }
    if (proc == LG_PROC_HEALTH) {
        return "lg-health";
    }
    if (proc == LG_PROC_SCHEDULE) {
        return "lg-schedule";
    }
    if (proc >= LG_PROC_SENSOR_BASE && proc < LG_PROC_PLANT) {
        return "lg-sensor";
    }
    return NULL;
}

/* argv for a process; text is the storage for the strings that have to be built. */
static void build_argv(int proc, int again, char *path, size_t pathlen, char **argv, char *epoch,
                       size_t epochlen) {
    const char *bin = binary_of(proc);
    int n = 0;

    (void)snprintf(path, pathlen, "%s/%s", g_bindir, bin);
    argv[n++] = path;
    if (proc == LG_PROC_PLANT) {
        (void)snprintf(epoch, epochlen, "%u", (unsigned)g_epoch_ms);
        argv[n++] = (char *)"--crossing";
        argv[n++] = g_crossing;
        argv[n++] = (char *)"--scenario";
        argv[n++] = g_scenario;
        argv[n++] = (char *)"--epoch-ms";
        argv[n++] = epoch;
    } else if (proc == LG_PROC_CONTROLLER) {
        argv[n++] = (char *)"-c";
        argv[n++] = g_crossing;
        if (again) {
            argv[n++] = (char *)"-r"; /* a restart: no boot release, a guard has to acknowledge */
        }
        if (g_sup.cfg.installed[LG_PROC_HEALTH]) {
            argv[n++] = (char *)"-H"; /* lg-health runs: no boot release before its verdicts */
        }
    } else if (proc == LG_PROC_SCHEDULE) {
        (void)snprintf(epoch, epochlen, "%u", (unsigned)g_epoch_ms);
        argv[n++] = (char *)"-t";
        argv[n++] = g_timetable;
        argv[n++] = (char *)"--epoch-ms";
        argv[n++] = epoch;
    } else if (proc == LG_PROC_HEALTH) {
        if (g_thresholds[0] != '\0') {
            argv[n++] = (char *)"-t";
            argv[n++] = g_thresholds;
        }
    } else if (proc >= LG_PROC_SENSOR_BASE && proc < LG_PROC_PLANT) {
        int idx = proc - LG_PROC_SENSOR_BASE;

        argv[n++] = (char *)"-d";
        argv[n++] = (char *)k_dir_names[idx / LG_SENSOR_ID_COUNT];
        argv[n++] = (char *)"-i";
        argv[n++] = (char *)k_id_names[idx % LG_SENSOR_ID_COUNT];
    }
    argv[n] = NULL;
}

/* Every binary the table starts must exist before anything is started: refuse, do not guess. */
static int check_binaries(void) {
    int proc;
    int bad = 0;

    for (proc = 0; proc < LG_PROC_COUNT; proc++) {
        char path[PATH_MAX_LG];

        if (!g_sup.cfg.installed[proc]) {
            continue;
        }
        (void)snprintf(path, sizeof path, "%s/%s", g_bindir, binary_of(proc));
        if (access(path, X_OK) != 0) {
            logf_(SLOG2_ERROR, "%s is not executable: %s", path, strerror(errno));
            bad = 1;
        }
    }
    return bad ? -1 : 0;
}

/* ---- the sender thread (PROC_STATUS to the controller) -------------------------------------- */

static void post_status(uint8_t proc, uint32_t now) {
    (void)pthread_mutex_lock(&g_out_lock);
    g_out.state[proc] = lg_sup_reported_state(&g_sup, proc);
    g_out.restarts[proc] = lg_sup_restarts_in_window(&g_sup, proc, now);
    g_out.pending |= 1u << proc;
    (void)pthread_cond_signal(&g_out_cv);
    (void)pthread_mutex_unlock(&g_out_lock);
}

/* The controller and the supervisor are not reported to the controller: it derives both itself. */
static int reportable(int proc) {
    return g_sup.cfg.installed[proc] && proc != LG_PROC_CONTROLLER && proc != LG_PROC_SUPERVISOR;
}

static void *sender(void *arg) {
    int coid = -1;

    (void)arg;
    for (;;) {
        lg_proc_status_t msg;
        lg_ack_t ack;
        uint8_t proc;
        int rc;

        (void)pthread_mutex_lock(&g_out_lock);
        while (g_out.pending == 0) {
            struct timespec until;

            (void)clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_nsec += 100 * 1000000L;
            if (until.tv_nsec >= 1000000000L) {
                until.tv_sec++;
                until.tv_nsec -= 1000000000L;
            }
            (void)pthread_cond_timedwait(&g_out_cv, &g_out_lock, &until);
        }
        proc = (uint8_t)__builtin_ctz(g_out.pending);
        g_out.pending &= ~(1u << proc);
        memset(&msg, 0, sizeof msg);
        msg.proc_id = proc;
        msg.state = g_out.state[proc];
        msg.restarts_in_window = g_out.restarts[proc];
        (void)pthread_mutex_unlock(&g_out_lock);

        if (coid < 0) {
            coid = name_open(LG_NAME_CONTROLLER, 0);
            if (coid < 0) {
                struct timespec pause = {0, 100 * 1000000L};

                (void)pthread_mutex_lock(&g_out_lock);
                g_out.pending |= 1u << proc; /* try again when the controller is there */
                g_out.linked = 0;
                (void)pthread_mutex_unlock(&g_out_lock);
                (void)nanosleep(&pause, NULL);
                continue;
            }
            (void)pthread_mutex_lock(&g_out_lock);
            g_out.connects++;
            g_out.linked = 1;
            (void)pthread_mutex_unlock(&g_out_lock);
        }
        fill_hdr(&msg.hdr, LG_MSG_PROC_STATUS, ++g_status_seq); /* sender thread only */
        memset(&ack, 0, sizeof ack);
        rc = (int)MsgSend(coid, &msg, sizeof msg, &ack, sizeof ack);
        if (rc == -1) {
            (void)name_close(coid);
            coid = -1;
            (void)pthread_mutex_lock(&g_out_lock);
            g_out.failed++;
            g_out.linked = 0;
            g_out.pending |= 1u << proc;
            (void)pthread_mutex_unlock(&g_out_lock);
            continue;
        }
        (void)pthread_mutex_lock(&g_out_lock);
        g_out.sent++;
        if (ack.hdr.type != LG_MSG_ACK || ack.status != 0) {
            g_out.rejected++; /* the controller refused it: not sent again */
        }
        (void)pthread_mutex_unlock(&g_out_lock);
    }
    return NULL;
}

/* ---- children ------------------------------------------------------------------------------- */

static void do_spawn(int proc, uint32_t now) {
    char path[PATH_MAX_LG];
    char epoch[16];
    char *argv[12];
    pid_t pid;

    if ((proc == LG_PROC_PLANT || proc == LG_PROC_SCHEDULE) && !g_epoch_set) {
        g_epoch_ms = now; /* scenario time 0: the same for every restart of the plant and of lg-schedule */
        g_epoch_set = 1;
    }
    build_argv(proc, g_sup.proc[proc].ever_started, path, sizeof path, argv, epoch, sizeof epoch);
    pid = spawnv(P_NOWAIT, path, argv);
    if (pid == -1) {
        logf_(SLOG2_ERROR, "spawn of %s failed: %s", lg_sup_proc_name((uint8_t)proc), strerror(errno));
        lg_sup_on_spawn_failed(&g_sup, (uint8_t)proc, now);
        return;
    }
    if (lg_sup_on_spawn(&g_sup, (uint8_t)proc, (uint32_t)pid, now) != 0) {
        logf_(SLOG2_ERROR, "spawn of %s: the table did not accept pid %d", lg_sup_proc_name((uint8_t)proc),
              (int)pid);
        (void)kill(pid, SIGKILL);
        return;
    }
    logf_(SLOG2_INFO, "started %s as pid %d", lg_sup_proc_name((uint8_t)proc), (int)pid);
}

/* Blocks in waitpid: every reaped child becomes a pulse to the main thread. */
static void *waiter(void *arg) {
    int coid = *(int *)arg;
    struct timespec pause = {0, 10 * 1000000L};

    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, 0);

        if (pid == -1) {
            (void)nanosleep(&pause, NULL); /* no children yet (ECHILD) or interrupted */
            continue;
        }
        (void)pthread_mutex_lock(&g_exit_lock);
        g_exit_log[g_exit_next % EXIT_LOG_SLOTS].pid = pid;
        g_exit_log[g_exit_next % EXIT_LOG_SLOTS].status = status;
        g_exit_next++;
        (void)pthread_mutex_unlock(&g_exit_lock);
        while (MsgSendPulse(coid, LG_PRIO_SUPERVISOR, LG_PULSE_CHILD_EXIT, (int)pid) == -1) {
            if (errno != EAGAIN) {
                break; /* the main loop is gone; nothing left to tell */
            }
            (void)nanosleep(&pause, NULL);
        }
    }
    return NULL;
}

static int find_status(pid_t pid, int *status) {
    unsigned i;
    int found = 0;

    (void)pthread_mutex_lock(&g_exit_lock);
    for (i = 0; i < EXIT_LOG_SLOTS; i++) {
        if (g_exit_log[i].pid == pid) {
            *status = g_exit_log[i].status;
            found = 1;
        }
    }
    (void)pthread_mutex_unlock(&g_exit_lock);
    return found;
}

static int start_thread(void *(*fn)(void *), void *arg, int policy, int prio, const char *what) {
    pthread_attr_t attr;
    struct sched_param sp;
    pthread_t th;
    int rc;

    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    (void)pthread_attr_init(&attr);
    (void)pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    (void)pthread_attr_setschedpolicy(&attr, policy);
    (void)pthread_attr_setschedparam(&attr, &sp);
    (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&th, &attr, fn, arg);
    (void)pthread_attr_destroy(&attr);
    if (rc != 0) {
        logf_(SLOG2_ERROR, "pthread_create (%s) failed: %s", what, strerror(rc));
        return -1;
    }
    return 0;
}

/* ---- pulses --------------------------------------------------------------------------------- */

static int on_heartbeat(message_context_t *ctp, int code, unsigned flags, void *handle) {
    uint32_t v = (uint32_t)ctp->msg->pulse.value.sival_int;
    struct _client_info info;

    (void)code;
    (void)flags;
    (void)handle;
    /* The pid comes from the kernel, not from the pulse value. A sender that is gone has no info. */
    if (ConnectClientInfo(ctp->msg->pulse.scoid, &info, 0) == -1) {
        g_hb_no_info++;
        return 0;
    }
    (void)lg_sup_on_heartbeat(&g_sup, v < 256u ? (uint8_t)v : (uint8_t)255, (uint32_t)info.pid, now_ms());
    return 0;
}

static int on_child_exit(message_context_t *ctp, int code, unsigned flags, void *handle) {
    pid_t pid = (pid_t)ctp->msg->pulse.value.sival_int;
    uint32_t now = now_ms();
    struct _client_info info;
    int status = 0;
    int have;
    const char *name = "?";
    int proc;

    (void)code;
    (void)flags;
    (void)handle;
    /*
     * The channel is public (heartbeats come in on it), so any process could send this pulse and make
     * the supervisor start a second copy of a process that is still running. Only the waiter thread
     * of this process may report an exit.
     */
    if (ConnectClientInfo(ctp->msg->pulse.scoid, &info, 0) == -1 || info.pid != getpid()) {
        g_foreign_pulses++;
        logf_(SLOG2_WARNING, "ignored a CHILD_EXIT pulse that did not come from the waiter thread");
        return 0;
    }
    have = find_status(pid, &status);
    for (proc = 0; proc < LG_PROC_COUNT; proc++) {
        if (g_sup.cfg.installed[proc] && g_sup.proc[proc].pid == (uint32_t)pid) {
            name = lg_sup_proc_name((uint8_t)proc);
        }
    }
    if (lg_sup_on_exit(&g_sup, (uint32_t)pid, now) == 0) {
        logf_(SLOG2_WARNING, "pid %d ended and is not a child of the table", (int)pid);
        return 0;
    }
    if (have && WIFSIGNALED(status)) {
        logf_(SLOG2_ERROR, "%s (pid %d) was killed by signal %d", name, (int)pid, (int)WTERMSIG(status));
    } else if (have && WIFEXITED(status)) {
        logf_(SLOG2_ERROR, "%s (pid %d) exited with status %d", name, (int)pid, (int)WEXITSTATUS(status));
    } else {
        logf_(SLOG2_ERROR, "%s (pid %d) ended", name, (int)pid);
    }
    return 0;
}

static int on_tick(message_context_t *ctp, int code, unsigned flags, void *handle) {
    lg_sup_action_t act[LG_SUP_MAX_ACTIONS];
    uint32_t now = now_ms();
    size_t n;
    size_t i;

    (void)ctp;
    (void)code;
    (void)flags;
    (void)handle;

    n = lg_sup_step(&g_sup, now, act, LG_SUP_MAX_ACTIONS);
    for (i = 0; i < n; i++) {
        switch (act[i].kind) {
        case LG_SUP_ACT_SPAWN:
            do_spawn(act[i].proc, now);
            break;
        case LG_SUP_ACT_KILL:
            logf_(SLOG2_ERROR, "no heartbeat from %s (pid %u): SIGKILL", lg_sup_proc_name(act[i].proc),
                  (unsigned)act[i].pid);
            if (kill((pid_t)act[i].pid, SIGKILL) == -1 && errno != ESRCH) {
                logf_(SLOG2_ERROR, "kill(%u) failed: %s", (unsigned)act[i].pid, strerror(errno));
            }
            break;
        case LG_SUP_ACT_STATUS:
            if (reportable(act[i].proc)) {
                post_status(act[i].proc, now);
                logf_(SLOG2_INFO, "%s is now %s (restarts in window %u)", lg_sup_proc_name(act[i].proc),
                      lg_sup_reported_state(&g_sup, act[i].proc) == LG_PROC_RUNNING
                          ? "RUNNING"
                          : (lg_sup_reported_state(&g_sup, act[i].proc) == LG_PROC_DEAD ? "DEAD" : "RESTARTING"),
                      (unsigned)lg_sup_restarts_in_window(&g_sup, act[i].proc, now));
            }
            break;
        default:
            break;
        }
    }
    /* The whole table again every refresh period: late joiners, and the controller's proof of life. */
    if (lg_elapsed_ge(now, g_refresh_ms, LG_SUP_STATUS_REFRESH_MS)) {
        int p;

        g_refresh_ms = now;
        for (p = 0; p < LG_PROC_COUNT; p++) {
            if (reportable(p)) {
                post_status((uint8_t)p, now);
            }
        }
    }
    return 0;
}

/* ---- /dev/lg/supervisor --------------------------------------------------------------------- */

static size_t status_text(char *buf, size_t len) {
    size_t n = lg_sup_text(&g_sup, now_ms(), buf, len);
    uint32_t sent;
    uint32_t failed;
    uint32_t rejected;
    uint32_t connects;
    uint8_t linked;
    int w;

    (void)pthread_mutex_lock(&g_out_lock);
    sent = g_out.sent;
    failed = g_out.failed;
    rejected = g_out.rejected;
    connects = g_out.connects;
    linked = g_out.linked;
    (void)pthread_mutex_unlock(&g_out_lock);
    w = snprintf(buf + n, len - n,
                 "controller_link linked=%u sent=%u failed=%u rejected=%u connects=%u hb_no_info=%u "
                 "foreign_pulses=%u\n",
                 (unsigned)linked, (unsigned)sent, (unsigned)failed, (unsigned)rejected, (unsigned)connects,
                 (unsigned)g_hb_no_info, (unsigned)g_foreign_pulses);
    if (w < 0 || (size_t)w >= len - n) {
        buf[n] = '\0';
        return n;
    }
    return n + (size_t)w;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
    size_t len;
    size_t off;
    size_t nbytes;
    int rc = iofunc_read_verify(ctp, msg, ocb, NULL);

    if (rc != EOK) {
        return rc;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }
    len = status_text(g_text, sizeof g_text); /* regenerated on every read: read it in one go */
    off = (size_t)ocb->offset;
    if (off >= len) {
        nbytes = 0;
    } else {
        nbytes = len - off;
        if (nbytes > msg->i.nbytes) {
            nbytes = msg->i.nbytes;
        }
    }
    _IO_SET_READ_NBYTES(ctp, (int)nbytes);
    if (nbytes == 0) {
        return _RESMGR_NPARTS(0);
    }
    SETIOV(ctp->iov, g_text + off, nbytes);
    ocb->offset += (off_t)nbytes;
    return _RESMGR_NPARTS(1);
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
    (void)msg;
    (void)ocb;
    logf_(SLOG2_WARNING, "write to %s from pid %d refused", LG_DEV_SUPERVISOR, (int)ctp->info.pid);
    return EPERM;
}

/* ---- setup ---------------------------------------------------------------------------------- */

static void setup_logging(void) {
    slog2_buffer_set_config_t cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.num_buffers = 1;
    cfg.buffer_set_name = "lg-supervisor";
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "main";
    cfg.buffer_config[0].num_pages = 8;
    if (slog2_register(&cfg, &g_log, 0) != 0) {
        g_log = NULL;
        (void)fprintf(stderr, "lg-supervisor: slog2_register failed: %s\n", strerror(errno));
    }
}

static void setup_priority(void) {
    struct sched_param sp;
    int rc;

    memset(&sp, 0, sizeof sp);
    sp.sched_priority = LG_PRIO_SUPERVISOR;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) {
        logf_(SLOG2_WARNING, "could not set SCHED_FIFO %d: %s (continuing at default priority)",
              LG_PRIO_SUPERVISOR, strerror(rc));
    } else {
        logf_(SLOG2_INFO, "running SCHED_FIFO priority %d", LG_PRIO_SUPERVISOR);
    }
}

static int start_tick_timer(int coid) {
    struct sigevent ev;
    struct itimerspec its;
    timer_t timer;

    SIGEV_PULSE_INIT(&ev, coid, LG_PRIO_SUPERVISOR, LG_PULSE_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        logf_(SLOG2_ERROR, "timer_create failed: %s", strerror(errno));
        return -1;
    }
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = (long)LG_TICK_MS * 1000000L;
    its.it_interval.tv_nsec = (long)LG_TICK_MS * 1000000L;
    if (timer_settime(timer, 0, &its, NULL) == -1) {
        logf_(SLOG2_ERROR, "timer_settime failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int copy_arg(char *dst, size_t cap, const char *src) {
    if (strlen(src) >= cap) {
        return -1;
    }
    (void)snprintf(dst, cap, "%s", src);
    return 0;
}

static void default_bindir(const char *argv0) {
    const char *slash = strrchr(argv0, '/');

    if (slash == NULL) {
        (void)snprintf(g_bindir, sizeof g_bindir, ".");
    } else if ((size_t)(slash - argv0) < sizeof g_bindir) {
        (void)snprintf(g_bindir, sizeof g_bindir, "%.*s", (int)(slash - argv0), argv0);
    }
}

int main(int argc, char **argv) {
    lg_sup_cfg_t cfg = lg_sup_cfg_default();
    pthread_condattr_t cattr;
    dispatch_t *dpp;
    dispatch_context_t *ctp;
    resmgr_attr_t rattr;
    name_attach_t *name;
    int chid;
    int self_coid;
    int wait_coid;
    int have_c = 0;
    int have_s = 0;
    int i;

    default_bindir(argv[0]);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            if (copy_arg(g_bindir, sizeof g_bindir, argv[++i]) != 0) {
                have_c = -1;
            }
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            have_c = copy_arg(g_crossing, sizeof g_crossing, argv[++i]) == 0 ? 1 : -1;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            have_s = copy_arg(g_scenario, sizeof g_scenario, argv[++i]) == 0 ? 1 : -1;
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            if (copy_arg(g_timetable, sizeof g_timetable, argv[++i]) != 0) {
                have_c = -1;
            }
        } else if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else {
            have_c = -1;
            break;
        }
    }
    if (have_c != 1 || have_s != 1) {
        (void)fprintf(stderr,
                      "usage: lg-supervisor -b <bindir> -c <crossing.json> -s <scenario.json> [-T <timetable.json>] "
                      "[-v]\n");
        return 2;
    }
    /* Step 12: lg-schedule (level 3) only with a timetable. Without -T a direction without trustworthy
     * detection is level 4, as in Steps 7 to 11 (Decisions log). */
    if (g_timetable[0] != '\0') {
        if (access(g_timetable, R_OK) != 0) {
            (void)fprintf(stderr, "lg-supervisor: timetable %s is not readable\n", g_timetable);
            return 2;
        }
        cfg.installed[LG_PROC_SCHEDULE] = 1;
        cfg.stage[LG_PROC_SCHEDULE] = 2; /* after the plant (the epoch), next to the controller */
    }

    setup_logging();
    setup_priority();
    {
        /* lg-health reads thresholds.json from the directory of the crossing config when it is there. */
        const char *slash = strrchr(g_crossing, '/');
        int dirlen = slash != NULL ? (int)(slash - g_crossing) + 1 : 0;

        (void)snprintf(g_thresholds, sizeof g_thresholds, "%.*sthresholds.json", dirlen, g_crossing);
        if (access(g_thresholds, R_OK) != 0) {
            logf_(SLOG2_WARNING, "%s not readable: lg-health uses its built-in thresholds", g_thresholds);
            g_thresholds[0] = 0; /* none: built-in thresholds */
        }
    }
    lg_sup_init(&g_sup, &cfg);
    if (check_binaries() != 0) {
        return 1;
    }
    (void)pthread_condattr_init(&cattr);
    (void)pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    (void)pthread_cond_init(&g_out_cv, &cattr);

    chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT);
    if (chid == -1) {
        logf_(SLOG2_ERROR, "ChannelCreate failed: %s", strerror(errno));
        return 1;
    }
    dpp = dispatch_create_channel(chid, 0);
    if (dpp == NULL) {
        logf_(SLOG2_ERROR, "dispatch_create_channel failed: %s", strerror(errno));
        return 1;
    }
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect_funcs, _RESMGR_IO_NFUNCS, &g_io_funcs);
    g_io_funcs.read = io_read;
    g_io_funcs.write = io_write;
    iofunc_attr_init(&g_attr, S_IFCHR | 0444, NULL, NULL);
    if (resmgr_attach(dpp, &rattr, LG_DEV_SUPERVISOR, _FTYPE_ANY, 0, &g_connect_funcs, &g_io_funcs,
                      &g_attr) == -1) {
        logf_(SLOG2_ERROR, "resmgr_attach(%s) failed: %s", LG_DEV_SUPERVISOR, strerror(errno));
        return 1;
    }
    name = name_attach(dpp, LG_NAME_SUPERVISOR, 0);
    if (name == NULL) {
        logf_(SLOG2_ERROR, "name_attach(%s) failed: %s", LG_NAME_SUPERVISOR, strerror(errno));
        return 1;
    }
    if (pulse_attach(dpp, 0, LG_PULSE_HEARTBEAT, on_heartbeat, NULL) == -1 ||
        pulse_attach(dpp, 0, LG_PULSE_CHILD_EXIT, on_child_exit, NULL) == -1 ||
        pulse_attach(dpp, 0, LG_PULSE_TICK, on_tick, NULL) == -1) {
        logf_(SLOG2_ERROR, "pulse_attach failed: %s", strerror(errno));
        return 1;
    }
    self_coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
    wait_coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
    if (self_coid == -1 || wait_coid == -1) {
        logf_(SLOG2_ERROR, "message_connect failed: %s", strerror(errno));
        return 1;
    }
    {
        static int waiter_coid;

        waiter_coid = wait_coid;
        if (start_thread(waiter, &waiter_coid, SCHED_FIFO, LG_PRIO_SUPERVISOR, "waiter") != 0 ||
            start_thread(sender, NULL, SCHED_FIFO, LG_PRIO_SUP_SENDER, "sender") != 0) {
            return 1;
        }
    }
    if (start_tick_timer(self_coid) != 0) {
        return 1;
    }
    ctp = dispatch_context_alloc(dpp);
    if (ctp == NULL) {
        logf_(SLOG2_ERROR, "dispatch_context_alloc failed: %s", strerror(errno));
        return 1;
    }
    g_refresh_ms = now_ms();
    logf_(SLOG2_INFO, "up: %s and name %s attached, bindir %s, tick %d ms", LG_DEV_SUPERVISOR,
          LG_NAME_SUPERVISOR, g_bindir, LG_TICK_MS);
    g_loop_started = 1;

    for (;;) {
        dispatch_context_t *next = dispatch_block(ctp);

        if (next == NULL) {
            if (errno == EINTR) {
                continue;
            }
            logf_(SLOG2_ERROR, "dispatch_block failed: %s", strerror(errno));
            return 1;
        }
        ctp = next;
        (void)dispatch_handler(ctp);
    }
}
