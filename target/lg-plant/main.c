/*
 * lg-plant: the simulated world (README 4.2). It moves trains along a scenario and answers the
 * sensors' questions "what do I see now" with ground truth. The controller never talks to it.
 *
 *   lg-plant --crossing <crossing.json> --scenario <scenario.json> [--epoch-ms <ms>] [-v]
 *
 *  - name "lg/plant"      message passing: LG_MSG_TRUTH_QUERY in, LG_MSG_TRUTH_REPLY out.
 *  - /dev/lg/plant        read: one line of text (scenario time, trains waiting/active/finished).
 *                         Writes are refused.
 *  - slog2                train events (strike-in, island entry, island exit, island cleared).
 *
 * The truth is a pure function of the scenario and the clock (core/plant_model.c), so the plant
 * keeps no per-tick state. Scenario time 0 is the moment this process starts, or the absolute
 * CLOCK_MONOTONIC time (32-bit ms) given with --epoch-ms: lg-supervisor passes the same epoch on
 * every (re)start, so a restarted plant continues the scenario instead of replaying it.
 * One thread, one dispatch loop, default channel: the plant takes
 * the priority of the sensor that asks (priority inheritance is wanted here, the callers are the
 * higher-priority sensors).
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
#include "lg_heartbeat.h"
#include "lg_names.h"
#include "lg_plant_model.h"
#include "lg_proto.h"
#include "lg_scenario.h"
#include "lg_target_util.h"

#define FILE_MAX 16384

static lg_plant_t g_plant;
static lg_json_doc_t g_doc;
static char g_file[FILE_MAX];
static uint32_t g_t0_ms;
static int g_epoch_given;
static int64_t g_prev_ms = -1; /* -1: the first window starts before t = 0 */
static uint32_t g_seq;
static slog2_buffer_t g_log;
static int g_verbose;
static int g_loop_started; /* after this, logging goes to slog2 only */

static iofunc_attr_t g_attr;
static resmgr_connect_funcs_t g_connect_funcs;
static resmgr_io_funcs_t g_io_funcs;
static char g_text[200];

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
    /* stderr only while starting up: a stuck terminal must not block the loop. */
    if ((g_verbose || severity <= SLOG2_ERROR) && !g_loop_started) {
        (void)fprintf(stderr, "lg-plant: %s\n", line);
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

static uint32_t plant_ms(void) {
    return now_ms() - g_t0_ms;
}

static void fill_hdr(lg_hdr_t *h, uint16_t type) {
    h->type = type;
    h->version = LG_PROTO_VERSION;
    h->seq = ++g_seq;
    h->t_ns = lg_now_ns();
}

/* ---- /dev/lg/plant ------------------------------------------------------------------------- */

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
    len = lg_plant_text(&g_plant, plant_ms(), g_text, sizeof g_text);
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
    logf_(SLOG2_WARNING, "write to %s from pid %d refused", LG_DEV_PLANT, (int)ctp->info.pid);
    return EPERM;
}

/* ---- messages and pulses ------------------------------------------------------------------- */

static int on_truth_query(message_context_t *ctp, int code, unsigned flags, void *handle) {
    const lg_truth_query_t *q = (const lg_truth_query_t *)ctp->msg;
    lg_truth_reply_t rep;
    lg_truth_t truth;
    uint32_t t = plant_ms();

    (void)flags;
    (void)handle;

    if ((size_t)ctp->info.msglen < sizeof *q || q->hdr.type != (uint16_t)code ||
        q->hdr.version != LG_PROTO_VERSION || q->dir >= LG_DIR_COUNT || q->id >= LG_SENSOR_ID_COUNT) {
        logf_(SLOG2_WARNING, "malformed TRUTH_QUERY from pid %d (len %d)", (int)ctp->info.pid,
              (int)ctp->info.msglen);
        (void)MsgError(ctp->rcvid, EINVAL);
        return 0;
    }
    truth = lg_plant_truth(&g_plant, t, q->dir, q->id);
    memset(&rep, 0, sizeof rep);
    fill_hdr(&rep.hdr, LG_MSG_TRUTH_REPLY);
    rep.dir = q->dir;
    rep.id = q->id;
    rep.occupied = truth.occupied;
    rep.island_any = truth.island_any;
    rep.axle_count = truth.axles;
    rep.plant_time_ms = t;
    (void)MsgReply(ctp->rcvid, EOK, &rep, sizeof rep);
    return 0;
}

static const char *event_name(uint8_t kind) {
    switch (kind) {
    case LG_PLANT_EV_STRIKE_IN: return "reaches the strike-in point";
    case LG_PLANT_EV_ISLAND_ENTER: return "enters the island";
    case LG_PLANT_EV_ISLAND_EXIT: return "front leaves the island";
    case LG_PLANT_EV_CLEARED: return "has cleared the island";
    default: return "?";
    }
}

static int on_tick(message_context_t *ctp, int code, unsigned flags, void *handle) {
    lg_plant_event_t ev[16];
    uint32_t now = plant_ms();
    size_t n;
    size_t i;

    (void)ctp;
    (void)code;
    (void)flags;
    (void)handle;

    n = lg_plant_events(&g_plant, g_prev_ms, now, ev, 16);
    for (i = 0; i < n; i++) {
        logf_(SLOG2_INFO, "t=%u ms: train %u (from %s) %s", (unsigned)ev[i].t_ms,
              (unsigned)ev[i].train_id, ev[i].dir == LG_DIR_WEST ? "west" : "east",
              event_name(ev[i].kind));
    }
    g_prev_ms = now;
    lg_hb_beat();
    return 0;
}

/* ---- setup --------------------------------------------------------------------------------- */

static void setup_logging(void) {
    slog2_buffer_set_config_t cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.num_buffers = 1;
    cfg.buffer_set_name = "lg-plant";
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "main";
    cfg.buffer_config[0].num_pages = 4;
    if (slog2_register(&cfg, &g_log, 0) != 0) {
        g_log = NULL;
        (void)fprintf(stderr, "lg-plant: slog2_register failed: %s\n", strerror(errno));
    }
}

static void setup_priority(void) {
    struct sched_param sp;
    int rc;

    memset(&sp, 0, sizeof sp);
    sp.sched_priority = LG_PRIO_PLANT;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) {
        logf_(SLOG2_WARNING, "could not set SCHED_FIFO %d: %s (continuing at default priority)",
              LG_PRIO_PLANT, strerror(rc));
    } else {
        logf_(SLOG2_INFO, "running SCHED_FIFO priority %d", LG_PRIO_PLANT);
    }
}

static int start_event_timer(dispatch_t *dpp) {
    struct sigevent ev;
    struct itimerspec its;
    timer_t timer;
    int coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);

    if (coid == -1) {
        logf_(SLOG2_ERROR, "message_connect failed: %s", strerror(errno));
        return -1;
    }
    /* Explicit priority: SIGEV_PULSE_PRIO_INHERIT would run the receiver at the priority the process
     * was created with (measured on the VM), not at this thread's priority. */
    SIGEV_PULSE_INIT(&ev, coid, LG_PRIO_PLANT, LG_PULSE_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        logf_(SLOG2_ERROR, "timer_create failed: %s", strerror(errno));
        return -1;
    }
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = (long)LG_PLANT_EVENT_PERIOD_MS * 1000000L;
    its.it_interval.tv_nsec = (long)LG_PLANT_EVENT_PERIOD_MS * 1000000L;
    if (timer_settime(timer, 0, &its, NULL) == -1) {
        logf_(SLOG2_ERROR, "timer_settime failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* Reads and validates the two files. On any problem: say why and refuse to start. */
static int load_config(const char *crossing_path, const char *scenario_path) {
    lg_crossing_cfg_t cfg;
    lg_scenario_t sc;
    char err[LG_ERR_LEN];
    size_t len = 0;
    int rc;

    if (lg_read_file(crossing_path, g_file, sizeof g_file, &len) != 0) {
        logf_(SLOG2_ERROR, "cannot read %s", crossing_path);
        return -1;
    }
    rc = lg_json_parse(&g_doc, g_file, len);
    if (rc != LG_JSON_OK) {
        logf_(SLOG2_ERROR, "%s: %s", crossing_path, lg_json_error_name(rc));
        return -1;
    }
    if (lg_crossing_from_json(&g_doc, &cfg, err, sizeof err) != 0) {
        logf_(SLOG2_ERROR, "%s: %s", crossing_path, err);
        return -1;
    }
    if (lg_read_file(scenario_path, g_file, sizeof g_file, &len) != 0) {
        logf_(SLOG2_ERROR, "cannot read %s", scenario_path);
        return -1;
    }
    rc = lg_json_parse(&g_doc, g_file, len);
    if (rc != LG_JSON_OK) {
        logf_(SLOG2_ERROR, "%s: %s", scenario_path, lg_json_error_name(rc));
        return -1;
    }
    if (lg_scenario_from_json(&g_doc, &cfg, &sc, err, sizeof err) != 0) {
        logf_(SLOG2_ERROR, "%s: %s", scenario_path, err);
        return -1;
    }
    lg_plant_init(&g_plant, &cfg, &sc);
    logf_(SLOG2_INFO, "scenario %s: %u trains, strike-in %d m, island %d m", sc.name,
          (unsigned)sc.n_trains, (int)(cfg.strike_in_mm / 1000), (int)(cfg.island_mm / 1000));
    return 0;
}

int main(int argc, char **argv) {
    const char *crossing = NULL;
    const char *scenario = NULL;
    dispatch_t *dpp;
    dispatch_context_t *ctp;
    resmgr_attr_t rattr;
    name_attach_t *name;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--crossing") == 0 && i + 1 < argc) {
            crossing = argv[++i];
        } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            scenario = argv[++i];
        } else if (strcmp(argv[i], "--epoch-ms") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 10);

            if (end == NULL || *end != '\0' || argv[i][0] == '\0' || v > 0xFFFFFFFFul) {
                crossing = NULL;
                break;
            }
            g_t0_ms = (uint32_t)v;
            g_epoch_given = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else {
            crossing = NULL;
            break;
        }
    }
    if (crossing == NULL || scenario == NULL) {
        (void)fprintf(stderr, "usage: lg-plant --crossing <crossing.json> --scenario <scenario.json> [--epoch-ms <ms>] [-v]\n");
        return 2;
    }

    setup_logging();
    setup_priority();
    if (load_config(crossing, scenario) != 0) {
        return 1;
    }

    dpp = dispatch_create(); /* default channel: priority inheritance, see the header comment */
    if (dpp == NULL) {
        logf_(SLOG2_ERROR, "dispatch_create failed: %s", strerror(errno));
        return 1;
    }
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect_funcs, _RESMGR_IO_NFUNCS, &g_io_funcs);
    g_io_funcs.read = io_read;
    g_io_funcs.write = io_write;
    iofunc_attr_init(&g_attr, S_IFCHR | 0444, NULL, NULL);

    if (resmgr_attach(dpp, &rattr, LG_DEV_PLANT, _FTYPE_ANY, 0, &g_connect_funcs, &g_io_funcs,
                      &g_attr) == -1) {
        logf_(SLOG2_ERROR, "resmgr_attach(%s) failed: %s", LG_DEV_PLANT, strerror(errno));
        return 1;
    }
    name = name_attach(dpp, LG_NAME_PLANT, 0);
    if (name == NULL) {
        logf_(SLOG2_ERROR, "name_attach(%s) failed: %s", LG_NAME_PLANT, strerror(errno));
        return 1;
    }
    if (message_attach(dpp, NULL, LG_MSG_TRUTH_QUERY, LG_MSG_TRUTH_QUERY, on_truth_query, NULL) == -1) {
        logf_(SLOG2_ERROR, "message_attach failed: %s", strerror(errno));
        return 1;
    }
    if (pulse_attach(dpp, 0, LG_PULSE_TICK, on_tick, NULL) == -1) {
        logf_(SLOG2_ERROR, "pulse_attach failed: %s", strerror(errno));
        return 1;
    }
    if (!g_epoch_given) {
        g_t0_ms = now_ms(); /* scenario time 0 */
    } else {
        /* An epoch in the future would make the scenario time wrap to a huge number: refuse it. */
        if ((int32_t)(now_ms() - g_t0_ms) < 0) {
            logf_(SLOG2_ERROR, "--epoch-ms lies in the future, refusing to start");
            return 2;
        }
        g_prev_ms = (int64_t)plant_ms(); /* the events up to now were logged by the earlier run */
    }
    if (start_event_timer(dpp) != 0) {
        return 1;
    }
    ctp = dispatch_context_alloc(dpp);
    if (ctp == NULL) {
        logf_(SLOG2_ERROR, "dispatch_context_alloc failed: %s", strerror(errno));
        return 1;
    }
    logf_(SLOG2_INFO, "up: %s and name %s attached, scenario time %s (now %u ms)", LG_DEV_PLANT,
          LG_NAME_PLANT, g_epoch_given ? "continues from the epoch" : "starts now", (unsigned)plant_ms());
    if (lg_hb_start(LG_PROC_PLANT) != 0) {
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
