/*
 * lg-health (README 4.2, 8.3, Step 11): health scores of the six sensors, the gate and the backup
 * battery, sent to the controller as LG_MSG_HEALTH_VERDICT with a diagnosis and a trend. The
 * statistics are in core/health.c; this process only reads the devices and passes messages.
 *
 *   lg-health [-t <thresholds.json>] [-v]
 *
 * It has NO gate command path (CLAUDE.md rule 4): the gate is opened read-only for DCMD_LG_GATE_STATS
 * and no gate message type is used (checked by tests/static/check_health_no_gate.sh).
 *
 * Threads (SCHED_FIFO LG_PRIO_HEALTH unless noted):
 *  - main: every LG_HEALTH_TICK_MS (clock_nanosleep on CLOCK_MONOTONIC, absolute, so it does not drift)
 *    it takes the readings the device threads left, feeds them to the health core, and hands every
 *    verdict that changed, or was not sent for LG_HEALTH_REFRESH_MS, to the sender. It beats the
 *    supervisor heartbeat.
 *  - one reader per device (6 sensors + gate): devctl STATS on its own descriptor, at the period the
 *    main thread asks for (LG_HEALTH_POLL_MS, LG_HEALTH_POLL_FAST_MS for a component that is not OK:
 *    level 1 samples faster, README 3). A device whose owner is stopped can block devctl in the reply
 *    phase (measured in Step 6): only that reader waits; the main thread sees no new reading and
 *    reports NO_DATA for that component after LG_HEALTH_NODATA_POLLS periods.
 *  - sender: MsgSend of the verdicts to lg/controller; a stopped controller blocks only this thread.
 *  - device: /dev/lg/health, read = one line per component (score, verdict, diagnosis, trend and the
 *    statistics behind them); devctl DCMD_LG_HEALTH_RESET forgets one component after a repair (needs a
 *    descriptor opened for writing). Writes are refused.
 *
 * Timings come from a VirtualBox VM: functional evidence only.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* iofunc.h must come before dispatch.h: it defines RESMGR_OCB_T as the real OCB type. */
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <devctl.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>
#include <sys/slog2.h>
#include <time.h>
#include <unistd.h>

#include "lg_config.h"
#include "lg_devctl.h"
#include "lg_health.h"
#include "lg_heartbeat.h"
#include "lg_json_mini.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_scenario.h"
#include "lg_target_util.h"
#include "lg_time.h"

#define LG_DEV_HEALTH "/dev/lg/health"
#define LG_HL_DEVICES 7 /* six sensors and the gate (the battery is read with the gate) */
#define LG_HL_GATE_DEV 6

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
        (void)fprintf(stderr, "lg-health: %s\n", line);
    }
}

/* ---- device readers -------------------------------------------------------------------------- */

typedef struct {
    pthread_mutex_t lock;
    char path[48];
    uint32_t period_ms; /* asked by the main thread */
    uint32_t seq;       /* readings made */
    uint32_t read_ms;   /* when the latest reading was made */
    uint32_t fails;     /* failed attempts in a row */
    lg_sensor_stats_t sensor;
    lg_gate_stats_t gate;
} reader_t;

static reader_t g_rd[LG_HL_DEVICES];

static void *reader_thread(void *arg) {
    reader_t *r = (reader_t *)arg;
    int is_gate = r == &g_rd[LG_HL_GATE_DEV];
    int fd = -1;

    for (;;) {
        struct timespec pause;
        uint32_t period;
        int rc = -1;

        if (fd < 0) {
            fd = open(r->path, O_RDONLY); /* read-only: no fault injection, no command */
        }
        if (fd >= 0) {
            if (is_gate) {
                lg_gate_stats_t st;

                memset(&st, 0, sizeof st);
                rc = devctl(fd, DCMD_LG_GATE_STATS, &st, sizeof st, NULL);
                if (rc == EOK) {
                    (void)pthread_mutex_lock(&r->lock);
                    r->gate = st;
                    (void)pthread_mutex_unlock(&r->lock);
                }
            } else {
                lg_sensor_stats_t st;

                memset(&st, 0, sizeof st);
                rc = devctl(fd, DCMD_LG_SENSOR_STATS, &st, sizeof st, NULL);
                if (rc == EOK) {
                    (void)pthread_mutex_lock(&r->lock);
                    r->sensor = st;
                    (void)pthread_mutex_unlock(&r->lock);
                }
            }
            if (rc != EOK) {
                (void)close(fd); /* the device may have a new owner (restarted driver): open again */
                fd = -1;
            }
        }
        (void)pthread_mutex_lock(&r->lock);
        if (rc == EOK) {
            r->seq++;
            r->read_ms = lg_now_ms();
            r->fails = 0;
        } else {
            r->fails++;
        }
        period = r->period_ms;
        (void)pthread_mutex_unlock(&r->lock);
        pause.tv_sec = (time_t)(period / 1000u);
        pause.tv_nsec = (long)(period % 1000u) * 1000000L;
        (void)nanosleep(&pause, NULL);
    }
    return NULL;
}

/* ---- sender ---------------------------------------------------------------------------------- */

static pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_out_cv = PTHREAD_COND_INITIALIZER;
static struct {
    lg_health_out_t v[LG_COMP_COUNT];
    uint32_t pending; /* bit per component */
    uint32_t sent;
    uint32_t send_fail;
    uint32_t refused;
    uint32_t connects;
} g_out;

static void post_verdict(uint8_t comp, const lg_health_out_t *o) {
    (void)pthread_mutex_lock(&g_out_lock);
    g_out.v[comp] = *o;
    g_out.pending |= 1u << comp;
    (void)pthread_cond_signal(&g_out_cv);
    (void)pthread_mutex_unlock(&g_out_lock);
}

static void *sender(void *arg) {
    int coid = -1;
    uint32_t seq = 0;

    (void)arg;
    for (;;) {
        uint32_t pending;
        lg_health_out_t v[LG_COMP_COUNT];
        uint8_t k;

        (void)pthread_mutex_lock(&g_out_lock);
        while (g_out.pending == 0) {
            (void)pthread_cond_wait(&g_out_cv, &g_out_lock);
        }
        pending = g_out.pending;
        g_out.pending = 0;
        memcpy(v, g_out.v, sizeof v);
        (void)pthread_mutex_unlock(&g_out_lock);

        if (coid < 0) {
            coid = name_open(LG_NAME_CONTROLLER, 0);
            if (coid < 0) {
                struct timespec pause = {0, 200 * 1000000L};

                (void)pthread_mutex_lock(&g_out_lock);
                g_out.pending |= pending; /* keep them for the next attempt */
                g_out.send_fail++;
                (void)pthread_mutex_unlock(&g_out_lock);
                (void)nanosleep(&pause, NULL);
                continue;
            }
            (void)pthread_mutex_lock(&g_out_lock);
            g_out.connects++;
            (void)pthread_mutex_unlock(&g_out_lock);
        }
        for (k = 0; k < LG_COMP_COUNT; k++) {
            lg_health_verdict_t msg;
            lg_ack_t ack;

            if ((pending & (1u << k)) == 0) {
                continue;
            }
            memset(&msg, 0, sizeof msg);
            msg.hdr.type = LG_MSG_HEALTH_VERDICT;
            msg.hdr.version = LG_PROTO_VERSION;
            msg.hdr.seq = ++seq;
            msg.hdr.t_ns = lg_now_ns();
            msg.component_id = k;
            msg.score = v[k].score;
            msg.verdict = v[k].verdict;
            msg.flags = v[k].assessed ? (uint8_t)LG_HV_ASSESSED : (uint8_t)0;
            msg.diagnosis_code = v[k].diag;
            msg.trend_s_to_failure = v[k].trend_s;
            memset(&ack, 0, sizeof ack);
            /* MsgSend returns the MsgReply status (0), not a length: check the reply header. */
            if (MsgSend(coid, &msg, sizeof msg, &ack, sizeof ack) == -1) {
                (void)pthread_mutex_lock(&g_out_lock);
                g_out.send_fail++;
                g_out.pending |= pending & ~((1u << k) - 1u); /* this one and the rest: next round */
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

/* ---- the health state (main thread writes, device thread reads, under g_h_lock) ---------------- */

static pthread_mutex_t g_h_lock = PTHREAD_MUTEX_INITIALIZER;
static lg_health_t g_h;
static uint32_t g_resets;
static uint32_t g_late_rounds;
static uint32_t g_rounds;

/* ---- /dev/lg/health ------------------------------------------------------------------------- */

static iofunc_attr_t g_attr;
static resmgr_connect_funcs_t g_connect_funcs;
static resmgr_io_funcs_t g_io_funcs;
static char g_text[2048];

static size_t health_text(char *buf, size_t len) {
    size_t n;
    int m;

    (void)pthread_mutex_lock(&g_h_lock);
    n = lg_health_text(&g_h, buf, len);
    (void)pthread_mutex_lock(&g_out_lock);
    m = snprintf(buf + n, len - n, "process rounds=%u late_rounds=%u sent=%u send_fail=%u refused=%u connects=%u "
                 "resets=%u\n",
                 (unsigned)g_rounds, (unsigned)g_late_rounds, (unsigned)g_out.sent, (unsigned)g_out.send_fail,
                 (unsigned)g_out.refused, (unsigned)g_out.connects, (unsigned)g_resets);
    (void)pthread_mutex_unlock(&g_out_lock);
    (void)pthread_mutex_unlock(&g_h_lock);
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
    len = health_text(g_text, sizeof g_text);
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
    logf_(SLOG2_WARNING, "write to %s from pid %d refused", LG_DEV_HEALTH, (int)ctp->info.pid);
    return EPERM;
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb) {
    lg_health_reset_req_t req;
    int rc = iofunc_devctl_default(ctp, msg, ocb);

    if (rc != _RESMGR_DEFAULT) {
        return rc;
    }
    if (msg->i.dcmd != DCMD_LG_HEALTH_RESET) {
        return ENOSYS;
    }
    if ((ocb->ioflag & _IO_FLAG_WR) == 0) {
        logf_(SLOG2_WARNING, "devctl reset from pid %d refused: descriptor not open for writing",
              (int)ctp->info.pid);
        return EPERM;
    }
    if (msg->i.nbytes < sizeof req || ctp->info.msglen < (int)(sizeof msg->i + sizeof req)) {
        return EINVAL;
    }
    memcpy(&req, _DEVCTL_DATA(msg->i), sizeof req);
    if (req.comp >= LG_COMP_COUNT) {
        return EINVAL;
    }
    (void)pthread_mutex_lock(&g_h_lock);
    lg_health_reset(&g_h, (uint8_t)req.comp, lg_now_ms());
    g_resets++;
    (void)pthread_mutex_unlock(&g_h_lock);
    logf_(SLOG2_WARNING, "health of %s reset by pid %d (maintenance)", lg_health_comp_name((uint8_t)req.comp),
          (int)ctp->info.pid);
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
    if (resmgr_attach(dpp, &rattr, LG_DEV_HEALTH, _FTYPE_ANY, 0, &g_connect_funcs, &g_io_funcs, &g_attr) == -1) {
        logf_(SLOG2_ERROR, "resmgr_attach(%s) failed: %s", LG_DEV_HEALTH, strerror(errno));
        return -1;
    }
    return start_thread(device_thread, dpp, LG_PRIO_HEALTH, "device");
}

static int load_thresholds(const char *path, lg_health_cfg_t *cfg) {
    static lg_json_doc_t doc;
    static char text[8192];
    char err[LG_ERR_LEN];
    size_t n;

    if (lg_read_file(path, text, sizeof text, &n) != 0 || lg_json_parse(&doc, text, n) != LG_JSON_OK) {
        logf_(SLOG2_ERROR, "cannot read or parse %s", path);
        return -1;
    }
    if (lg_health_cfg_from_json(&doc, cfg, err, sizeof err) != 0) {
        logf_(SLOG2_ERROR, "%s: %s", path, err);
        return -1;
    }
    return 0;
}

/* One round: take what the readers left and feed it to the core. Called with g_h_lock held. */
static void round_step(uint32_t now, uint32_t seen_seq[LG_HL_DEVICES], uint32_t last_new_ms[LG_HL_DEVICES],
                       uint32_t last_nodata_ms[LG_HL_DEVICES]) {
    int dev;

    for (dev = 0; dev < LG_HL_DEVICES; dev++) {
        reader_t *r = &g_rd[dev];
        uint8_t comp = dev == LG_HL_GATE_DEV ? (uint8_t)LG_COMP_GATE : (uint8_t)dev;
        uint32_t period = lg_health_poll_period_ms(&g_h, comp);
        uint32_t seq;
        uint32_t read_ms;
        lg_sensor_stats_t ss;
        lg_gate_stats_t gs;

        if (dev == LG_HL_GATE_DEV && lg_health_poll_period_ms(&g_h, LG_COMP_BATTERY) < period) {
            period = lg_health_poll_period_ms(&g_h, LG_COMP_BATTERY);
        }
        (void)pthread_mutex_lock(&r->lock);
        r->period_ms = period;
        seq = r->seq;
        read_ms = r->read_ms;
        ss = r->sensor;
        gs = r->gate;
        (void)pthread_mutex_unlock(&r->lock);

        if (seq != seen_seq[dev]) {
            seen_seq[dev] = seq;
            last_new_ms[dev] = now;
            if (dev == LG_HL_GATE_DEV) {
                lg_health_gate_in_t in;

                in.moves_done = gs.moves_done;
                in.last_travel_ms = gs.last_travel_ms;
                in.last_peak_ma = gs.last_peak_ma;
                in.battery_mv = gs.battery_mv;
                lg_health_on_gate(&g_h, &in, read_ms);
            } else {
                lg_health_sensor_in_t in;

                in.ticks = ss.acc_ticks;
                in.no_sample = ss.acc_no_sample;
                in.blind = ss.blind;
                in.dev_count = ss.dev_count;
                in.dev_sum = ss.dev_sum;
                in.rise_count = ss.rise_count;
                in.last_rise_ms = ss.last_rise_ms;
                in.last_fall_ms = ss.last_fall_ms;
                (void)lg_health_on_sensor(&g_h, comp, &in, read_ms);
            }
        } else if (lg_elapsed_ge(now, last_new_ms[dev], 2u * period + LG_HEALTH_TICK_MS) &&
                   lg_elapsed_ge(now, last_nodata_ms[dev], period)) {
            /* No reading for two periods: the device is missing, failing or blocked. */
            last_nodata_ms[dev] = now;
            lg_health_on_no_data(&g_h, comp, now);
        }
    }
}

int main(int argc, char **argv) {
    slog2_buffer_set_config_t scfg;
    struct sched_param sp;
    lg_health_cfg_t cfg = lg_health_cfg_default();
    const char *thresholds = NULL;
    static const char *const dir_names[LG_DIR_COUNT] = {LG_DIR_NAME_WEST, LG_DIR_NAME_EAST};
    static const char *const id_names[LG_SENSOR_ID_COUNT] = {LG_SENSOR_NAME_A, LG_SENSOR_NAME_B,
                                                             LG_SENSOR_NAME_ISLAND};
    uint32_t seen_seq[LG_HL_DEVICES];
    uint32_t last_new_ms[LG_HL_DEVICES];
    uint32_t last_nodata_ms[LG_HL_DEVICES];
    uint32_t last_sent_ms[LG_COMP_COUNT];
    lg_health_out_t sent[LG_COMP_COUNT];
    struct timespec next;
    uint32_t now;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            thresholds = argv[++i];
        } else {
            (void)fprintf(stderr, "usage: lg-health [-t <thresholds.json>] [-v]\n");
            return 2;
        }
    }
    memset(&scfg, 0, sizeof scfg);
    scfg.num_buffers = 1;
    scfg.buffer_set_name = "lg-health";
    scfg.verbosity_level = SLOG2_INFO;
    scfg.buffer_config[0].buffer_name = "main";
    scfg.buffer_config[0].num_pages = 4;
    if (slog2_register(&scfg, &g_log, 0) != 0) {
        g_log = NULL;
    }
    if (thresholds != NULL && load_thresholds(thresholds, &cfg) != 0) {
        return 1; /* refuse to run on thresholds nobody checked */
    }
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = LG_PRIO_HEALTH;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        logf_(SLOG2_WARNING, "could not set SCHED_FIFO %d (continuing)", LG_PRIO_HEALTH);
    }
    now = lg_now_ms();
    lg_health_init(&g_h, &cfg, now);
    for (i = 0; i < LG_HL_DEVICES; i++) {
        reader_t *r = &g_rd[i];

        (void)pthread_mutex_init(&r->lock, NULL);
        r->period_ms = cfg.poll_ms;
        if (i == LG_HL_GATE_DEV) {
            (void)snprintf(r->path, sizeof r->path, "%s", LG_DEV_GATE);
        } else {
            (void)snprintf(r->path, sizeof r->path, LG_DEV_SENSOR_FMT, dir_names[i / LG_SENSOR_ID_COUNT],
                           id_names[i % LG_SENSOR_ID_COUNT]);
        }
        seen_seq[i] = 0;
        last_new_ms[i] = now;
        last_nodata_ms[i] = now - cfg.poll_ms;
        if (start_thread(reader_thread, r, LG_PRIO_HEALTH, "reader") != 0) {
            return 1;
        }
    }
    for (i = 0; i < LG_COMP_COUNT; i++) {
        last_sent_ms[i] = now - LG_HEALTH_REFRESH_MS;
        memset(&sent[i], 0, sizeof sent[i]);
        sent[i].verdict = 0xFF; /* nothing sent yet */
    }
    if (setup_device() != 0 || start_thread(sender, NULL, LG_PRIO_HEALTH, "sender") != 0) {
        return 1;
    }
    logf_(SLOG2_INFO, "up: %s attached, thresholds %s, poll %u/%u ms, no gate command path", LG_DEV_HEALTH,
          thresholds != NULL ? thresholds : "built-in", (unsigned)cfg.poll_ms, (unsigned)cfg.poll_fast_ms);
    if (lg_hb_start(LG_PROC_HEALTH) != 0) {
        logf_(SLOG2_WARNING, "heartbeat thread not started: no supervision");
    }
    g_loop_started = 1;
    if (clock_gettime(CLOCK_MONOTONIC, &next) != 0) {
        logf_(SLOG2_ERROR, "clock_gettime failed: %s", strerror(errno));
        return 1;
    }

    for (;;) {
        lg_health_out_t changed[LG_COMP_COUNT];
        uint32_t post = 0;
        uint8_t k;

        next.tv_nsec += (long)LG_HEALTH_TICK_MS * 1000000L;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {
        }
        now = lg_now_ms();
        {
            uint64_t want = (uint64_t)next.tv_sec * 1000ull + (uint64_t)next.tv_nsec / 1000000ull;

            if ((uint32_t)now - (uint32_t)want > 2u * LG_HEALTH_TICK_MS) {
                /* Held up (the VM or higher priorities): continue from now, no burst of rounds. */
                g_late_rounds++;
                (void)clock_gettime(CLOCK_MONOTONIC, &next);
            }
        }
        (void)pthread_mutex_lock(&g_h_lock);
        g_rounds++;
        round_step(now, seen_seq, last_new_ms, last_nodata_ms);
        for (k = 0; k < LG_COMP_COUNT; k++) {
            lg_health_out_t o = lg_health_get(&g_h, k);

            if (o.verdict != sent[k].verdict || o.diag != sent[k].diag || o.assessed != sent[k].assessed ||
                lg_elapsed_ge(now, last_sent_ms[k], LG_HEALTH_REFRESH_MS)) {
                if (o.verdict != sent[k].verdict && sent[k].verdict != 0xFF) {
                    post |= 1u << (k + 16); /* log it below, outside the lock */
                }
                changed[k] = o;
                sent[k] = o;
                last_sent_ms[k] = now;
                post |= 1u << k;
            }
        }
        (void)pthread_mutex_unlock(&g_h_lock);
        for (k = 0; k < LG_COMP_COUNT; k++) {
            if (post & (1u << k)) {
                post_verdict(k, &changed[k]);
            }
            if (post & (1u << (k + 16))) {
                logf_(changed[k].verdict == LG_VERDICT_OK ? SLOG2_INFO : SLOG2_WARNING,
                      "%s: verdict %u score %u diag %s trend %d s", lg_health_comp_name(k),
                      (unsigned)changed[k].verdict, (unsigned)changed[k].score, lg_health_diag_name(changed[k].diag),
                      (int)changed[k].trend_s);
            }
        }
        lg_hb_beat(); /* the loop that computes the verdicts is the proof of life */
    }
}
