/*
 * lg-sensor: one simulated track sensor (README 4.2, 7.3, 7.4). One binary, six instances:
 *
 *   lg-sensor -d west|east -i a|b|island [-s seed] [-v]
 *
 *  - /dev/lg/sensor/<dir>/<id>   read: the latest sample as text. Writes are refused. devctl:
 *                                fault injection (needs a descriptor opened for writing) and stats.
 *  - name lg/sensor/<dir>/<id>
 *  - every period (LG_SENSOR_PERIOD_MS): ask the plant what this sensor sees, run the sensor model
 *    (noise, faults), send LG_MSG_SENSOR_SAMPLE to lg/controller.
 *
 * Threads (all share the state under g_lock; the lock is never held across a blocking call):
 *  - main: only serves the device (dispatch loop on a fixed-priority channel, so a low-priority
 *    reader cannot pull the sensor's priority down, see docs/ARCHITECTURE.md). It never calls
 *    anything that waits for another process, so `cat` and `lgctl` always get an answer.
 *  - worker (SCHED_FIFO 45): the periodic work. A timer sends it a pulse on a channel that is
 *    private to this process (_NTO_CHF_PRIVATE: another process cannot attach to it, measured on
 *    the VM) and fixed-priority. The pulse carries the priority 45 explicitly: with
 *    SIGEV_PULSE_PRIO_INHERIT the receiver would run at the priority the process was created with
 *    (10 from a shell, measured). The worker does the MsgSend calls to the plant and to the
 *    controller. A MsgSend is bounded by TimerTimeout in its send phase (a server that is alive
 *    but not receiving costs one timeout per tick). If a server has RECEIVED the message and never
 *    replies, and its channel has _NTO_CHF_UNBLOCK (every dispatch_create channel has), the kernel
 *    keeps the sender blocked until the reply: then only the worker waits and the device still
 *    answers. Step 8 restarts such a process.
 *  - connectors (one per peer, low priority): name_open and name_close, which wait for the
 *    server, run only there. A timeout keeps the connection; any other error is read as "the
 *    server is gone": the connection is dropped and a connector reconnects. A peer that wants to
 *    refuse a sample without a reconnect must answer with lg_ack_t.status, not with MsgError.
 *
 * Failures are counted. The sequence number counts ticks, not sent samples, so a dropped sample
 * shows up as a gap at the receiver. Periods in which the worker was late by two periods or more
 * consume sequence numbers as well and are counted as overruns; the pulses that queued up in the
 * meantime are discarded, not replayed, so a controller never receives a burst.
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
#include "lg_heartbeat.h"
#include "lg_metrics.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_sensor_model.h"
#include "lg_target_util.h"
#include "lg_time.h"

static const char *const dir_names[LG_DIR_COUNT] = {LG_DIR_NAME_WEST, LG_DIR_NAME_EAST};
static const char *const id_names[LG_SENSOR_ID_COUNT] = {LG_SENSOR_NAME_A, LG_SENSOR_NAME_B,
                                                         LG_SENSOR_NAME_ISLAND};

static uint8_t g_dir;
static uint8_t g_id;
static slog2_buffer_t g_log;
static int g_verbose;
static int g_loop_started; /* after this, logging goes to slog2 only */

/* State shared by the threads, protected by g_lock. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static lg_sensor_model_t g_model;
static lg_sensor_out_t g_last;
static int g_have_sample;
static uint16_t g_last_axles;
static uint32_t g_seq;      /* tick counter, also the sequence number of the sample of this tick */
static uint32_t g_last_seq; /* sequence number of the latest sample that was produced */
static int g_worker_policy; /* scheduling of the worker thread at its latest tick */
static int g_worker_prio;
static struct {
    uint32_t ticks;
    uint32_t samples_sent;
    uint32_t no_sample;
    uint32_t send_fail;
    uint32_t no_controller;
    uint32_t plant_fail;
    uint32_t overruns;
    uint32_t timeouts;
} g_stats;
static lg_sensor_acc_t g_acc; /* sample statistics for lg-health (Step 11), under g_lock */

/* A connection shared between the worker (uses it) and a connector thread (makes it). */
typedef struct {
    pthread_mutex_t lock;
    const char *name; /* name to open */
    int coid;         /* connection id, -1 when there is none */
    int stale;        /* a dead connection for the connector to close, -1 if none */
    uint32_t connects;
} conn_slot_t;

static conn_slot_t g_plant = {PTHREAD_MUTEX_INITIALIZER, LG_NAME_PLANT, -1, -1, 0};
static conn_slot_t g_ctrl = {PTHREAD_MUTEX_INITIALIZER, LG_NAME_CONTROLLER, -1, -1, 0};

static iofunc_attr_t g_attr;
static resmgr_connect_funcs_t g_connect_funcs;
static resmgr_io_funcs_t g_io_funcs;
static char g_text[200]; /* main thread only */

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
    /* stderr only while starting up: a stuck terminal must not block a loop. */
    if ((g_verbose || severity <= SLOG2_ERROR) && !g_loop_started) {
        (void)fprintf(stderr, "lg-sensor %s/%s: %s\n", dir_names[g_dir], id_names[g_id], line);
    }
}

/* CLOCK_MONOTONIC in ms; a failed read aborts (a restart is the safe state). */
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

static void count(uint32_t *counter) {
    (void)pthread_mutex_lock(&g_lock);
    (*counter)++;
    (void)pthread_mutex_unlock(&g_lock);
}

/* ---- connections and bounded IPC ----------------------------------------------------------- */

static int slot_get(conn_slot_t *slot) {
    int coid;

    (void)pthread_mutex_lock(&slot->lock);
    coid = slot->coid;
    (void)pthread_mutex_unlock(&slot->lock);
    return coid;
}

static uint32_t slot_connects(conn_slot_t *slot) {
    uint32_t n;

    (void)pthread_mutex_lock(&slot->lock);
    n = slot->connects;
    (void)pthread_mutex_unlock(&slot->lock);
    return n;
}

/* The server is gone: forget the connection and hand it to the connector to close. */
static void slot_fail(conn_slot_t *slot, int coid) {
    (void)pthread_mutex_lock(&slot->lock);
    if (slot->coid == coid) {
        slot->coid = -1;
        slot->stale = coid;
    }
    (void)pthread_mutex_unlock(&slot->lock);
}

/* One per slot, low priority: the only place where name_open and name_close are called. */
static void *connector(void *arg) {
    conn_slot_t *slot = (conn_slot_t *)arg;
    struct timespec pause;

    pause.tv_sec = 0;
    pause.tv_nsec = (long)LG_SENSOR_CONNECTOR_PERIOD_MS * 1000000L;
    for (;;) {
        int stale;
        int need;

        (void)pthread_mutex_lock(&slot->lock);
        stale = slot->stale;
        slot->stale = -1;
        need = slot->coid < 0;
        (void)pthread_mutex_unlock(&slot->lock);

        if (stale >= 0) {
            (void)name_close(stale);
        }
        if (need) {
            int c = name_open(slot->name, 0); /* may block while the server is stuck: only here */

            if (c >= 0) {
                (void)pthread_mutex_lock(&slot->lock);
                slot->coid = c;
                slot->connects++;
                (void)pthread_mutex_unlock(&slot->lock);
            }
        }
        (void)nanosleep(&pause, NULL);
    }
    return NULL;
}

/* Starts a thread with an explicit policy and priority (not the priority of the caller). */
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

/* MsgSend that cannot wait for a server that is not receiving for longer than the limit. */
static long bounded_send(int coid, const void *smsg, int sbytes, void *rmsg, int rbytes) {
    uint64_t timeout_ns = (uint64_t)LG_SENSOR_IPC_TIMEOUT_MS * 1000000ull;

    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout_ns,
                     NULL) == -1) {
        return -1;
    }
    return MsgSend(coid, smsg, sbytes, rmsg, rbytes);
}

/* Returns 1 with the truth in *rep, 0 when the plant is missing or did not answer in time. */
static int query_plant(uint32_t seq, lg_truth_reply_t *rep) {
    lg_truth_query_t q;
    int coid = slot_get(&g_plant);

    if (coid < 0) {
        return 0;
    }
    memset(&q, 0, sizeof q);
    fill_hdr(&q.hdr, LG_MSG_TRUTH_QUERY, seq);
    q.dir = g_dir;
    q.id = g_id;
    memset(rep, 0, sizeof *rep);
    if (bounded_send(coid, &q, sizeof q, rep, sizeof *rep) == -1) {
        int e = errno;

        if (e == ETIMEDOUT) {
            count(&g_stats.timeouts); /* alive but not answering: keep the connection */
        } else {
            slot_fail(&g_plant, coid); /* the server is gone: a connector reconnects */
        }
        return 0;
    }
    if (rep->hdr.type != LG_MSG_TRUTH_REPLY || rep->hdr.version != LG_PROTO_VERSION ||
        rep->dir != g_dir || rep->id != g_id) {
        slot_fail(&g_plant, coid); /* it does not speak our protocol: start again */
        return 0;
    }
    return 1;
}

static void send_sample(uint32_t seq, const lg_sensor_out_t *s) {
    lg_sensor_sample_t msg;
    lg_ack_t ack;
    int coid = slot_get(&g_ctrl);

    if (coid < 0) {
        count(&g_stats.no_controller);
        return;
    }
    memset(&msg, 0, sizeof msg);
    fill_hdr(&msg.hdr, LG_MSG_SENSOR_SAMPLE, seq);
    msg.dir = g_dir;
    msg.id = g_id;
    msg.occupied = s->occupied;
    msg.quality = s->quality;
    msg.axle_count = s->axle_count;
    msg.raw_level = s->raw_level;
    memset(&ack, 0, sizeof ack);
    if (bounded_send(coid, &msg, sizeof msg, &ack, sizeof ack) == -1) {
        int e = errno; /* before count(): a successful call may change errno */

        count(&g_stats.send_fail);
        if (e == ETIMEDOUT) {
            count(&g_stats.timeouts);
        } else {
            slot_fail(&g_ctrl, coid);
        }
        return;
    }
    if (ack.status != 0) {
        count(&g_stats.send_fail);
        return;
    }
    count(&g_stats.samples_sent);
}

/* ---- the periodic work (worker thread) ------------------------------------------------------ */

static void tick(uint64_t *prev_tick_ns) {
    uint64_t now_ns = lg_now_ns();
    uint32_t now = now_ms();
    lg_truth_reply_t truth;
    lg_sensor_out_t out;
    uint32_t seq;
    int have_truth;
    int have;
    int hung;

    uint64_t period_ns = (uint64_t)LG_SENSOR_PERIOD_MS * 1000000ull;
    uint32_t missed = 0;
    int late = 0;

    if (*prev_tick_ns != 0) {
        uint64_t gap_ns = now_ns - *prev_tick_ns;

        late = lg_tick_late(*prev_tick_ns, now_ns);
        if (gap_ns >= 2 * period_ns) {
            missed = (uint32_t)(gap_ns / period_ns) - 1; /* whole periods without a tick */
        }
    }
    (void)pthread_mutex_lock(&g_lock);
    g_stats.ticks++;
    g_seq += 1 + missed; /* skipped periods consume sequence numbers: the stall shows as a gap */
    seq = g_seq;
    g_stats.overruns += missed > 0 ? missed : (uint32_t)late;
    (void)pthread_mutex_unlock(&g_lock);
    *prev_tick_ns = now_ns;

    have_truth = query_plant(seq, &truth); /* may wait: the lock is not held */

    (void)pthread_mutex_lock(&g_lock);
    if (have_truth) {
        g_last_axles = truth.axle_count;
        have = lg_sensor_model_step(&g_model, now, truth.occupied, truth.axle_count, &out);
    } else {
        /* No truth: a sensor that cannot see anything reports quality 0, unless it is hung. */
        g_stats.plant_fail++;
        if (g_model.fault == LG_SFAULT_HANG) {
            have = 0;
        } else {
            memset(&out, 0, sizeof out);
            out.axle_count = g_last_axles;
            have = 1;
        }
    }
    if (have) {
        g_last = out;
        g_last_seq = seq;
        g_have_sample = 1;
    } else {
        g_stats.no_sample++;
    }
    lg_sensor_acc_step(&g_acc, now, have, &out);
    hung = g_model.fault == LG_SFAULT_HANG;
    (void)pthread_mutex_unlock(&g_lock);

    if (have) {
        send_sample(seq, &out); /* may wait: the lock is not held */
    }
    /*
     * The heartbeat proves that the worker got through a whole tick (a worker stuck in a send
     * stops beating). The injected HANG fault stops it on purpose (README 7.4): the supervisor
     * then restarts the process, which also clears the fault.
     */
    if (!hung) {
        lg_hb_beat();
    }
}

/* Throw away the pulses that queued up while the worker was busy or blocked (no burst replay). */
static void drain_pulses(int chid) {
    int i;

    for (i = 0; i < 10000; i++) {
        struct _pulse pulse;
        uint64_t zero_ns = 1; /* do not wait: only what is already queued */
        int rcvid;

        if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &zero_ns, NULL) == -1) {
            return;
        }
        rcvid = MsgReceive(chid, &pulse, sizeof pulse, NULL);
        if (rcvid == -1) {
            return; /* ETIMEDOUT: nothing is queued */
        }
        if (rcvid > 0) {
            (void)MsgError(rcvid, ENOSYS);
        }
    }
}

/*
 * The worker waits for a periodic timer pulse on a channel that only this process can attach to
 * (_NTO_CHF_PRIVATE) and that keeps the priority of the thread (_NTO_CHF_FIXED_PRIORITY).
 */
static void *worker(void *arg) {
    struct sigevent ev;
    struct itimerspec its;
    timer_t timer;
    uint64_t prev_tick_ns = 0;
    int chid;
    int coid;

    (void)arg;
    chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_PRIVATE);
    if (chid == -1) {
        logf_(SLOG2_ERROR, "worker: ChannelCreate failed: %s, aborting", strerror(errno));
        abort();
    }
    coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        logf_(SLOG2_ERROR, "worker: ConnectAttach failed: %s, aborting", strerror(errno));
        abort();
    }
    /* Explicit priority, see the comment at the top: INHERIT would give the process priority. */
    SIGEV_PULSE_INIT(&ev, coid, LG_PRIO_SENSOR, LG_PULSE_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        logf_(SLOG2_ERROR, "worker: timer_create failed: %s, aborting", strerror(errno));
        abort();
    }
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = (long)LG_SENSOR_PERIOD_MS * 1000000L;
    its.it_interval.tv_nsec = (long)LG_SENSOR_PERIOD_MS * 1000000L;
    if (timer_settime(timer, 0, &its, NULL) == -1) {
        logf_(SLOG2_ERROR, "worker: timer_settime failed: %s, aborting", strerror(errno));
        abort();
    }
    for (;;) {
        struct _pulse pulse;
        struct sched_param sp;
        int policy = 0;
        int rcvid = MsgReceive(chid, &pulse, sizeof pulse, NULL);

        if (rcvid == -1) {
            if (errno == EINTR) {
                continue;
            }
            logf_(SLOG2_ERROR, "worker: MsgReceive failed: %s, aborting", strerror(errno));
            abort();
        }
        if (rcvid > 0) {
            (void)MsgError(rcvid, ENOSYS); /* a message is not a tick, and nobody may wait for us */
            continue;
        }
        if (pulse.code != LG_PULSE_TICK) {
            continue;
        }
        memset(&sp, 0, sizeof sp);
        if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0) {
            (void)pthread_mutex_lock(&g_lock);
            g_worker_policy = policy;
            g_worker_prio = sp.sched_priority;
            (void)pthread_mutex_unlock(&g_lock);
        }
        tick(&prev_tick_ns);
        drain_pulses(chid);
    }
    return NULL;
}

/* ---- /dev/lg/sensor/<dir>/<id> (main thread) ------------------------------------------------ */

static size_t sensor_text(char *buf, size_t len) {
    (void)pthread_mutex_lock(&g_lock);
    (void)snprintf(buf, len, "occupied=%u quality=%u axles=%u raw=%u fault=%s seq=%u tick=%u\n",
                   g_have_sample ? (unsigned)g_last.occupied : 0u,
                   g_have_sample ? (unsigned)g_last.quality : 0u,
                   g_have_sample ? (unsigned)g_last.axle_count : 0u,
                   g_have_sample ? (unsigned)g_last.raw_level : 0u, lg_sensor_fault_name(g_model.fault),
                   (unsigned)g_last_seq, (unsigned)g_seq);
    (void)pthread_mutex_unlock(&g_lock);
    return strlen(buf);
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
    len = sensor_text(g_text, sizeof g_text); /* regenerated on every read: read it in one go */
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
    logf_(SLOG2_WARNING, "write to the sensor device from pid %d refused", (int)ctp->info.pid);
    return EPERM;
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb) {
    int status = iofunc_devctl_default(ctp, msg, ocb);
    int nbytes = 0;

    if (status != _RESMGR_DEFAULT) {
        return status;
    }

    switch (msg->i.dcmd) {
    case DCMD_LG_SENSOR_FAULT: {
        lg_sensor_fault_req_t req;
        int rc;

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
        if (req.mode == LG_SFAULT_CRASH) {
            logf_(SLOG2_ERROR, "fault CRASH injected through devctl, aborting");
            abort();
        }
        /* Range check first: casting a large value to uint8_t would wrap into a valid mode. */
        if (req.mode > 0xFFu) {
            rc = -1;
        } else {
            (void)pthread_mutex_lock(&g_lock);
            rc = lg_sensor_model_set_fault(&g_model, (uint8_t)req.mode, req.param, now_ms());
            (void)pthread_mutex_unlock(&g_lock);
        }
        if (rc != 0) {
            logf_(SLOG2_WARNING, "devctl fault rejected (mode %u param %u)", (unsigned)req.mode,
                  (unsigned)req.param);
            return EINVAL;
        }
        logf_(SLOG2_WARNING, "fault %s param %u set through devctl",
              lg_sensor_fault_name((uint8_t)req.mode), (unsigned)req.param);
        break;
    }
    case DCMD_LG_SENSOR_STATS: {
        lg_sensor_stats_t st;
        struct sched_param sp;
        int policy = 0;

        memset(&st, 0, sizeof st);
        (void)pthread_mutex_lock(&g_lock);
        st.ticks = g_stats.ticks;
        st.samples_sent = g_stats.samples_sent;
        st.no_sample = g_stats.no_sample;
        st.send_fail = g_stats.send_fail;
        st.no_controller = g_stats.no_controller;
        st.plant_fail = g_stats.plant_fail;
        st.overruns = g_stats.overruns;
        st.timeouts = g_stats.timeouts;
        st.fault = g_model.fault;
        st.fault_param = g_model.param;
        st.seq = g_seq;
        st.worker_policy = (uint32_t)g_worker_policy;
        st.worker_priority = (uint32_t)g_worker_prio;
        st.dev_sum = g_acc.dev_sum;
        st.dev_count = g_acc.dev_count;
        st.acc_ticks = g_acc.ticks;
        st.acc_no_sample = g_acc.no_sample;
        st.blind = g_acc.blind;
        st.rise_count = g_acc.rise_count;
        st.last_rise_ms = g_acc.last_rise_ms;
        st.last_fall_ms = g_acc.last_fall_ms;
        (void)pthread_mutex_unlock(&g_lock);
        st.connects = slot_connects(&g_plant) + slot_connects(&g_ctrl);
        memset(&sp, 0, sizeof sp);
        if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0) {
            st.sched_policy = (uint32_t)policy;
            st.sched_priority = (uint32_t)sp.sched_priority;
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

/* ---- setup --------------------------------------------------------------------------------- */

static void setup_logging(void) {
    slog2_buffer_set_config_t cfg;
    char set_name[40];

    (void)snprintf(set_name, sizeof set_name, "lg-sensor-%s-%s", dir_names[g_dir], id_names[g_id]);
    memset(&cfg, 0, sizeof cfg);
    cfg.num_buffers = 1;
    cfg.buffer_set_name = set_name;
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "main";
    cfg.buffer_config[0].num_pages = 2;
    if (slog2_register(&cfg, &g_log, 0) != 0) {
        g_log = NULL;
        (void)fprintf(stderr, "lg-sensor: slog2_register failed: %s\n", strerror(errno));
    }
}

static void setup_priority(void) {
    struct sched_param sp;
    int rc;

    memset(&sp, 0, sizeof sp);
    sp.sched_priority = LG_PRIO_SENSOR;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) {
        logf_(SLOG2_WARNING, "could not set SCHED_FIFO %d: %s (continuing at default priority)",
              LG_PRIO_SENSOR, strerror(rc));
    } else {
        logf_(SLOG2_INFO, "running SCHED_FIFO priority %d", LG_PRIO_SENSOR);
    }
}

static int index_of(const char *const *names, int count_, const char *s) {
    int i;

    for (i = 0; i < count_; i++) {
        if (strcmp(names[i], s) == 0) {
            return i;
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    int dir = -1;
    int id = -1;
    uint32_t seed = 0;
    int seed_given = 0;
    char dev_path[64];
    char name_path[64];
    dispatch_t *dpp;
    dispatch_context_t *ctp;
    resmgr_attr_t rattr;
    name_attach_t *name;
    int chid;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            dir = index_of(dir_names, LG_DIR_COUNT, argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            id = index_of(id_names, LG_SENSOR_ID_COUNT, argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 0);
            seed_given = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else {
            dir = -1;
            break;
        }
    }
    if (dir < 0 || id < 0) {
        (void)fprintf(stderr, "usage: lg-sensor -d west|east -i a|b|island [-s seed] [-v]\n");
        return 2;
    }
    g_dir = (uint8_t)dir;
    g_id = (uint8_t)id;
    if (!seed_given) {
        seed = (uint32_t)(g_dir * LG_SENSOR_ID_COUNT + g_id + 1) * 2654435761u;
    }
    (void)snprintf(dev_path, sizeof dev_path, LG_DEV_SENSOR_FMT, dir_names[g_dir], id_names[g_id]);
    (void)snprintf(name_path, sizeof name_path, LG_NAME_SENSOR_FMT, dir_names[g_dir], id_names[g_id]);

    setup_logging();
    setup_priority();
    lg_sensor_model_init(&g_model, seed, now_ms());

    /* Fixed priority: whoever reads the device must not lower this thread's priority. */
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
    iofunc_attr_init(&g_attr, S_IFCHR | 0644, NULL, NULL);

    if (resmgr_attach(dpp, &rattr, dev_path, _FTYPE_ANY, 0, &g_connect_funcs, &g_io_funcs, &g_attr) ==
        -1) {
        logf_(SLOG2_ERROR, "resmgr_attach(%s) failed: %s", dev_path, strerror(errno));
        return 1;
    }
    name = name_attach(dpp, name_path, 0);
    if (name == NULL) {
        logf_(SLOG2_ERROR, "name_attach(%s) failed: %s", name_path, strerror(errno));
        return 1;
    }
    if (start_thread(connector, &g_plant, SCHED_RR, LG_PRIO_CONNECTOR, "plant connector") != 0 ||
        start_thread(connector, &g_ctrl, SCHED_RR, LG_PRIO_CONNECTOR, "controller connector") != 0 ||
        start_thread(worker, NULL, SCHED_FIFO, LG_PRIO_SENSOR, "worker") != 0) {
        return 1;
    }
    ctp = dispatch_context_alloc(dpp);
    if (ctp == NULL) {
        logf_(SLOG2_ERROR, "dispatch_context_alloc failed: %s", strerror(errno));
        return 1;
    }
    logf_(SLOG2_INFO, "up: %s and name %s attached, seed %u, period %d ms", dev_path, name_path,
          (unsigned)seed, LG_SENSOR_PERIOD_MS);
    if (lg_hb_start((uint8_t)(LG_PROC_SENSOR_BASE + g_dir * LG_SENSOR_ID_COUNT + g_id)) != 0) {
        logf_(SLOG2_WARNING, "heartbeat thread not started: no supervision");
    }
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
