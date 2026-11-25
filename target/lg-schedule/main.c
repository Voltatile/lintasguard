/*
 * lg-schedule (README 4.2, 8.4, Step 12): the timetable merged with live station departures. Every
 * LG_SCHED_PUSH_MS it sends one LG_MSG_SCHED_REPLY per direction to lg/controller (the controller turns the
 * entries into level 3 windows and uses the information flag for its timeout). The rules are in
 * core/timetable.c; this process only receives, keeps the time and passes messages.
 *
 *   lg-schedule -t <timetable.json> [--epoch-ms <ms>] [-v]
 *
 * --epoch-ms: the CLOCK_MONOTONIC millisecond at which the scenario started (lg-supervisor gives the same
 * value to lg-plant), so that timetable times (seconds since the scenario start) and the plant run on one
 * clock. Without it the scenario starts now. Only the clock is shared: no ground truth is read.
 *
 * It has NO gate command path (CLAUDE.md rule 4): no gate name, path or message type is used (checked by
 * tests/static/check_schedule_no_gate.sh). What it sends can only open windows (gate down) or keep the
 * information flag; a station message can only move a window earlier or add one.
 *
 * Threads:
 *  - main (SCHED_FIFO LG_PRIO_SCHEDULE): name_attach(LG_NAME_SCHEDULE) channel, MsgReceive loop. A timer
 *    (timer_create, SIGEV_PULSE LG_PULSE_TICK on its own channel, every LG_HEARTBEAT_PERIOD_MS) beats the
 *    supervisor heartbeat on every tick and pushes the replies every LG_SCHED_PUSH_MS; messages are
 *    LG_MSG_STATION_DEPARTURE (from lg-bridge, answered with lg_ack_t) and LG_MSG_SCHED_QUERY (lgctl,
 *    answered with lg_sched_reply_t).
 *  - sender: MsgSend of the replies to lg/controller; a stopped controller blocks only this thread.
 *  - device: /dev/lg/schedule, read = the timetable state (lg_tt_text) plus process counters; devctl
 *    DCMD_LG_SCHED_OUTAGE injects a loss of information (descriptor opened for writing). Writes are refused.
 *
 * Not authenticated yet: any local process can send a station departure or an outage (Step 13). Timings
 * come from a VirtualBox VM: functional evidence only.
 */

#include <errno.h>
#include <fcntl.h>
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
#include <devctl.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>
#include <sys/slog2.h>
#include <time.h>
#include <unistd.h>

#include "lg_config.h"
#include "lg_devctl.h"
#include "lg_heartbeat.h"
#include "lg_json_mini.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_scenario.h"
#include "lg_target_util.h"
#include "lg_timetable.h"

static slog2_buffer_t g_log;
static int g_verbose;
static int g_loop_started;

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
    if ((g_verbose || severity <= SLOG2_ERROR) && !g_loop_started) {
        (void)fprintf(stderr, "lg-schedule: %s\n", line);
    }
}

/* ---- state (main thread writes, device thread reads, under g_lock) ---------------------------- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static lg_tt_t g_tt;
static uint32_t g_epoch_ms;
static const char *g_tt_path = "";
static struct {
    uint32_t ticks;
    uint32_t station_msgs;
    uint32_t queries;
    uint32_t bad_msgs; /* wrong size, version or type */
    uint32_t outages;  /* outage requests accepted */
} g_cnt;

static uint32_t world_now(void) {
    return lg_now_ms() - g_epoch_ms; /* wraps like lg-plant's scenario clock */
}

/* ---- sender ---------------------------------------------------------------------------------- */

static pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_out_cv = PTHREAD_COND_INITIALIZER;
static struct {
    lg_sched_reply_t r[LG_DIR_COUNT];
    uint32_t pending; /* bit per direction */
    uint32_t sent;
    uint32_t send_fail;
    uint32_t refused;
    uint32_t connects;
} g_out;

static void post_reply(const lg_sched_reply_t *r) {
    (void)pthread_mutex_lock(&g_out_lock);
    g_out.r[r->dir] = *r;
    g_out.pending |= 1u << r->dir;
    (void)pthread_cond_signal(&g_out_cv);
    (void)pthread_mutex_unlock(&g_out_lock);
}

static void *sender(void *arg) {
    int coid = -1;
    uint32_t seq = 0;

    (void)arg;
    for (;;) {
        uint32_t pending;
        lg_sched_reply_t r[LG_DIR_COUNT];
        uint8_t d;

        (void)pthread_mutex_lock(&g_out_lock);
        while (g_out.pending == 0) {
            (void)pthread_cond_wait(&g_out_cv, &g_out_lock);
        }
        pending = g_out.pending;
        g_out.pending = 0;
        memcpy(r, g_out.r, sizeof r);
        (void)pthread_mutex_unlock(&g_out_lock);

        if (coid < 0) {
            coid = name_open(LG_NAME_CONTROLLER, 0);
            if (coid < 0) {
                struct timespec pause = {0, 200 * 1000000L};

                (void)pthread_mutex_lock(&g_out_lock);
                g_out.send_fail++; /* the next tick brings fresh replies: nothing is kept */
                (void)pthread_mutex_unlock(&g_out_lock);
                (void)nanosleep(&pause, NULL);
                continue;
            }
            (void)pthread_mutex_lock(&g_out_lock);
            g_out.connects++;
            (void)pthread_mutex_unlock(&g_out_lock);
        }
        for (d = 0; d < LG_DIR_COUNT; d++) {
            lg_ack_t ack;

            if ((pending & (1u << d)) == 0) {
                continue;
            }
            r[d].hdr.type = LG_MSG_SCHED_REPLY;
            r[d].hdr.version = LG_PROTO_VERSION;
            r[d].hdr.seq = ++seq;
            r[d].hdr.t_ns = lg_now_ns();
            memset(&ack, 0, sizeof ack);
            /* MsgSend returns the MsgReply status (0), not a length: check the reply header. */
            if (MsgSend(coid, &r[d], sizeof r[d], &ack, sizeof ack) == -1) {
                (void)pthread_mutex_lock(&g_out_lock);
                g_out.send_fail++;
                (void)pthread_mutex_unlock(&g_out_lock);
                (void)name_close(coid);
                coid = -1;
                break;
            }
            (void)pthread_mutex_lock(&g_out_lock);
            if (ack.hdr.type == LG_MSG_ACK && ack.status == 0) {
                g_out.sent++;
            } else {
                g_out.refused++;
            }
            (void)pthread_mutex_unlock(&g_out_lock);
        }
    }
    return NULL;
}

/* ---- /dev/lg/schedule ------------------------------------------------------------------------ */

static iofunc_attr_t g_attr;
static resmgr_connect_funcs_t g_connect_funcs;
static resmgr_io_funcs_t g_io_funcs;
static char g_text[4096];

static size_t schedule_text(char *buf, size_t len) {
    size_t n;
    int m;

    (void)pthread_mutex_lock(&g_lock);
    n = lg_tt_text(&g_tt, world_now(), buf, len);
    (void)pthread_mutex_lock(&g_out_lock);
    m = snprintf(buf + n, len - n,
                 "process ticks=%u sent=%u send_fail=%u refused=%u connects=%u station_msgs=%u queries=%u "
                 "bad_msgs=%u outages=%u epoch_ms=%u timetable=%s\n",
                 (unsigned)g_cnt.ticks, (unsigned)g_out.sent, (unsigned)g_out.send_fail, (unsigned)g_out.refused,
                 (unsigned)g_out.connects, (unsigned)g_cnt.station_msgs, (unsigned)g_cnt.queries,
                 (unsigned)g_cnt.bad_msgs, (unsigned)g_cnt.outages, (unsigned)g_epoch_ms, g_tt_path);
    (void)pthread_mutex_unlock(&g_out_lock);
    (void)pthread_mutex_unlock(&g_lock);
    if (m > 0) {
        n += (size_t)m >= len - n ? len - n - 1 : (size_t)m;
    }
    return n;
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
    len = schedule_text(g_text, sizeof g_text);
    off = (size_t)ocb->offset;
    nbytes = off >= len ? 0 : len - off;
    if (nbytes > msg->i.nbytes) {
        nbytes = msg->i.nbytes;
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
    logf_(SLOG2_WARNING, "write to %s from pid %d refused", LG_DEV_SCHEDULE, (int)ctp->info.pid);
    return EPERM;
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb) {
    lg_sched_outage_req_t req;
    int rc = iofunc_devctl_default(ctp, msg, ocb);

    if (rc != _RESMGR_DEFAULT) {
        return rc;
    }
    if (msg->i.dcmd != DCMD_LG_SCHED_OUTAGE) {
        return ENOSYS;
    }
    if ((ocb->ioflag & _IO_FLAG_WR) == 0) {
        logf_(SLOG2_WARNING, "devctl outage from pid %d refused: descriptor not open for writing", (int)ctp->info.pid);
        return EPERM;
    }
    if (msg->i.nbytes < sizeof req || ctp->info.msglen < (int)(sizeof msg->i + sizeof req)) {
        return EINVAL;
    }
    memcpy(&req, _DEVCTL_DATA(msg->i), sizeof req);
    if (req.dir_mask > ((1u << LG_DIR_COUNT) - 1u)) {
        return EINVAL;
    }
    (void)pthread_mutex_lock(&g_lock);
    lg_tt_set_outage(&g_tt, (uint8_t)req.dir_mask);
    g_cnt.outages++;
    (void)pthread_mutex_unlock(&g_lock);
    logf_(SLOG2_WARNING, "schedule outage mask 0x%x set by pid %d", (unsigned)req.dir_mask, (int)ctp->info.pid);
    memset(&msg->o, 0, sizeof msg->o);
    return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o);
}

static void *device_thread(void *arg) {
    dispatch_t *dpp = (dispatch_t *)arg;
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);

    if (ctp == NULL) {
        logf_(SLOG2_ERROR, "dispatch_context_alloc failed: %s", strerror(errno));
        return NULL;
    }
    for (;;) {
        dispatch_context_t *next = dispatch_block(ctp);

        if (next == NULL) {
            if (errno == EINTR) {
                continue;
            }
            logf_(SLOG2_ERROR, "dispatch_block failed: %s", strerror(errno));
            return NULL;
        }
        ctp = next;
        (void)dispatch_handler(ctp);
    }
}

/* ---- setup ----------------------------------------------------------------------------------- */

static int start_thread(void *(*fn)(void *), void *arg, int prio, const char *what) {
    pthread_attr_t attr;
    struct sched_param sp;
    pthread_t th;
    int rc;

    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    (void)pthread_attr_init(&attr);
    (void)pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    (void)pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
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

static int setup_device(void) {
    dispatch_t *dpp = dispatch_create();
    resmgr_attr_t rattr;

    if (dpp == NULL) {
        logf_(SLOG2_ERROR, "dispatch_create failed: %s", strerror(errno));
        return -1;
    }
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect_funcs, _RESMGR_IO_NFUNCS, &g_io_funcs);
    g_io_funcs.read = io_read;
    g_io_funcs.write = io_write;
    g_io_funcs.devctl = io_devctl;
    iofunc_attr_init(&g_attr, S_IFCHR | 0644, NULL, NULL);
    if (resmgr_attach(dpp, &rattr, LG_DEV_SCHEDULE, _FTYPE_ANY, 0, &g_connect_funcs, &g_io_funcs, &g_attr) == -1) {
        logf_(SLOG2_ERROR, "resmgr_attach(%s) failed: %s", LG_DEV_SCHEDULE, strerror(errno));
        return -1;
    }
    return start_thread(device_thread, dpp, LG_PRIO_SCHEDULE, "device");
}

static int load_timetable(const char *path) {
    static lg_json_doc_t doc;
    static char text[16384];
    char err[LG_ERR_LEN];
    size_t n;

    if (lg_read_file(path, text, sizeof text, &n) != 0 || lg_json_parse(&doc, text, n) != LG_JSON_OK) {
        logf_(SLOG2_ERROR, "cannot read or parse %s", path);
        return -1;
    }
    if (lg_tt_from_json(&doc, &g_tt, err, sizeof err) != 0) {
        logf_(SLOG2_ERROR, "%s: %s", path, err);
        return -1;
    }
    return 0;
}

/* Periodic tick pulse on our own channel (QNX timer -> pulse, no signal handler). */
static int start_tick_timer(int chid) {
    struct sigevent ev;
    struct itimerspec its;
    timer_t tid;
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);

    if (coid == -1) {
        logf_(SLOG2_ERROR, "ConnectAttach failed: %s", strerror(errno));
        return -1;
    }
    SIGEV_PULSE_INIT(&ev, coid, LG_PRIO_SCHEDULE, LG_PULSE_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &tid) == -1) {
        logf_(SLOG2_ERROR, "timer_create failed: %s", strerror(errno));
        return -1;
    }
    /* The tick is the heartbeat period (the supervisor wants a beat every LG_HEARTBEAT_PERIOD_MS); the
     * replies go out every LG_SCHED_PUSH_MS / LG_HEARTBEAT_PERIOD_MS ticks. */
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = (long)LG_HEARTBEAT_PERIOD_MS * 1000000L;
    its.it_interval.tv_nsec = (long)LG_HEARTBEAT_PERIOD_MS * 1000000L;
    if (timer_settime(tid, 0, &its, NULL) == -1) {
        logf_(SLOG2_ERROR, "timer_settime failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void on_tick(void) {
    static uint32_t n;
    lg_sched_reply_t r[LG_DIR_COUNT];
    uint32_t now;
    uint8_t d;

    lg_hb_beat(); /* the loop that produces the information is the proof of life */
    if (n++ % (LG_SCHED_PUSH_MS / LG_HEARTBEAT_PERIOD_MS) != 0) {
        return;
    }
    (void)pthread_mutex_lock(&g_lock);
    now = world_now();
    for (d = 0; d < LG_DIR_COUNT; d++) {
        memset(&r[d], 0, sizeof r[d]);
        lg_tt_reply(&g_tt, d, now, g_epoch_ms, &r[d]);
    }
    g_cnt.ticks++;
    (void)pthread_mutex_unlock(&g_lock);
    for (d = 0; d < LG_DIR_COUNT; d++) {
        post_reply(&r[d]);
    }
}

static void reply_ack(int rcvid, int32_t status) {
    lg_ack_t ack;

    memset(&ack, 0, sizeof ack);
    ack.hdr.type = LG_MSG_ACK;
    ack.hdr.version = LG_PROTO_VERSION;
    ack.hdr.t_ns = lg_now_ns();
    ack.status = status;
    (void)MsgReply(rcvid, EOK, &ack, sizeof ack);
}

static void on_station(int rcvid, const lg_station_departure_t *m, int len) {
    uint32_t now;
    int rc;

    if (len < (int)sizeof *m || m->hdr.version != LG_PROTO_VERSION || m->dir >= LG_DIR_COUNT ||
        m->eta_ms > LG_STATION_ETA_MAX_MS) {
        (void)pthread_mutex_lock(&g_lock);
        g_cnt.bad_msgs++;
        (void)pthread_mutex_unlock(&g_lock);
        reply_ack(rcvid, EINVAL);
        return;
    }
    (void)pthread_mutex_lock(&g_lock);
    now = world_now();
    rc = lg_tt_station(&g_tt, m->dir, now + m->eta_ms, now);
    g_cnt.station_msgs++;
    (void)pthread_mutex_unlock(&g_lock);
    logf_(SLOG2_INFO, "station departure dir=%s eta_ms=%u at world %u ms: %s",
          m->dir == LG_DIR_WEST ? "west" : "east", (unsigned)m->eta_ms, (unsigned)now,
          rc == 1 ? "window moved earlier" : rc == 2 ? "window added" : rc == 0 ? "ignored (no delay)" : "rejected");
    reply_ack(rcvid, rc < 0 ? EINVAL : 0);
}

static void on_query(int rcvid, const lg_sched_query_t *q, int len) {
    lg_sched_reply_t r;

    if (len < (int)sizeof *q || q->hdr.version != LG_PROTO_VERSION || q->dir >= LG_DIR_COUNT) {
        (void)pthread_mutex_lock(&g_lock);
        g_cnt.bad_msgs++;
        (void)pthread_mutex_unlock(&g_lock);
        (void)MsgError(rcvid, EINVAL);
        return;
    }
    memset(&r, 0, sizeof r);
    (void)pthread_mutex_lock(&g_lock);
    lg_tt_reply(&g_tt, q->dir, world_now(), g_epoch_ms, &r);
    g_cnt.queries++;
    (void)pthread_mutex_unlock(&g_lock);
    r.hdr.type = LG_MSG_SCHED_REPLY;
    r.hdr.version = LG_PROTO_VERSION;
    r.hdr.t_ns = lg_now_ns();
    (void)MsgReply(rcvid, EOK, &r, sizeof r);
}

int main(int argc, char **argv) {
    slog2_buffer_set_config_t scfg;
    struct sched_param sp;
    name_attach_t *att;
    const char *tt_path = NULL;
    int epoch_given = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            tt_path = argv[++i];
        } else if (strcmp(argv[i], "--epoch-ms") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 10);

            if (end == argv[i] || *end != '\0' || v > 0xFFFFFFFFul) {
                (void)fprintf(stderr, "lg-schedule: bad --epoch-ms\n");
                return 2;
            }
            g_epoch_ms = (uint32_t)v;
            epoch_given = 1;
        } else {
            (void)fprintf(stderr, "usage: lg-schedule -t <timetable.json> [--epoch-ms <ms>] [-v]\n");
            return 2;
        }
    }
    if (tt_path == NULL) {
        (void)fprintf(stderr, "usage: lg-schedule -t <timetable.json> [--epoch-ms <ms>] [-v]\n");
        return 2;
    }
    g_tt_path = tt_path;
    memset(&scfg, 0, sizeof scfg);
    scfg.num_buffers = 1;
    scfg.buffer_set_name = "lg-schedule";
    scfg.verbosity_level = SLOG2_INFO;
    scfg.buffer_config[0].buffer_name = "main";
    scfg.buffer_config[0].num_pages = 4;
    if (slog2_register(&scfg, &g_log, 0) != 0) {
        g_log = NULL;
    }
    if (load_timetable(tt_path) != 0) {
        return 1; /* refuse to run on a timetable nobody checked: no information is better than wrong information */
    }
    if (!epoch_given) {
        g_epoch_ms = lg_now_ms();
    }
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = LG_PRIO_SCHEDULE;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        logf_(SLOG2_WARNING, "could not set SCHED_FIFO %d (continuing)", LG_PRIO_SCHEDULE);
    }
    att = name_attach(NULL, LG_NAME_SCHEDULE, 0);
    if (att == NULL) {
        logf_(SLOG2_ERROR, "name_attach(%s) failed: %s", LG_NAME_SCHEDULE, strerror(errno));
        return 1;
    }
    if (setup_device() != 0 || start_thread(sender, NULL, LG_PRIO_SCHEDULE, "sender") != 0 ||
        start_tick_timer(att->chid) != 0) {
        return 1;
    }
    logf_(SLOG2_INFO, "up: %s and %s attached, timetable %s (%u entries, lead %u ms, tolerance %u ms), epoch %u ms "
          "(%s), push every %u ms, no gate command path",
          LG_NAME_SCHEDULE, LG_DEV_SCHEDULE, tt_path, (unsigned)g_tt.count, (unsigned)g_tt.lead_ms,
          (unsigned)g_tt.tol_ms, (unsigned)g_epoch_ms, epoch_given ? "given" : "now", (unsigned)LG_SCHED_PUSH_MS);
    if (lg_hb_start(LG_PROC_SCHEDULE) != 0) {
        logf_(SLOG2_WARNING, "heartbeat thread not started: no supervision");
    }
    g_loop_started = 1;

    for (;;) {
        union {
            uint16_t type;
            struct _pulse pulse;
            lg_hdr_t hdr;
            lg_station_departure_t dep;
            lg_sched_query_t query;
            char raw[128];
        } msg;
        struct _msg_info info;
        int rcvid = MsgReceive(att->chid, &msg, sizeof msg, &info);

        if (rcvid == -1) {
            if (errno != EINTR) {
                logf_(SLOG2_ERROR, "MsgReceive failed: %s", strerror(errno));
            }
            continue;
        }
        if (rcvid == 0) {
            if (msg.pulse.code == LG_PULSE_TICK) {
                on_tick();
            } else if (msg.pulse.code == _PULSE_CODE_DISCONNECT) {
                (void)ConnectDetach(msg.pulse.scoid); /* a client (lg-bridge, lgctl) went away */
            }
            continue;
        }
        if (msg.type == _IO_CONNECT) {
            (void)MsgReply(rcvid, EOK, NULL, 0); /* a client connecting by name */
            continue;
        }
        if (msg.type > _IO_BASE && msg.type <= _IO_MAX) {
            (void)MsgError(rcvid, ENOSYS);
            continue;
        }
        if (msg.type == LG_MSG_STATION_DEPARTURE) {
            on_station(rcvid, &msg.dep, (int)info.msglen);
        } else if (msg.type == LG_MSG_SCHED_QUERY) {
            on_query(rcvid, &msg.query, (int)info.msglen);
        } else {
            (void)pthread_mutex_lock(&g_lock);
            g_cnt.bad_msgs++;
            (void)pthread_mutex_unlock(&g_lock);
            (void)MsgError(rcvid, ENOSYS);
        }
    }
}
