/*
 * lg-controller (README 4.2, 4.3, 8.1, 8.2): the only process that commands the gate.
 *
 *   lg-controller [-v]
 *
 *  - name lg/controller      message passing: LG_MSG_SENSOR_SAMPLE in (reply: lg_ack_t, a refusal is
 *                            lg_ack_t.status, never MsgError, so a sensor does not reconnect).
 *  - /dev/lg/controller      read: one line of status text. Writes are refused (EPERM).
 *  - every LG_TICK_MS: run the decision logic (core/controller_logic.c) and tell the gate what it
 *    should do. LOWER or RAISE is sent on every tick: it is the watchdog keepalive of lg-gate and its
 *    reply carries the gate state. Lights and bell are sent on change and refreshed periodically.
 *
 * Threads. The main thread serves everything that must never wait for another process: samples,
 * the tick, the status device, and the decision logic, on a channel with _NTO_CHF_FIXED_PRIORITY, so
 * a lower-priority sensor or reader never pulls the controller below its own priority (measured,
 * evidence/step05_priority.txt). The gate worker (SCHED_FIFO, same priority) does ALL the MsgSend
 * calls to the gate: a gate that was stopped after it received a command keeps its sender blocked
 * in the reply phase (the channel has _NTO_CHF_UNBLOCK, TimerTimeout does not cover that phase,
 * measured for lg-sensor in Step 6, and seen here: a SIGSTOP of the gate froze the first version of
 * this controller for good). Only the worker waits then. The main thread keeps ticking, sees that no
 * reply arrives, treats the gate as silent after LG_GATE_WATCHDOG_MS (level 4) and overwrites the
 * request that the worker has not picked up yet (counted as gate_dropped). The connector thread
 * calls name_open and name_close, which wait for a stuck server. Shared state: the request and
 * reply mailbox (g_io_lock) and the connection id (g_slot_lock); the lock is never held across a
 * blocking call. The tick pulse carries the priority explicitly (SIGEV_PULSE_PRIO_INHERIT would
 * give the creation priority, evidence/step06_channels.txt). A tick pulse that comes less than
 * LG_CTL_BURST_GUARD_MS after the last one is ignored: no burst replay after a stall. The tick
 * channel is the public one (the name lives on it), so another process could send extra tick pulses;
 * the logic uses the real clock, so an extra tick only adds an evaluation. Authentication of
 * senders is Step 13.
 *
 * Not in this step: heartbeats and restarts (Step 8), guard acknowledgement (the message type exists
 * but has no handler: nothing may release level 4 before lgctl and authentication exist), health
 * verdicts (Step 11, interim verdicts are in the logic), timetable (Step 12).
 */

#include <errno.h>
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
#include <time.h>

#include "lg_config.h"
#include "lg_controller_logic.h"
#include "lg_metrics.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_target_util.h"

#define LG_DEV_CONTROLLER "/dev/lg/controller"

static slog2_buffer_t g_log;
static int g_verbose;
static int g_loop_started; /* after this, logging goes to slog2 only */

static lg_ctl_t g_ctl; /* main thread only */
static uint64_t g_prev_tick_ns;
static uint32_t g_ack_seq;
static uint32_t g_refresh_ticks;
static uint32_t g_ignored_ticks;

/* The gate connection, shared between the main thread (uses it) and the connector (makes it). */
static pthread_mutex_t g_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_gate_coid = -1;
static int g_gate_stale = -1;
static uint32_t g_gate_connects;

/*
 * Mailbox between the main thread (posts a request every tick, collects the latest reply) and the
 * gate worker (does the blocking IPC). Protected by g_io_lock.
 */
static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_io_cv = PTHREAD_COND_INITIALIZER;
static struct {
    uint8_t pending;       /* a request is waiting for the worker */
    uint8_t target_cmd;    /* LG_GATE_CMD_LOWER or LG_GATE_CMD_RAISE */
    uint8_t lights;        /* wanted */
    uint8_t bell;
    uint8_t send_lights;   /* send the lights command in this request */
    uint8_t send_bell;
    uint8_t sent_lights;   /* last value the gate acknowledged, 255 = never */
    uint8_t sent_bell;
    uint8_t resp_valid;    /* a reply the main thread has not collected yet */
    uint8_t resp_state;
    uint32_t resp_ms;      /* when the worker got it */
    uint32_t timeouts;
    uint32_t dropped;      /* requests overwritten before the worker picked them up */
    uint32_t cmd_seq;
} g_io = {0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0};

static iofunc_attr_t g_attr;
static resmgr_connect_funcs_t g_connect_funcs;
static resmgr_io_funcs_t g_io_funcs;
static char g_text[400];

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
        (void)fprintf(stderr, "lg-controller: %s\n", line);
    }
}

/* CLOCK_MONOTONIC in ms; a failed read aborts (a restart, and the gate watchdog, are the safer state). */
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

/* ---- gate connection ------------------------------------------------------------------------ */

static int gate_get(void) {
    int coid;

    (void)pthread_mutex_lock(&g_slot_lock);
    coid = g_gate_coid;
    (void)pthread_mutex_unlock(&g_slot_lock);
    return coid;
}

/* The gate is gone: forget the connection and hand it to the connector to close. */
static void gate_fail(int coid) {
    (void)pthread_mutex_lock(&g_slot_lock);
    if (g_gate_coid == coid) {
        g_gate_coid = -1;
        g_gate_stale = coid;
    }
    (void)pthread_mutex_unlock(&g_slot_lock);
}

/* The only place where name_open and name_close are called: they wait for a stuck server. */
static void *connector(void *arg) {
    struct timespec pause;

    (void)arg;
    pause.tv_sec = 0;
    pause.tv_nsec = (long)LG_CTL_CONNECTOR_PERIOD_MS * 1000000L;
    for (;;) {
        int stale;
        int need;

        (void)pthread_mutex_lock(&g_slot_lock);
        stale = g_gate_stale;
        g_gate_stale = -1;
        need = g_gate_coid < 0;
        (void)pthread_mutex_unlock(&g_slot_lock);

        if (stale >= 0) {
            (void)name_close(stale);
        }
        if (need) {
            int c = name_open(LG_NAME_GATE, 0);

            if (c >= 0) {
                (void)pthread_mutex_lock(&g_slot_lock);
                g_gate_coid = c;
                g_gate_connects++;
                (void)pthread_mutex_unlock(&g_slot_lock);
            }
        }
        (void)nanosleep(&pause, NULL);
    }
    return NULL;
}

static int start_thread(void *(*fn)(void *), int policy, int prio, const char *what) {
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
    rc = pthread_create(&th, &attr, fn, NULL);
    (void)pthread_attr_destroy(&attr);
    if (rc != 0) {
        logf_(SLOG2_ERROR, "pthread_create (%s) failed: %s", what, strerror(rc));
        return -1;
    }
    return 0;
}

static uint32_t gate_connects(void) {
    uint32_t n;

    (void)pthread_mutex_lock(&g_slot_lock);
    n = g_gate_connects;
    (void)pthread_mutex_unlock(&g_slot_lock);
    return n;
}

/*
 * One command to the gate (gate worker only). Returns 1 when the gate answered with a valid state;
 * the state is left in the mailbox for the main thread. May block for good if the gate stops after
 * it received the command: that is why only the worker calls it.
 */
static int send_gate(uint8_t cmd) {
    lg_gate_cmd_msg_t msg;
    lg_gate_state_msg_t rep;
    uint64_t timeout_ns = (uint64_t)LG_CTL_IPC_TIMEOUT_MS * 1000000ull;
    int coid = gate_get();
    long rc;

    if (coid < 0) {
        return 0;
    }
    memset(&msg, 0, sizeof msg);
    fill_hdr(&msg.hdr, LG_MSG_GATE_CMD, ++g_io.cmd_seq); /* worker only */
    msg.cmd = cmd;
    memset(&rep, 0, sizeof rep);
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout_ns,
                     NULL) == -1) {
        return 0;
    }
    rc = MsgSend(coid, &msg, sizeof msg, &rep, sizeof rep);
    if (rc == -1) {
        int e = errno; /* before anything else can change it */

        (void)pthread_mutex_lock(&g_io_lock);
        g_io.timeouts++;
        (void)pthread_mutex_unlock(&g_io_lock);
        if (e != ETIMEDOUT) {
            gate_fail(coid); /* the gate is gone: the connector reconnects */
        }
        return 0;
    }
    /*
     * MsgSend returns the status given to MsgReply (0), not a byte count: a short reply is caught
     * by the header check, because rep was zeroed before the call.
     */
    if (rep.hdr.type != LG_MSG_GATE_STATE || rep.hdr.version != LG_PROTO_VERSION ||
        rep.state > LG_GATE_FAULT) {
        gate_fail(coid); /* it does not speak our protocol: start again */
        return 0;
    }
    (void)pthread_mutex_lock(&g_io_lock);
    g_io.resp_state = rep.state;
    g_io.resp_ms = now_ms();
    g_io.resp_valid = 1;
    (void)pthread_mutex_unlock(&g_io_lock);
    return 1;
}

static void set_sent(uint8_t *field, uint8_t value) {
    (void)pthread_mutex_lock(&g_io_lock);
    *field = value;
    (void)pthread_mutex_unlock(&g_io_lock);
}

/* The only thread that talks to the gate. Warning devices go ON before the gate moves, OFF after. */
static void *gate_worker(void *arg) {
    (void)arg;
    for (;;) {
        uint8_t target_cmd;
        uint8_t lights;
        uint8_t bell;
        uint8_t send_lights;
        uint8_t send_bell;

        (void)pthread_mutex_lock(&g_io_lock);
        while (!g_io.pending) {
            (void)pthread_cond_wait(&g_io_cv, &g_io_lock);
        }
        g_io.pending = 0;
        target_cmd = g_io.target_cmd;
        lights = g_io.lights;
        bell = g_io.bell;
        send_lights = g_io.send_lights;
        send_bell = g_io.send_bell;
        (void)pthread_mutex_unlock(&g_io_lock);

        if (lights && send_lights && send_gate(LG_GATE_CMD_LIGHTS_ON)) {
            set_sent(&g_io.sent_lights, lights);
        }
        if (bell && send_bell && send_gate(LG_GATE_CMD_BELL_ON)) {
            set_sent(&g_io.sent_bell, bell);
        }
        /* The target is the watchdog keepalive and its reply carries the gate state. */
        if (!send_gate(target_cmd)) {
            continue; /* the gate is not answering: no further timeouts for this request */
        }
        if (!lights && send_lights && send_gate(LG_GATE_CMD_LIGHTS_OFF)) {
            set_sent(&g_io.sent_lights, lights);
        }
        if (!bell && send_bell && send_gate(LG_GATE_CMD_BELL_OFF)) {
            set_sent(&g_io.sent_bell, bell);
        }
    }
    return NULL;
}

/* Main thread, before the logic step: feed the latest gate reply and the timeout count. */
static void collect_gate_reply(void) {
    uint8_t valid;
    uint8_t state;
    uint32_t ms;
    uint32_t timeouts;

    (void)pthread_mutex_lock(&g_io_lock);
    valid = g_io.resp_valid;
    state = g_io.resp_state;
    ms = g_io.resp_ms;
    timeouts = g_io.timeouts;
    g_io.resp_valid = 0;
    (void)pthread_mutex_unlock(&g_io_lock);
    if (valid) {
        (void)lg_ctl_on_gate_reply(&g_ctl, state, ms);
    }
    g_ctl.gate_timeouts = timeouts;
}

/* Main thread, after the logic step: hand the wishes of this tick to the worker (latest wins). */
static void post_request(const lg_ctl_out_t *out) {
    int refresh;

    g_refresh_ticks++;
    refresh = g_refresh_ticks >= LG_CTL_LIGHTS_REFRESH_TICKS;
    if (refresh) {
        g_refresh_ticks = 0;
    }
    (void)pthread_mutex_lock(&g_io_lock);
    if (g_io.pending) {
        g_io.dropped++; /* the worker is still busy (or stuck) with an older request */
    }
    g_io.pending = 1;
    g_io.target_cmd = out->gate_target == LG_GATE_TARGET_UP ? LG_GATE_CMD_RAISE : LG_GATE_CMD_LOWER;
    g_io.lights = out->lights;
    g_io.bell = out->bell;
    g_io.send_lights = (uint8_t)(g_io.sent_lights != out->lights || refresh);
    g_io.send_bell = (uint8_t)(g_io.sent_bell != out->bell || refresh);
    (void)pthread_cond_signal(&g_io_cv);
    (void)pthread_mutex_unlock(&g_io_lock);
}

/* ---- messages and pulses -------------------------------------------------------------------- */

static void reply_ack(message_context_t *ctp, int status) {
    lg_ack_t ack;

    memset(&ack, 0, sizeof ack);
    fill_hdr(&ack.hdr, LG_MSG_ACK, ++g_ack_seq);
    ack.status = status;
    (void)MsgReply(ctp->rcvid, EOK, &ack, sizeof ack);
}

static int on_sample(message_context_t *ctp, int code, unsigned flags, void *handle) {
    const lg_sensor_sample_t *s = (const lg_sensor_sample_t *)ctp->msg;

    (void)flags;
    (void)handle;

    if ((size_t)ctp->info.msglen < sizeof *s || s->hdr.type != (uint16_t)code ||
        s->hdr.version != LG_PROTO_VERSION) {
        g_ctl.samples_rejected++; /* malformed: lg_ctl_on_sample never saw it */
        reply_ack(ctp, EINVAL);
        return 0;
    }
    /* lg_ctl_on_sample counts the out-of-range fields itself. */
    if (lg_ctl_on_sample(&g_ctl, s->dir, s->id, s->occupied, s->quality, s->axle_count,
                         (uint32_t)(s->hdr.t_ns / 1000000ull), now_ms()) != 0) {
        reply_ack(ctp, EINVAL);
        return 0;
    }
    reply_ack(ctp, 0);
    return 0;
}

static int on_tick(message_context_t *ctp, int code, unsigned flags, void *handle) {
    uint64_t now_ns = lg_now_ns();
    uint32_t now = now_ms();
    lg_ctl_out_t out;

    (void)ctp;
    (void)code;
    (void)flags;
    (void)handle;

    if (g_prev_tick_ns != 0) {
        if (now_ns - g_prev_tick_ns < (uint64_t)LG_CTL_BURST_GUARD_MS * 1000000ull) {
            g_ignored_ticks++; /* queued pulse after a stall, or an extra tick: not a new period */
            return 0;
        }
        if (lg_tick_late(g_prev_tick_ns, now_ns)) {
            lg_ctl_note_late(&g_ctl);
        }
    }
    g_prev_tick_ns = now_ns;

    collect_gate_reply();
    out = lg_ctl_step(&g_ctl, now);
    if (out.level_changed) {
        logf_(out.level >= LG_LEVEL_UNCERTAIN ? SLOG2_ERROR : SLOG2_WARNING,
              "level %u -> %u (reason %u)", (unsigned)out.level_from, (unsigned)out.level,
              (unsigned)out.level_reason);
    }
    if (out.activated_mask != 0) {
        logf_(SLOG2_INFO, "activation mask 0x%x (west=1 east=2), level %u", (unsigned)out.activated_mask,
              (unsigned)out.level);
    }
    if (out.cleared_mask != 0) {
        logf_(SLOG2_INFO, "cleared mask 0x%x", (unsigned)out.cleared_mask);
    }
    post_request(&out);
    return 0;
}

/* ---- /dev/lg/controller --------------------------------------------------------------------- */

static uint32_t gate_dropped(void) {
    uint32_t n;

    (void)pthread_mutex_lock(&g_io_lock);
    n = g_io.dropped;
    (void)pthread_mutex_unlock(&g_io_lock);
    return n;
}

static size_t status_text(char *buf, size_t len) {
    size_t n = lg_ctl_text(&g_ctl, buf, len);
    struct sched_param sp;
    int policy = 0;
    int extra;

    if (n == 0 || buf[n - 1] != '\n' || n + 1 >= len) {
        return n;
    }
    memset(&sp, 0, sizeof sp);
    if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0) {
        policy = -1;
        sp.sched_priority = -1;
    }
    /* Replace the newline with the process-level counters (still one line). */
    extra = snprintf(buf + n - 1, len - (n - 1),
                     " ignored_ticks=%u gate_connects=%u gate_dropped=%u sched_policy=%d "
                     "sched_priority=%d\n",
                     (unsigned)g_ignored_ticks, (unsigned)gate_connects(), (unsigned)gate_dropped(),
                     policy, sp.sched_priority);
    if (extra < 0 || (size_t)extra >= len - (n - 1)) {
        buf[n - 1] = '\n';
        buf[n] = '\0';
        return n;
    }
    return n - 1 + (size_t)extra;
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
    logf_(SLOG2_WARNING, "write to %s from pid %d refused", LG_DEV_CONTROLLER, (int)ctp->info.pid);
    return EPERM;
}

/* ---- setup ---------------------------------------------------------------------------------- */

static void setup_logging(void) {
    slog2_buffer_set_config_t cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.num_buffers = 1;
    cfg.buffer_set_name = "lg-controller";
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "main";
    cfg.buffer_config[0].num_pages = 4;
    if (slog2_register(&cfg, &g_log, 0) != 0) {
        g_log = NULL;
        (void)fprintf(stderr, "lg-controller: slog2_register failed: %s\n", strerror(errno));
    }
}

static void setup_priority(void) {
    struct sched_param sp;
    int rc;

    memset(&sp, 0, sizeof sp);
    sp.sched_priority = LG_PRIO_CONTROLLER;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) {
        logf_(SLOG2_WARNING, "could not set SCHED_FIFO %d: %s (continuing at default priority)",
              LG_PRIO_CONTROLLER, strerror(rc));
    } else {
        logf_(SLOG2_INFO, "running SCHED_FIFO priority %d", LG_PRIO_CONTROLLER);
    }
}

static int start_tick_timer(dispatch_t *dpp) {
    struct sigevent ev;
    struct itimerspec its;
    timer_t timer;
    int coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);

    if (coid == -1) {
        logf_(SLOG2_ERROR, "message_connect failed: %s", strerror(errno));
        return -1;
    }
    /* Explicit priority: INHERIT would give the priority the process was created with. */
    SIGEV_PULSE_INIT(&ev, coid, LG_PRIO_CONTROLLER, LG_PULSE_TICK, 0);
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

int main(int argc, char **argv) {
    lg_ctl_cfg_t cfg = lg_ctl_cfg_default();
    dispatch_t *dpp;
    dispatch_context_t *ctp;
    resmgr_attr_t rattr;
    name_attach_t *name;
    int chid;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else {
            (void)fprintf(stderr, "usage: lg-controller [-v]\n");
            return 2;
        }
    }

    setup_logging();
    setup_priority();

    /* Starts in level 4 with the boot lock: gate target DOWN, lights and bell on. */
    lg_ctl_init(&g_ctl, &cfg, now_ms());

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

    if (resmgr_attach(dpp, &rattr, LG_DEV_CONTROLLER, _FTYPE_ANY, 0, &g_connect_funcs, &g_io_funcs,
                      &g_attr) == -1) {
        logf_(SLOG2_ERROR, "resmgr_attach(%s) failed: %s", LG_DEV_CONTROLLER, strerror(errno));
        return 1;
    }
    name = name_attach(dpp, LG_NAME_CONTROLLER, 0);
    if (name == NULL) {
        logf_(SLOG2_ERROR, "name_attach(%s) failed: %s", LG_NAME_CONTROLLER, strerror(errno));
        return 1;
    }
    if (message_attach(dpp, NULL, LG_MSG_SENSOR_SAMPLE, LG_MSG_SENSOR_SAMPLE, on_sample, NULL) == -1) {
        logf_(SLOG2_ERROR, "message_attach failed: %s", strerror(errno));
        return 1;
    }
    if (pulse_attach(dpp, 0, LG_PULSE_TICK, on_tick, NULL) == -1) {
        logf_(SLOG2_ERROR, "pulse_attach failed: %s", strerror(errno));
        return 1;
    }
    if (start_thread(gate_worker, SCHED_FIFO, LG_PRIO_CONTROLLER, "gate worker") != 0 ||
        start_thread(connector, SCHED_RR, LG_PRIO_CONNECTOR, "gate connector") != 0 ||
        start_tick_timer(dpp) != 0) {
        return 1;
    }
    ctp = dispatch_context_alloc(dpp);
    if (ctp == NULL) {
        logf_(SLOG2_ERROR, "dispatch_context_alloc failed: %s", strerror(errno));
        return 1;
    }
    logf_(SLOG2_INFO, "up: %s and name %s attached, level 4 (boot), tick %d ms", LG_DEV_CONTROLLER,
          LG_NAME_CONTROLLER, LG_TICK_MS);
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
