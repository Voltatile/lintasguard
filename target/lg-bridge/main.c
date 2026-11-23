/*
 * lg-bridge (README 4.2, 7.6, Step 10): streams the state of the crossing as NDJSON over TCP to the
 * host dashboard. It has NO gate command path (CLAUDE.md rule 4): it only reads the text of the
 * resource managers and never opens the gate or the controller by name or sends them a message
 * (checked by tests/static/check_bridge_no_gate.sh).
 *
 *   lg-bridge [-p <port>] [-v]          default port LG_BRIDGE_PORT (5555)
 *
 *  - main thread: select() over the listening socket and up to LG_BR_MAX_CLIENTS clients, 100 ms
 *    timeout; every 100 ms it sends a "state" line, new "level"/"notify" lines and, every second,
 *    "metric" lines to every client. Sends are non-blocking with a per-client buffer; a client that
 *    falls behind is dropped, never waited for. Inbound lines go through lg_bridge_parse_in (strict
 *    whitelist: subscribe, station_departure); everything else is answered with an error and
 *    counted. The heartbeat to the supervisor comes from this loop.
 *  - reader thread: reads /dev/lg/controller, /dev/lg/gate, /dev/lg/plant and /dev/lg/events every
 *    100 ms into a mailbox. A resource manager whose owner is stopped can block a read; only this
 *    thread waits then, the main loop keeps serving the last snapshot marked stale.
 *  - status thread: /dev/lg/bridge, read only (one line of counters); writes are refused (EPERM).
 *
 * station_departure is accepted and counted; forwarding it to lg-schedule comes with Step 12.
 * Metrics here are for display only; the evidence numbers come from lg-meter.
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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resmgr.h>
#include <sys/select.h>
#include <sys/slog2.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "lg_bridge_proto.h"
#include "lg_config.h"
#include "lg_gate_model.h"
#include "lg_heartbeat.h"
#include "lg_json_mini.h"
#include "lg_metrics.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_target_util.h"
#include "lg_time.h"

#define LG_DEV_BRIDGE "/dev/lg/bridge"
#define LG_BR_MAX_CLIENTS 8
#define LG_BR_OUTBUF 16384        /* per client; more pending than this = the client is too slow */
#define LG_BR_PERIOD_MS 100       /* state lines and device reads */
#define LG_BR_METRIC_PERIOD_MS 1000
#define LG_BR_STALE_MS 1000       /* no fresh snapshot for this long: state lines say stale */
#define LG_BR_EV_QUEUE 64         /* event lines waiting for the main loop */
#define LG_BR_EV_PER_READ 40      /* events parsed from one read of /dev/lg/events (the ring holds 32) */
#define LG_BR_DEV_TEXT 6144

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
        (void)fprintf(stderr, "lg-bridge: %s\n", line);
    }
}

/* ---- mailbox between the reader and the main loop --------------------------------------------- */

static pthread_mutex_t g_mb_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    lg_bridge_state_t state;  /* latest snapshot (t_ms = when the reader made it) */
    uint32_t snapshots;       /* number of snapshots made */
    char ev[LG_BR_EV_QUEUE][LG_BRIDGE_LINE_MAX]; /* event lines not yet sent */
    uint32_t ev_head;         /* next to send */
    uint32_t ev_tail;         /* next free */
    uint32_t ev_dropped;      /* queue full */
    uint64_t closure_ms_per_hour;
    uint32_t warning_last_ms;
    uint32_t trains;
} g_mb;

/* ---- counters for /dev/lg/bridge (main loop writes, status thread reads under the lock) -------- */

static pthread_mutex_t g_st_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint32_t clients;
    uint32_t accepted_clients;
    uint32_t dropped_slow;
    uint32_t refused_full;
    uint32_t refused_error; /* accepted socket could not be made non-blocking */
    uint32_t lines_sent;
    uint32_t in_lines;
    uint32_t in_subscribe;
    uint32_t in_station;
    uint32_t in_rejected;
    uint32_t snapshots;
    uint32_t stale;
    uint32_t ev_sent;
    uint32_t ev_replayed;
    uint32_t ev_dropped; /* copy of the mailbox counter: event lines lost because the queue was full */
    int port;
} g_st;

/* ---- reader thread ----------------------------------------------------------------------------- */

/* One read() per open: our resource managers build the whole text for every read at offset 0 and it fits
 * the buffer, and a second read would only make the server build it again (review finding L1). */
static int read_dev(const char *path, char *buf, size_t len) {
    int fd = open(path, O_RDONLY);
    ssize_t n;

    buf[0] = '\0';
    if (fd == -1) {
        return -1;
    }
    n = read(fd, buf, len - 1);
    (void)close(fd);
    if (n <= 0) {
        buf[0] = '\0';
        return -1;
    }
    buf[n] = '\0';
    return 0;
}

static uint8_t gate_state_code(const char *name) {
    static const char *names[] = {"UP", "LOWERING", "DOWN", "RAISING", "FAULT"};
    uint8_t i;

    for (i = 0; i < 5; i++) {
        if (strcmp(name, names[i]) == 0) {
            return i;
        }
    }
    return LG_GATE_FAULT;
}

static void *reader(void *arg) {
    static char text[LG_BR_DEV_TEXT];
    static lg_metrics_t metrics;
    static lg_bridge_event_t evs[LG_BR_EV_PER_READ];
    lg_bridge_evtrack_t track;
    int metrics_started = 0;

    (void)arg;
    lg_bridge_evtrack_init(&track);
    for (;;) {
        lg_bridge_state_t st;
        uint32_t now = lg_now_ms();
        const char *line;

        memset(&st, 0, sizeof st);
        st.t_ms = now;
        if (read_dev("/dev/lg/controller", text, sizeof text) == 0) {
            (void)lg_bridge_from_controller(&st, text);
        }
        if (read_dev(LG_DEV_GATE, text, sizeof text) == 0) {
            (void)lg_bridge_from_gate(&st, text);
        }
        if (read_dev(LG_DEV_PLANT, text, sizeof text) == 0) {
            (void)lg_bridge_from_plant(&st, text);
        }
        if (st.gate_ok) {
            if (!metrics_started) {
                lg_metrics_init(&metrics, now);
                metrics_started = 1;
            }
            lg_metrics_tick(&metrics, now, gate_state_code(st.gate_state), (uint8_t)(st.lights != 0),
                            (uint8_t)(st.island != 0), 0);
        }

        (void)pthread_mutex_lock(&g_mb_lock);
        g_mb.state = st;
        g_mb.snapshots++;
        if (metrics_started) {
            g_mb.closure_ms_per_hour = lg_metrics_closure_ms_per_hour(&metrics);
            g_mb.warning_last_ms = metrics.warning.last_ms;
            g_mb.trains = metrics.trains;
        }
        (void)pthread_mutex_unlock(&g_mb_lock);

        /* New events, oldest first. A restarted controller numbers again from 1: the tracker (core) finds
         * the last event sent by seq and time, so the first events of a new controller are not skipped
         * (review finding M2). */
        if (read_dev(LG_DEV_EVENTS, text, sizeof text) == 0) {
            uint32_t nev = 0;
            uint32_t first;
            uint32_t i;

            for (line = text; line != NULL && *line != '\0' && nev < LG_BR_EV_PER_READ;) {
                const char *end = strchr(line, '\n');
                size_t n = end ? (size_t)(end - line) : strlen(line);
                char one[200];

                if (n < sizeof one) {
                    memcpy(one, line, n);
                    one[n] = '\0';
                    if (lg_bridge_parse_event(one, &evs[nev]) == 0) {
                        nev++;
                    }
                }
                line = end ? end + 1 : NULL;
            }
            first = lg_bridge_evtrack_first_new(&track, evs, nev);
            for (i = first; i < nev; i++) {
                char json[LG_BRIDGE_LINE_MAX];

                if (lg_bridge_event_json(&evs[i], json, sizeof json) > 0) {
                    (void)pthread_mutex_lock(&g_mb_lock);
                    if (g_mb.ev_tail - g_mb.ev_head < LG_BR_EV_QUEUE) {
                        memcpy(g_mb.ev[g_mb.ev_tail % LG_BR_EV_QUEUE], json, sizeof json);
                        g_mb.ev_tail++;
                    } else {
                        g_mb.ev_dropped++;
                    }
                    (void)pthread_mutex_unlock(&g_mb_lock);
                }
            }
        }
        {
            struct timespec ts = {0, (long)LG_BR_PERIOD_MS * 1000000L};

            (void)nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

/* ---- clients ----------------------------------------------------------------------------------- */

typedef struct {
    int fd;
    char out[LG_BR_OUTBUF];
    size_t out_len;
    char in[LG_BRIDGE_IN_MAX + 2];
    size_t in_len;
    int discarding; /* an inbound line was too long: skip to the next newline */
} client_t;

static client_t g_cl[LG_BR_MAX_CLIENTS];
static lg_json_doc_t g_doc;
/* Event lines already sent, replayed to a client that connects later (main thread only). */
static lg_bridge_recent_t g_recent;

static void client_close(client_t *c, const char *why) {
    if (c->fd >= 0) {
        logf_(SLOG2_INFO, "client fd %d closed: %s", c->fd, why);
        (void)close(c->fd);
    }
    c->fd = -1;
    c->out_len = 0;
    c->in_len = 0;
    c->discarding = 0;
    (void)pthread_mutex_lock(&g_st_lock);
    if (g_st.clients > 0) {
        g_st.clients--;
    }
    (void)pthread_mutex_unlock(&g_st_lock);
}

/* Queue a line for one client; a client that cannot take it is too slow and is dropped. */
static void client_queue(client_t *c, const char *line, size_t n) {
    if (c->fd < 0 || n == 0) {
        return;
    }
    if (c->out_len + n > sizeof c->out) {
        (void)pthread_mutex_lock(&g_st_lock);
        g_st.dropped_slow++;
        (void)pthread_mutex_unlock(&g_st_lock);
        client_close(c, "too slow");
        return;
    }
    memcpy(c->out + c->out_len, line, n);
    c->out_len += n;
    (void)pthread_mutex_lock(&g_st_lock);
    g_st.lines_sent++;
    (void)pthread_mutex_unlock(&g_st_lock);
}

static void broadcast(const char *line, size_t n) {
    int i;

    for (i = 0; i < LG_BR_MAX_CLIENTS; i++) {
        client_queue(&g_cl[i], line, n);
    }
}

static void client_flush(client_t *c) {
    ssize_t n;

    if (c->fd < 0 || c->out_len == 0) {
        return;
    }
    n = send(c->fd, c->out, c->out_len, 0); /* SIGPIPE is ignored */
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            client_close(c, strerror(errno));
        }
        return;
    }
    memmove(c->out, c->out + n, c->out_len - (size_t)n);
    c->out_len -= (size_t)n;
}

static void handle_line(client_t *c, const char *line, size_t n) {
    lg_bridge_in_t in;
    char reply[256];
    size_t len;

    lg_bridge_parse_in(&g_doc, line, n, &in);
    (void)pthread_mutex_lock(&g_st_lock);
    g_st.in_lines++;
    if (in.kind == LG_BRIDGE_IN_SUBSCRIBE) {
        g_st.in_subscribe++;
    } else if (in.kind == LG_BRIDGE_IN_STATION_DEPARTURE) {
        g_st.in_station++;
    } else {
        g_st.in_rejected++;
    }
    (void)pthread_mutex_unlock(&g_st_lock);
    if (in.kind == LG_BRIDGE_IN_SUBSCRIBE) {
        len = lg_bridge_reply_json(1, "subscribe", NULL, reply, sizeof reply);
    } else if (in.kind == LG_BRIDGE_IN_STATION_DEPARTURE) {
        logf_(SLOG2_INFO, "station_departure dir=%u eta_s=%u accepted (lg-schedule arrives in Step 12)",
              (unsigned)in.dir, (unsigned)in.eta_s);
        len = lg_bridge_reply_json(1, "station_departure", NULL, reply, sizeof reply);
    } else {
        logf_(SLOG2_WARNING, "inbound line from fd %d rejected: %s", c->fd, in.reason);
        len = lg_bridge_reply_json(0, "rejected", in.reason, reply, sizeof reply);
    }
    client_queue(c, reply, len);
}

static void client_read(client_t *c) {
    char buf[512];
    ssize_t n = recv(c->fd, buf, sizeof buf, 0);
    ssize_t i;

    if (n == 0) {
        client_close(c, "closed by the host");
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            client_close(c, strerror(errno));
        }
        return;
    }
    for (i = 0; i < n && c->fd >= 0; i++) {
        char ch = buf[i];

        if (ch == '\n') {
            if (c->discarding) {
                char reply[128];
                size_t len = lg_bridge_reply_json(0, "rejected", "too_long", reply, sizeof reply);

                (void)pthread_mutex_lock(&g_st_lock);
                g_st.in_lines++;
                g_st.in_rejected++;
                (void)pthread_mutex_unlock(&g_st_lock);
                client_queue(c, reply, len);
                c->discarding = 0;
            } else {
                handle_line(c, c->in, c->in_len);
            }
            c->in_len = 0;
            continue;
        }
        if (c->discarding) {
            continue;
        }
        if (c->in_len >= LG_BRIDGE_IN_MAX) {
            c->discarding = 1;
            c->in_len = 0;
            continue;
        }
        c->in[c->in_len++] = ch;
    }
}

static void accept_client(int lfd) {
    int fd = accept(lfd, NULL, NULL);
    int i;
    int flags;

    if (fd < 0) {
        return;
    }
    for (i = 0; i < LG_BR_MAX_CLIENTS && g_cl[i].fd >= 0; i++) {
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (i == LG_BR_MAX_CLIENTS || flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        (void)pthread_mutex_lock(&g_st_lock);
        if (i == LG_BR_MAX_CLIENTS) {
            g_st.refused_full++;
        } else {
            g_st.refused_error++;
        }
        (void)pthread_mutex_unlock(&g_st_lock);
        (void)close(fd);
        return;
    }
    g_cl[i].fd = fd;
    g_cl[i].out_len = 0;
    g_cl[i].in_len = 0;
    g_cl[i].discarding = 0;
    (void)pthread_mutex_lock(&g_st_lock);
    g_st.clients++;
    g_st.accepted_clients++;
    (void)pthread_mutex_unlock(&g_st_lock);
    {
        /* The recent events first, oldest first, at most half the output buffer (never a drop). */
        uint32_t k = lg_bridge_recent_select(&g_recent, LG_BR_OUTBUF / 2);
        uint32_t j;

        for (j = g_recent.count - k; j < g_recent.count; j++) {
            const char *ev = lg_bridge_recent_get(&g_recent, j);

            client_queue(&g_cl[i], ev, strlen(ev));
        }
        (void)pthread_mutex_lock(&g_st_lock);
        g_st.ev_replayed += k;
        (void)pthread_mutex_unlock(&g_st_lock);
        logf_(SLOG2_INFO, "client connected on fd %d (slot %d), %u recent events replayed", fd, i, (unsigned)k);
    }
}

/* One round of output: a state line, queued event lines, and metric lines once a second. */
static void send_round(uint32_t now, uint32_t *last_metric) {
    lg_bridge_state_t st;
    char line[LG_BRIDGE_LINE_MAX];
    char evs[8][LG_BRIDGE_LINE_MAX];
    uint32_t nev = 0;
    uint64_t closure;
    uint32_t warn_ms;
    uint32_t trains;
    uint32_t snapshots;
    uint32_t dropped;
    size_t n;
    uint32_t k;

    (void)pthread_mutex_lock(&g_mb_lock);
    st = g_mb.state;
    snapshots = g_mb.snapshots;
    while (g_mb.ev_head != g_mb.ev_tail && nev < 8) {
        memcpy(evs[nev++], g_mb.ev[g_mb.ev_head % LG_BR_EV_QUEUE], LG_BRIDGE_LINE_MAX);
        g_mb.ev_head++;
    }
    closure = g_mb.closure_ms_per_hour;
    warn_ms = g_mb.warning_last_ms;
    trains = g_mb.trains;
    dropped = g_mb.ev_dropped;
    (void)pthread_mutex_unlock(&g_mb_lock);

    st.stale = (uint8_t)(snapshots == 0 || lg_elapsed_ge(now, st.t_ms, LG_BR_STALE_MS));
    st.t_ms = now;
    n = lg_bridge_state_json(&st, line, sizeof line);
    broadcast(line, n);
    for (k = 0; k < nev; k++) {
        broadcast(evs[k], strlen(evs[k]));
        lg_bridge_recent_push(&g_recent, evs[k]);
    }
    (void)pthread_mutex_lock(&g_st_lock);
    g_st.snapshots = snapshots;
    g_st.stale += st.stale;
    g_st.ev_sent += nev;
    g_st.ev_dropped = dropped;
    (void)pthread_mutex_unlock(&g_st_lock);
    if (lg_elapsed_ge(now, *last_metric, LG_BR_METRIC_PERIOD_MS)) {
        *last_metric = now;
        broadcast(line, lg_bridge_metric_json(now, "closure_ms_per_hour", (int64_t)closure, line, sizeof line));
        broadcast(line, lg_bridge_metric_json(now, "warning_time_ms", (int64_t)warn_ms, line, sizeof line));
        broadcast(line, lg_bridge_metric_json(now, "trains", (int64_t)trains, line, sizeof line));
    }
}

/* ---- /dev/lg/bridge ---------------------------------------------------------------------------- */

static iofunc_attr_t g_attr;
static resmgr_connect_funcs_t g_connect_funcs;
static resmgr_io_funcs_t g_io_funcs;
static char g_text[512];

static size_t status_text(char *buf, size_t len) {
    int n;

    (void)pthread_mutex_lock(&g_st_lock);
    n = snprintf(buf, len,
                 "port=%d clients=%u accepted=%u dropped_slow=%u refused_full=%u lines_sent=%u in_lines=%u "
                 "in_subscribe=%u in_station=%u in_rejected=%u snapshots=%u stale_rounds=%u events_sent=%u "
                 "events_replayed=%u events_dropped=%u refused_error=%u\n",
                 g_st.port, (unsigned)g_st.clients, (unsigned)g_st.accepted_clients, (unsigned)g_st.dropped_slow,
                 (unsigned)g_st.refused_full, (unsigned)g_st.lines_sent, (unsigned)g_st.in_lines,
                 (unsigned)g_st.in_subscribe, (unsigned)g_st.in_station, (unsigned)g_st.in_rejected,
                 (unsigned)g_st.snapshots, (unsigned)g_st.stale, (unsigned)g_st.ev_sent,
                 (unsigned)g_st.ev_replayed, (unsigned)g_st.ev_dropped, (unsigned)g_st.refused_error);
    (void)pthread_mutex_unlock(&g_st_lock);
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n >= len ? len - 1 : (size_t)n;
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
    len = status_text(g_text, sizeof g_text);
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
    logf_(SLOG2_WARNING, "write to %s from pid %d refused", LG_DEV_BRIDGE, (int)ctp->info.pid);
    return EPERM;
}

static void *status_device(void *arg) {
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

/* ---- setup ------------------------------------------------------------------------------------- */

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

static int setup_status_device(void) {
    dispatch_t *dpp = dispatch_create();
    resmgr_attr_t rattr;

    if (dpp == NULL) {
        logf_(SLOG2_ERROR, "dispatch_create failed: %s", strerror(errno));
        return -1;
    }
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 1024;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect_funcs, _RESMGR_IO_NFUNCS, &g_io_funcs);
    g_io_funcs.read = io_read;
    g_io_funcs.write = io_write;
    iofunc_attr_init(&g_attr, S_IFCHR | 0444, NULL, NULL);
    if (resmgr_attach(dpp, &rattr, LG_DEV_BRIDGE, _FTYPE_ANY, 0, &g_connect_funcs, &g_io_funcs, &g_attr) == -1) {
        logf_(SLOG2_ERROR, "resmgr_attach(%s) failed: %s", LG_DEV_BRIDGE, strerror(errno));
        return -1;
    }
    return start_thread(status_device, dpp, LG_PRIO_BRIDGE, "status device");
}

static int setup_listener(int port) {
    struct sockaddr_in addr;
    int one = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int flags;

    if (fd < 0) {
        logf_(SLOG2_ERROR, "socket failed: %s", strerror(errno));
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 4) != 0) {
        logf_(SLOG2_ERROR, "bind/listen on port %d failed: %s", port, strerror(errno));
        (void)close(fd);
        return -1;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        logf_(SLOG2_ERROR, "fcntl O_NONBLOCK failed: %s", strerror(errno));
        (void)close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    slog2_buffer_set_config_t cfg;
    struct sched_param sp;
    int port = LG_BRIDGE_PORT;
    int lfd;
    int i;
    uint32_t last_round;
    uint32_t last_metric;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else {
            (void)fprintf(stderr, "usage: lg-bridge [-p <port>] [-v]\n");
            return 2;
        }
    }
    if (port <= 0 || port > 65535) {
        (void)fprintf(stderr, "lg-bridge: bad port %d\n", port);
        return 2;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.num_buffers = 1;
    cfg.buffer_set_name = "lg-bridge";
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "main";
    cfg.buffer_config[0].num_pages = 4;
    if (slog2_register(&cfg, &g_log, 0) != 0) {
        g_log = NULL;
    }
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = LG_PRIO_BRIDGE;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        logf_(SLOG2_WARNING, "could not set SCHED_FIFO %d (continuing)", LG_PRIO_BRIDGE);
    } else {
        logf_(SLOG2_INFO, "running SCHED_FIFO priority %d", LG_PRIO_BRIDGE);
    }
    (void)signal(SIGPIPE, SIG_IGN);
    for (i = 0; i < LG_BR_MAX_CLIENTS; i++) {
        g_cl[i].fd = -1;
    }
    lg_bridge_recent_init(&g_recent);
    g_st.port = port;
    lfd = setup_listener(port);
    if (lfd < 0 || setup_status_device() != 0 || start_thread(reader, NULL, LG_PRIO_BRIDGE, "device reader") != 0) {
        return 1;
    }
    logf_(SLOG2_INFO, "up: listening on TCP port %d, %s attached, no gate command path", port, LG_DEV_BRIDGE);
    if (lg_hb_start(LG_PROC_BRIDGE) != 0) {
        logf_(SLOG2_WARNING, "heartbeat thread not started: no supervision");
    }
    g_loop_started = 1;
    /* Due at once, and valid at any uptime: 0 would be "in the future" between 2^31 and 2^32 ms of
     * uptime and stop every round for 25 days (review finding L2). */
    last_round = lg_now_ms() - LG_BR_PERIOD_MS;
    last_metric = lg_now_ms() - LG_BR_METRIC_PERIOD_MS;

    for (;;) {
        fd_set rd;
        fd_set wr;
        struct timeval tv = {0, 20000};
        int maxfd = lfd;
        uint32_t now;

        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(lfd, &rd);
        for (i = 0; i < LG_BR_MAX_CLIENTS; i++) {
            if (g_cl[i].fd >= 0) {
                FD_SET(g_cl[i].fd, &rd);
                if (g_cl[i].out_len > 0) {
                    FD_SET(g_cl[i].fd, &wr);
                }
                if (g_cl[i].fd > maxfd) {
                    maxfd = g_cl[i].fd;
                }
            }
        }
        if (select(maxfd + 1, &rd, &wr, NULL, &tv) < 0) {
            if (errno != EINTR) {
                logf_(SLOG2_ERROR, "select failed: %s", strerror(errno));
            }
            FD_ZERO(&rd);
            FD_ZERO(&wr);
        }
        lg_hb_beat(); /* the loop that serves the clients is the proof of life */
        if (FD_ISSET(lfd, &rd)) {
            accept_client(lfd);
        }
        for (i = 0; i < LG_BR_MAX_CLIENTS; i++) {
            if (g_cl[i].fd >= 0 && FD_ISSET(g_cl[i].fd, &rd)) {
                client_read(&g_cl[i]);
            }
        }
        now = lg_now_ms();
        if (lg_elapsed_ge(now, last_round, LG_BR_PERIOD_MS)) {
            last_round = now;
            send_round(now, &last_metric);
        }
        for (i = 0; i < LG_BR_MAX_CLIENTS; i++) {
            client_flush(&g_cl[i]);
        }
    }
}
