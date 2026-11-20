/*
 * lg-meter: evaluation tool, NOT part of the control path (README 8.7). It watches the crossing from
 * the outside and produces the run metrics:
 *
 *   lg-meter -e <plant_ms> [-o file.csv] [-m <max_s>]
 *
 *  - every LG_TICK_MS it asks lg/plant for the ground truth of the island (LG_MSG_TRUTH_QUERY, the
 *    same message the sensors use) and reads /dev/lg/gate (state, lights);
 *  - it feeds lg_metrics_tick (core/metrics.c), including the wrong-side check: a train on the
 *    island while the gate is not DOWN;
 *  - it stops when the plant time reaches <plant_ms> (the end of the scenario) or after <max_s>
 *    seconds of its own clock (default 600), reads /dev/lg/controller once for the controller's own
 *    counters (late ticks, level, cycles) and prints and writes key,value lines.
 *
 * The controller never sees the ground truth: only this tool and the sensors ask the plant for it.
 * The meter never sends a message to the gate (it only reads the device text), so it has no way
 * to move it. Exit code: 0 when wrong_side_events is 0 and the observation was complete, 1 when
 * wrong_side_events is not 0, 2 usage, 3 the plant, the gate or the controller could not be reached
 * (or the output file could not be written), 4 the observation was not complete: less than 95
 * percent of the periods gave a sample, the gap between two samples exceeded MAX_GAP_MS, or the
 * time limit -m ended the run before the plant reached -e. A wrong-side check that could not look
 * is not a pass.
 *
 * Resolution: the checks are as fine as the meter's own sampling. The largest gap between two of
 * its samples is part of the output (meter_max_gap_ms); on the VirtualBox VM it is functional
 * evidence, not a hardware figure.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>

#include "lg_config.h"
#include "lg_gate_model.h"
#include "lg_metrics.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_target_util.h"

#define LG_DEV_CONTROLLER "/dev/lg/controller"
#define IPC_TIMEOUT_MS 20
#define MIN_COVERAGE_PCT 95
#define MAX_GAP_MS 200

static int query_island(int coid, uint32_t seq, lg_truth_reply_t *rep) {
    lg_truth_query_t q;
    uint64_t timeout_ns = (uint64_t)IPC_TIMEOUT_MS * 1000000ull;
    long rc;

    memset(&q, 0, sizeof q);
    q.hdr.type = LG_MSG_TRUTH_QUERY;
    q.hdr.version = LG_PROTO_VERSION;
    q.hdr.seq = seq;
    q.hdr.t_ns = lg_now_ns();
    q.dir = LG_DIR_WEST;
    q.id = LG_SENSOR_ISLAND;
    memset(rep, 0, sizeof *rep);
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout_ns, NULL) ==
        -1) {
        return 0;
    }
    rc = MsgSend(coid, &q, sizeof q, rep, sizeof *rep);
    /* MsgSend returns the MsgReply status, not a length; rep was zeroed, so a short reply fails here. */
    return rc != -1 && rep->hdr.type == LG_MSG_TRUTH_REPLY && rep->hdr.version == LG_PROTO_VERSION;
}

/* Reads a status text from a device into buf (NUL terminated). Returns the length or -1. */
static int read_text(int fd, char *buf, size_t len) {
    ssize_t n;

    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
        return -1;
    }
    n = read(fd, buf, len - 1);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    return (int)n;
}

/* Parses "state=NAME" into lg_gate_state_t. Returns -1 when it is not found or unknown. */
static int parse_gate_state(const char *text) {
    const char *p = strstr(text, "state=");
    int i;

    if (p == NULL) {
        return -1;
    }
    p += 6;
    for (i = LG_GATE_UP; i <= LG_GATE_FAULT; i++) {
        const char *name = lg_gate_state_name((uint8_t)i);
        size_t n = strlen(name);

        if (strncmp(p, name, n) == 0 && (p[n] == ' ' || p[n] == '\n' || p[n] == '\0')) {
            return i;
        }
    }
    return -1;
}

static int parse_uint_field(const char *text, const char *key, uint32_t *out) {
    const char *p = strstr(text, key);

    if (p == NULL) {
        return -1;
    }
    *out = (uint32_t)strtoul(p + strlen(key), NULL, 10);
    return 0;
}

int main(int argc, char **argv) {
    uint32_t end_plant_ms = 0;
    uint32_t max_s = 600;
    const char *out_path = NULL;
    lg_metrics_t m;
    struct sched_param sp;
    struct timespec next;
    uint64_t period_ns = (uint64_t)LG_TICK_MS * 1000000ull;
    uint64_t prev_ns = 0;
    uint64_t start_ns;
    uint64_t max_gap_ns = 0;      /* between two periods of the loop */
    uint64_t last_ok_ns = 0;      /* time of the last period that gave a complete sample */
    uint64_t max_ok_gap_ns = 0;   /* between two complete samples: the real resolution */
    uint32_t periods = 0;
    uint32_t samples = 0;
    uint32_t plant_fail = 0;
    uint32_t gate_fail = 0;
    uint32_t seq = 0;
    uint32_t plant_ms = 0;
    uint32_t controller_late = 0;
    uint32_t controller_cycles = 0;
    uint32_t controller_level = 99;
    char ctl_text[512];
    char gate_text[256];
    int plant_coid = -1;
    int gate_fd;
    int ctl_fd;
    int i;
    int rc = 0;
    FILE *out = NULL;
    uint32_t wall_ms;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            end_plant_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            max_s = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else {
            end_plant_ms = 0;
            break;
        }
    }
    if (end_plant_ms == 0 || max_s == 0) {
        (void)fprintf(stderr, "usage: lg-meter -e <plant_ms> [-o file.csv] [-m <max_s>]\n");
        return 2;
    }

    memset(&sp, 0, sizeof sp);
    sp.sched_priority = LG_PRIO_METER;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        (void)fprintf(stderr, "lg-meter: could not set SCHED_FIFO %d (continuing)\n", LG_PRIO_METER);
    }

    for (i = 0; i < 50 && plant_coid < 0; i++) { /* wait up to 5 s for the plant */
        plant_coid = name_open(LG_NAME_PLANT, 0);
        if (plant_coid < 0) {
            struct timespec w = {0, 100000000L};
            (void)nanosleep(&w, NULL);
        }
    }
    gate_fd = open(LG_DEV_GATE, O_RDONLY);
    ctl_fd = open(LG_DEV_CONTROLLER, O_RDONLY);
    if (plant_coid < 0 || gate_fd < 0 || ctl_fd < 0) {
        (void)fprintf(stderr, "lg-meter: cannot reach plant (%d), gate (%d) or controller (%d)\n",
                      plant_coid, gate_fd, ctl_fd);
        return 3;
    }

    lg_metrics_init(&m, 0);
    start_ns = lg_now_ns();
    (void)clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        lg_truth_reply_t truth;
        uint64_t now_ns;
        int gate_state;
        uint32_t lights = 0;

        next.tv_nsec += (long)period_ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        now_ns = lg_now_ns();
        if (prev_ns != 0 && now_ns - prev_ns > max_gap_ns) {
            max_gap_ns = now_ns - prev_ns;
        }
        prev_ns = now_ns;
        wall_ms = (uint32_t)((now_ns - start_ns) / 1000000ull);
        if (wall_ms >= max_s * 1000u) {
            break;
        }
        periods++;

        if (!query_island(plant_coid, ++seq, &truth)) {
            plant_fail++;
            continue;
        }
        plant_ms = truth.plant_time_ms;
        if (read_text(gate_fd, gate_text, sizeof gate_text) < 0 ||
            (gate_state = parse_gate_state(gate_text)) < 0 ||
            parse_uint_field(gate_text, "lights=", &lights) != 0) {
            gate_fail++;
            continue;
        }
        lg_metrics_tick(&m, wall_ms, (uint8_t)gate_state, (uint8_t)(lights != 0), truth.island_any, 0);
        samples++;
        if (last_ok_ns != 0 && now_ns - last_ok_ns > max_ok_gap_ns) {
            max_ok_gap_ns = now_ns - last_ok_ns;
        }
        last_ok_ns = now_ns;
        if (plant_ms >= end_plant_ms) {
            break;
        }
    }

    ctl_text[0] = '\0';
    if (read_text(ctl_fd, ctl_text, sizeof ctl_text) >= 0) {
        (void)parse_uint_field(ctl_text, "late_ticks=", &controller_late);
        (void)parse_uint_field(ctl_text, " cycles=", &controller_cycles);
        (void)parse_uint_field(ctl_text, "level=", &controller_level);
    }
    m.deadline_misses = controller_late; /* the controller counts its own late ticks */

    if (out_path != NULL) {
        out = fopen(out_path, "w");
        if (out == NULL) {
            (void)fprintf(stderr, "lg-meter: cannot write %s: %s\n", out_path, strerror(errno));
            rc = 3;
        }
    }
    for (i = 0; i < 2; i++) {
        FILE *f = i == 0 ? stdout : out;

        if (f == NULL) {
            continue;
        }
        (void)fprintf(f, "key,value\n");
        (void)fprintf(f, "plant_end_ms,%u\n", (unsigned)plant_ms);
        (void)fprintf(f, "meter_samples,%u\n", (unsigned)samples);
        (void)fprintf(f, "meter_plant_fail,%u\n", (unsigned)plant_fail);
        (void)fprintf(f, "meter_gate_fail,%u\n", (unsigned)gate_fail);
        (void)fprintf(f, "meter_periods,%u\n", (unsigned)periods);
        (void)fprintf(f, "meter_coverage_pct,%u\n", periods ? (unsigned)((uint64_t)samples * 100u / periods) : 0u);
        (void)fprintf(f, "meter_max_gap_ms,%u\n", (unsigned)(max_gap_ns / 1000000ull));
        (void)fprintf(f, "meter_max_sample_gap_ms,%u\n", (unsigned)(max_ok_gap_ns / 1000000ull));
        (void)fprintf(f, "trains,%u\n", (unsigned)m.trains);
        (void)fprintf(f, "trains_without_warning,%u\n", (unsigned)m.trains_without_warning);
        (void)fprintf(f, "warning_count,%u\n", (unsigned)m.warning.count);
        (void)fprintf(f, "warning_max_ms,%u\n", (unsigned)m.warning.max_ms);
        (void)fprintf(f, "warning_avg_ms,%u\n",
                      m.warning.count ? (unsigned)(m.warning.sum_ms / m.warning.count) : 0u);
        (void)fprintf(f, "closure_count,%u\n", (unsigned)m.closure.count);
        (void)fprintf(f, "closure_max_ms,%u\n", (unsigned)m.closure.max_ms);
        (void)fprintf(f, "closure_avg_ms,%u\n",
                      m.closure.count ? (unsigned)(m.closure.sum_ms / m.closure.count) : 0u);
        (void)fprintf(f, "closed_ms,%llu\n", (unsigned long long)m.closed_ms);
        (void)fprintf(f, "observed_ms,%llu\n", (unsigned long long)m.observed_ms);
        (void)fprintf(f, "closure_ms_per_hour,%llu\n",
                      (unsigned long long)lg_metrics_closure_ms_per_hour(&m));
        (void)fprintf(f, "wrong_side_events,%u\n", (unsigned)m.wrong_side_events);
        (void)fprintf(f, "wrong_side_ticks,%u\n", (unsigned)m.wrong_side_ticks);
        (void)fprintf(f, "controller_late_ticks,%u\n", (unsigned)controller_late);
        (void)fprintf(f, "controller_cycles,%u\n", (unsigned)controller_cycles);
        (void)fprintf(f, "controller_level_end,%u\n", (unsigned)controller_level);
    }
    if (out != NULL) {
        (void)fclose(out);
    }
    if (!lg_metrics_wrong_side_ok(&m)) {
        (void)fprintf(stderr, "lg-meter: WRONG-SIDE EVENTS: %u\n", (unsigned)m.wrong_side_events);
        return 1;
    }
    if (rc == 0 && (periods == 0 || (uint64_t)samples * 100u < (uint64_t)periods * MIN_COVERAGE_PCT ||
                    max_ok_gap_ns > (uint64_t)MAX_GAP_MS * 1000000ull || plant_ms < end_plant_ms)) {
        (void)fprintf(stderr,
                      "lg-meter: observation incomplete (periods %u, samples %u, largest gap %u ms, "
                      "plant time %u of %u ms): no verdict\n",
                      (unsigned)periods, (unsigned)samples, (unsigned)(max_ok_gap_ns / 1000000ull),
                      (unsigned)plant_ms, (unsigned)end_plant_ms);
        return 4;
    }
    return rc;
}
