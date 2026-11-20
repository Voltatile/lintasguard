/*
 * lg-gate: simulated barrier, lights and bell as a QNX resource manager (README 4.2, 8.5).
 *
 *  - /dev/lg/gate           read: state as text. No write handler, so nothing written there can
 *                           move the gate (CLAUDE.md rule 4). devctl: fault injection and stats.
 *  - name "lg/gate"         message passing: LG_MSG_GATE_CMD in, LG_MSG_GATE_STATE out, and
 *                           LG_PULSE_HEARTBEAT as a keepalive.
 *  - watchdog               lowers the gate when the controller stops sending (lg_gate_model.c).
 *
 * One thread, one dispatch loop: the resource manager, the name, the messages and the pulses all
 * run in the same loop, so the model needs no locking. The gate starts DOWN.
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
#include "lg_devctl.h"
#include "lg_gate_model.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_target_util.h"

static lg_gate_model_t g_model;
static uint32_t g_ticks;
static uint32_t g_seq;
static slog2_buffer_t g_log;
static int g_verbose;
static int g_loop_started; /* after this, logging goes to slog2 only */

static iofunc_attr_t g_attr;
static resmgr_connect_funcs_t g_connect_funcs;
static resmgr_io_funcs_t g_io_funcs;

/* The text served by io_read, regenerated on every read: read the file with one read, like cat. */
static char g_snap[256];
static size_t g_snap_len;

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
    /* stderr only while starting up: a write to a stuck terminal or pipe would block the one
     * thread that runs the watchdog, and the loop must never block on logging. */
    if ((g_verbose || severity <= SLOG2_ERROR) && !g_loop_started) {
        (void)fprintf(stderr, "lg-gate: %s\n", line);
    }
}

/*
 * CLOCK_MONOTONIC in ms. A failed read must not silently freeze the watchdog (the model ignores
 * a clock that stands still), so it fails to the safe state: abort, and a restart boots DOWN.
 */
static uint32_t now_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        logf_(SLOG2_ERROR, "clock_gettime failed: %s, aborting so that the gate restarts DOWN",
              strerror(errno));
        abort();
    }
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void fill_hdr(lg_hdr_t *h, uint16_t type) {
    h->type = type;
    h->version = LG_PROTO_VERSION;
    h->seq = ++g_seq;
    h->t_ns = lg_now_ns();
}

/* ---- Resource manager: /dev/lg/gate ------------------------------------------------------ */

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
    size_t off;
    size_t nbytes;
    int rc = iofunc_read_verify(ctp, msg, ocb, NULL);

    if (rc != EOK) {
        return rc;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }

    g_snap_len = lg_gate_model_text(&g_model, g_snap, sizeof g_snap);
    off = (size_t)ocb->offset;
    if (off >= g_snap_len) {
        nbytes = 0;
    } else {
        nbytes = g_snap_len - off;
        if (nbytes > msg->i.nbytes) {
            nbytes = msg->i.nbytes;
        }
    }

    _IO_SET_READ_NBYTES(ctp, (int)nbytes);
    if (nbytes == 0) {
        return _RESMGR_NPARTS(0);
    }
    SETIOV(ctp->iov, g_snap + off, nbytes);
    ocb->offset += (off_t)nbytes;
    return _RESMGR_NPARTS(1);
}

/*
 * Explicit refusal. The default write handler accepts and discards the data, which would look
 * like success to the writer. Nothing written here ever reaches the gate model (CLAUDE.md rule 4).
 */
static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
    (void)msg;
    (void)ocb;
    logf_(SLOG2_WARNING, "write to %s from pid %d refused: the gate takes commands through "
                         "message passing only",
          LG_DEV_GATE, (int)ctp->info.pid);
    return EPERM;
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb) {
    int status = iofunc_devctl_default(ctp, msg, ocb);
    int nbytes = 0;

    if (status != _RESMGR_DEFAULT) {
        return status;
    }

    switch (msg->i.dcmd) {
    case DCMD_LG_GATE_FAULT: {
        lg_gate_fault_req_t req;

        /* Fault injection needs a descriptor opened for writing (a read-only fd is not enough). */
        if ((ocb->ioflag & _IO_FLAG_WR) == 0) {
            logf_(SLOG2_WARNING, "devctl fault from pid %d refused: descriptor not open for writing",
                  (int)ctp->info.pid);
            return EPERM;
        }
        /* nbytes is filled in by the client: also check what actually arrived. */
        if (msg->i.nbytes < sizeof req || ctp->info.msglen < (int)(sizeof msg->i + sizeof req)) {
            return EINVAL;
        }
        memcpy(&req, _DEVCTL_DATA(msg->i), sizeof req);
        if (req.mode == LG_GFAULT_CRASH) {
            logf_(SLOG2_ERROR, "fault CRASH injected through devctl, aborting");
            abort();
        }
        /* Range check first: casting a large value to uint8_t would wrap into a valid mode. */
        if (req.mode > 0xFFu ||
            lg_gate_model_set_fault(&g_model, (uint8_t)req.mode, req.factor_pct, now_ms()) !=
                0) {
            logf_(SLOG2_WARNING, "devctl fault rejected (mode %u factor %u)", (unsigned)req.mode,
                  (unsigned)req.factor_pct);
            return EINVAL;
        }
        logf_(SLOG2_WARNING, "fault %s factor %u%% set through devctl",
              lg_gate_fault_name((uint8_t)req.mode), (unsigned)req.factor_pct);
        break;
    }
    case DCMD_LG_GATE_STATS: {
        lg_gate_stats_t st;

        memset(&st, 0, sizeof st);
        st.state = g_model.state;
        st.target = g_model.target;
        st.position_mdeg = g_model.position_mdeg;
        st.current_ma = g_model.current_ma;
        st.last_travel_ms = g_model.last_travel_ms;
        st.fault = g_model.fault;
        st.fault_pct = g_model.fault_pct;
        st.watchdog_trips = g_model.watchdog_trips;
        st.rejected = g_model.rejected;
        st.ticks = g_ticks;
        {
            struct sched_param sp;
            int policy = 0;

            memset(&sp, 0, sizeof sp);
            if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0) {
                st.sched_policy = (uint32_t)policy;
                st.sched_priority = (uint32_t)sp.sched_priority;
            }
        }
        memcpy(_DEVCTL_DATA(msg->o), &st, sizeof st);
        nbytes = (int)sizeof st;
        break;
    }
    default:
        return ENOSYS;
    }

    memset(&msg->o, 0, sizeof msg->o);
    msg->o.ret_val = 0;
    msg->o.nbytes = (uint32_t)nbytes;
    return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o + (size_t)nbytes);
}

/* ---- Messages and pulses ---------------------------------------------------------------- */

/*
 * LG_MSG_GATE_CMD. Today the test client plays the controller. Step 13 adds the credential
 * check here: only the controller's user and process may command the gate.
 */
static int on_gate_cmd(message_context_t *ctp, int code, unsigned flags, void *handle) {
    const lg_gate_cmd_msg_t *cmd = (const lg_gate_cmd_msg_t *)ctp->msg;
    lg_gate_state_msg_t rep;
    uint8_t prev_state = g_model.state;

    (void)flags;
    (void)handle;

    if ((size_t)ctp->info.msglen < sizeof *cmd || cmd->hdr.type != (uint16_t)code ||
        cmd->hdr.version != LG_PROTO_VERSION) {
        logf_(SLOG2_WARNING, "malformed GATE_CMD from pid %d (len %d)", (int)ctp->info.pid,
              (int)ctp->info.msglen);
        (void)MsgError(ctp->rcvid, EINVAL);
        return 0;
    }
    if (lg_gate_model_command(&g_model, cmd->cmd, now_ms()) != 0) {
        logf_(SLOG2_WARNING, "unknown GATE_CMD %u from pid %d", (unsigned)cmd->cmd,
              (int)ctp->info.pid);
        (void)MsgError(ctp->rcvid, EINVAL);
        return 0;
    }
    /*
     * Only a command that changes the state is logged: the controller repeats LOWER or RAISE every
     * tick (the watchdog keepalive), and a line per command would overwrite the ring buffer of
     * slog2 within seconds, the watchdog lines included.
     */
    if (g_model.state != prev_state) {
        logf_(SLOG2_INFO, "GATE_CMD %u from pid %d: state %s -> %s", (unsigned)cmd->cmd,
              (int)ctp->info.pid, lg_gate_state_name(prev_state), lg_gate_state_name(g_model.state));
    }

    memset(&rep, 0, sizeof rep);
    fill_hdr(&rep.hdr, LG_MSG_GATE_STATE);
    lg_gate_model_fill_state(&g_model, &rep);
    (void)MsgReply(ctp->rcvid, EOK, &rep, sizeof rep);
    return 0;
}

static int on_heartbeat(message_context_t *ctp, int code, unsigned flags, void *handle) {
    (void)ctp;
    (void)code;
    (void)flags;
    (void)handle;
    lg_gate_model_feed(&g_model, now_ms());
    return 0;
}

static int on_tick(message_context_t *ctp, int code, unsigned flags, void *handle) {
    uint8_t prev_state = g_model.state;
    uint32_t prev_trips = g_model.watchdog_trips;
    uint32_t now = now_ms();

    (void)ctp;
    (void)code;
    (void)flags;
    (void)handle;

    g_ticks++;
    lg_gate_model_step(&g_model, now);
    if (g_model.watchdog_trips != prev_trips) {
        logf_(SLOG2_ERROR, "watchdog: no controller command or heartbeat for %u ms (limit %u ms), "
                           "lowering the gate (trip %u)",
              (unsigned)(now - g_model.last_feed_ms), (unsigned)g_model.cfg.watchdog_ms,
              (unsigned)g_model.watchdog_trips);
    }
    if (g_model.state != prev_state) {
        logf_(SLOG2_INFO, "gate state %s -> %s (position %d deg)", lg_gate_state_name(prev_state),
              lg_gate_state_name(g_model.state), (int)(g_model.position_mdeg / 1000));
    }
    return 0;
}

/* ---- Setup ------------------------------------------------------------------------------ */

static void setup_logging(void) {
    slog2_buffer_set_config_t cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.num_buffers = 1;
    cfg.buffer_set_name = "lg-gate";
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "main";
    cfg.buffer_config[0].num_pages = 4;
    if (slog2_register(&cfg, &g_log, 0) != 0) {
        g_log = NULL;
        (void)fprintf(stderr, "lg-gate: slog2_register failed: %s\n", strerror(errno));
    }
}

static void setup_priority(void) {
    struct sched_param sp;
    int rc;

    memset(&sp, 0, sizeof sp);
    sp.sched_priority = LG_PRIO_GATE;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) {
        logf_(SLOG2_WARNING, "could not set SCHED_FIFO %d: %s (continuing at default priority)",
              LG_PRIO_GATE, strerror(rc));
    } else {
        logf_(SLOG2_INFO, "running SCHED_FIFO priority %d", LG_PRIO_GATE);
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
    SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, LG_PULSE_TICK, 0);
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
    lg_gate_cfg_t cfg = lg_gate_cfg_default();
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
            (void)fprintf(stderr, "usage: lg-gate [-v]\n");
            return 2;
        }
    }

    setup_logging();
    setup_priority();

    /* The gate starts DOWN with lights and bell on, before anything else happens. */
    lg_gate_model_init(&g_model, &cfg, now_ms());

    /*
     * _NTO_CHF_FIXED_PRIORITY: on a default channel the server thread takes the priority of
     * whoever sent the message, even a lower one (measured on this VM: a priority-10 client
     * pulled a FIFO-48 server down to 10). The gate must keep its own priority whoever asks.
     * UNBLOCK and DISCONNECT are needed by the resource manager framework.
     */
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
    g_io_funcs.devctl = io_devctl;
    /* io_write refuses every write, so /dev/lg/gate cannot be used to move the gate. */
    iofunc_attr_init(&g_attr, S_IFCHR | 0644, NULL, NULL);

    if (resmgr_attach(dpp, &rattr, LG_DEV_GATE, _FTYPE_ANY, 0, &g_connect_funcs, &g_io_funcs,
                      &g_attr) == -1) {
        logf_(SLOG2_ERROR, "resmgr_attach(%s) failed: %s", LG_DEV_GATE, strerror(errno));
        return 1;
    }
    name = name_attach(dpp, LG_NAME_GATE, 0);
    if (name == NULL) {
        logf_(SLOG2_ERROR, "name_attach(%s) failed: %s", LG_NAME_GATE, strerror(errno));
        return 1;
    }
    if (message_attach(dpp, NULL, LG_MSG_GATE_CMD, LG_MSG_GATE_CMD, on_gate_cmd, NULL) == -1) {
        logf_(SLOG2_ERROR, "message_attach failed: %s", strerror(errno));
        return 1;
    }
    if (pulse_attach(dpp, 0, LG_PULSE_HEARTBEAT, on_heartbeat, NULL) == -1 ||
        pulse_attach(dpp, 0, LG_PULSE_TICK, on_tick, NULL) == -1) {
        logf_(SLOG2_ERROR, "pulse_attach failed: %s", strerror(errno));
        return 1;
    }
    if (start_tick_timer(dpp) != 0) {
        return 1;
    }

    ctp = dispatch_context_alloc(dpp);
    if (ctp == NULL) {
        logf_(SLOG2_ERROR, "dispatch_context_alloc failed: %s", strerror(errno));
        return 1;
    }
    logf_(SLOG2_INFO, "up: %s and name %s attached, gate DOWN, watchdog %u ms, tick %d ms",
          LG_DEV_GATE, LG_NAME_GATE, (unsigned)cfg.watchdog_ms, LG_TICK_MS);
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
