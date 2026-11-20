/*
 * lg-sample-sink: stand-in for lg-controller until Step 7. It registers the name lg/controller,
 * accepts LG_MSG_SENSOR_SAMPLE from the sensors, and after N seconds prints per sensor how many
 * samples arrived, how many sequence numbers were missing (gaps), and how old the samples were
 * when they arrived (now minus the timestamp in the header, same CLOCK_MONOTONIC).
 *
 *   lg-sample-sink [-t seconds]      default 10
 *
 * Remove or replace it when the controller exists.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <time.h>
#include <unistd.h>

#include "lg_names.h"
#include "lg_proto.h"
#include "lg_target_util.h"

#define SENSORS (LG_DIR_COUNT * LG_SENSOR_ID_COUNT)

typedef struct {
    uint32_t count;
    uint32_t gaps;    /* missing sequence numbers */
    uint32_t reorder; /* sequence number not above the previous one */
    uint32_t last_seq;
    uint64_t max_age_ns;
    uint64_t sum_age_ns;
    uint8_t seen;
} sensor_stat_t;

static sensor_stat_t g_stat[SENSORS];
static uint32_t g_invalid;
static uint32_t g_seq;

static int on_sample(message_context_t *ctp, int code, unsigned flags, void *handle) {
    const lg_sensor_sample_t *s = (const lg_sensor_sample_t *)ctp->msg;
    lg_ack_t ack;
    uint64_t now = lg_now_ns();
    sensor_stat_t *st;
    uint64_t age;

    (void)flags;
    (void)handle;

    if ((size_t)ctp->info.msglen < sizeof *s || s->hdr.type != (uint16_t)code ||
        s->hdr.version != LG_PROTO_VERSION || s->dir >= LG_DIR_COUNT || s->id >= LG_SENSOR_ID_COUNT) {
        g_invalid++;
        (void)MsgError(ctp->rcvid, EINVAL);
        return 0;
    }
    st = &g_stat[s->dir * LG_SENSOR_ID_COUNT + s->id];
    if (st->seen) {
        if (s->hdr.seq <= st->last_seq) {
            st->reorder++;
        } else if (s->hdr.seq > st->last_seq + 1) {
            st->gaps += s->hdr.seq - st->last_seq - 1;
        }
    }
    st->seen = 1;
    st->last_seq = s->hdr.seq;
    st->count++;
    age = now > s->hdr.t_ns ? now - s->hdr.t_ns : 0;
    st->sum_age_ns += age;
    if (age > st->max_age_ns) {
        st->max_age_ns = age;
    }

    memset(&ack, 0, sizeof ack);
    ack.hdr.type = LG_MSG_ACK;
    ack.hdr.version = LG_PROTO_VERSION;
    ack.hdr.seq = ++g_seq;
    ack.hdr.t_ns = now;
    (void)MsgReply(ctp->rcvid, EOK, &ack, sizeof ack);
    return 0;
}

static int on_done(message_context_t *ctp, int code, unsigned flags, void *handle) {
    int d;
    int i;

    (void)ctp;
    (void)code;
    (void)flags;
    (void)handle;

    for (d = 0; d < LG_DIR_COUNT; d++) {
        for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
            const sensor_stat_t *st = &g_stat[d * LG_SENSOR_ID_COUNT + i];
            const char *dir = d == LG_DIR_WEST ? LG_DIR_NAME_WEST : LG_DIR_NAME_EAST;
            const char *id = i == LG_SENSOR_A ? LG_SENSOR_NAME_A
                                               : (i == LG_SENSOR_B ? LG_SENSOR_NAME_B : LG_SENSOR_NAME_ISLAND);

            printf("sink: %s/%s count=%u gaps=%u reorder=%u max_age_us=%u avg_age_us=%u\n", dir, id,
                   (unsigned)st->count, (unsigned)st->gaps, (unsigned)st->reorder,
                   (unsigned)(st->max_age_ns / 1000u),
                   st->count ? (unsigned)(st->sum_age_ns / st->count / 1000u) : 0u);
        }
    }
    printf("sink: invalid=%u\n", (unsigned)g_invalid);
    (void)fflush(stdout);
    exit(0);
}

int main(int argc, char **argv) {
    unsigned seconds = 10;
    dispatch_t *dpp;
    dispatch_context_t *ctp;
    name_attach_t *name;
    struct sigevent ev;
    struct itimerspec its;
    timer_t timer;
    int coid;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            seconds = (unsigned)strtoul(argv[++i], NULL, 10);
        } else {
            (void)fprintf(stderr, "usage: lg-sample-sink [-t seconds]\n");
            return 2;
        }
    }
    if (seconds == 0) {
        (void)fprintf(stderr, "lg-sample-sink: -t must be above 0\n");
        return 2;
    }

    dpp = dispatch_create(); /* default channel: the sink inherits the priority of the sensor */
    if (dpp == NULL) {
        (void)fprintf(stderr, "dispatch_create failed: %s\n", strerror(errno));
        return 1;
    }
    name = name_attach(dpp, LG_NAME_CONTROLLER, 0);
    if (name == NULL) {
        (void)fprintf(stderr, "name_attach(%s) failed: %s\n", LG_NAME_CONTROLLER, strerror(errno));
        return 1;
    }
    if (message_attach(dpp, NULL, LG_MSG_SENSOR_SAMPLE, LG_MSG_SENSOR_SAMPLE, on_sample, NULL) == -1 ||
        pulse_attach(dpp, 0, LG_PULSE_SHUTDOWN, on_done, NULL) == -1) {
        (void)fprintf(stderr, "attach failed: %s\n", strerror(errno));
        return 1;
    }
    coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
    if (coid == -1) {
        (void)fprintf(stderr, "message_connect failed: %s\n", strerror(errno));
        return 1;
    }
    SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, LG_PULSE_SHUTDOWN, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        (void)fprintf(stderr, "timer_create failed: %s\n", strerror(errno));
        return 1;
    }
    memset(&its, 0, sizeof its);
    its.it_value.tv_sec = (time_t)seconds;
    if (timer_settime(timer, 0, &its, NULL) == -1) {
        (void)fprintf(stderr, "timer_settime failed: %s\n", strerror(errno));
        return 1;
    }
    ctp = dispatch_context_alloc(dpp);
    if (ctp == NULL) {
        (void)fprintf(stderr, "dispatch_context_alloc failed: %s\n", strerror(errno));
        return 1;
    }
    printf("sink: up, registered %s, running %u s\n", LG_NAME_CONTROLLER, seconds);
    (void)fflush(stdout);

    for (;;) {
        dispatch_context_t *next = dispatch_block(ctp);

        if (next == NULL) {
            if (errno == EINTR) {
                continue;
            }
            (void)fprintf(stderr, "dispatch_block failed: %s\n", strerror(errno));
            return 1;
        }
        ctp = next;
        (void)dispatch_handler(ctp);
    }
}
