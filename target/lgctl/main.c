/*
 * lgctl: command line control of LintasGuard (README 4.2, 10.2). Step 6 has the sensor commands
 * only; the other subcommands (kill, guard-ack, status) arrive with their steps.
 *
 *   lgctl sensor <dir>/<id> [fault] <mode> [param]    inject a fault through devctl
 *   lgctl sensor <dir>/<id> stats                     print the counters of the sensor
 *   lgctl sensor <dir>/<id> fault-ro <mode> [param]   test aid: the same through a read-only
 *                                                     descriptor, which the sensor must refuse
 *
 *   <dir> west|east, <id> a|b|island
 *   <mode> none noise dropout stuck-occupied stuck-clear delay drift hang crash
 *   param  noise level 0..100, dropout percent 0..100, delay ms 0..2000, drift units per second 0..100
 *
 * Examples: lgctl sensor west/a noise 50      lgctl sensor west/a fault drift 2
 *
 * Exit codes: 0 ok, 1 failure, 2 usage, 3 rejected as invalid (EINVAL), 4 refused (EPERM).
 * Fault injection needs the device opened for writing: access control proper is Step 13.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lg_devctl.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_sensor_model.h"

static const char *const dir_names[LG_DIR_COUNT] = {LG_DIR_NAME_WEST, LG_DIR_NAME_EAST};
static const char *const id_names[LG_SENSOR_ID_COUNT] = {LG_SENSOR_NAME_A, LG_SENSOR_NAME_B,
                                                         LG_SENSOR_NAME_ISLAND};

static int usage(void) {
    (void)fprintf(stderr,
                  "usage: lgctl sensor <dir>/<id> [fault] <mode> [param]\n"
                  "       lgctl sensor <dir>/<id> stats\n"
                  "  dir west|east, id a|b|island\n"
                  "  mode none noise dropout stuck-occupied stuck-clear delay drift hang crash\n");
    return 2;
}

static int parse_mode(const char *s) {
    if (strcmp(s, "none") == 0) return LG_SFAULT_NONE;
    if (strcmp(s, "noise") == 0) return LG_SFAULT_NOISE;
    if (strcmp(s, "dropout") == 0) return LG_SFAULT_DROPOUT;
    if (strcmp(s, "stuck-occupied") == 0) return LG_SFAULT_STUCK_OCCUPIED;
    if (strcmp(s, "stuck-clear") == 0) return LG_SFAULT_STUCK_CLEAR;
    if (strcmp(s, "delay") == 0) return LG_SFAULT_DELAY;
    if (strcmp(s, "drift") == 0) return LG_SFAULT_DRIFT;
    if (strcmp(s, "hang") == 0) return LG_SFAULT_HANG;
    if (strcmp(s, "crash") == 0) return LG_SFAULT_CRASH;
    return -1;
}

/* "west/a" into the device path. Returns 0 on success, -1 on a bad sensor name. */
static int sensor_path(const char *spec, char *path, size_t len) {
    char copy[32];
    char *slash;
    int dir = -1;
    int id = -1;
    int i;

    if (strlen(spec) >= sizeof copy) {
        return -1;
    }
    (void)strcpy(copy, spec);
    slash = strchr(copy, '/');
    if (slash == NULL) {
        return -1;
    }
    *slash = '\0';
    for (i = 0; i < LG_DIR_COUNT; i++) {
        if (strcmp(copy, dir_names[i]) == 0) {
            dir = i;
        }
    }
    for (i = 0; i < LG_SENSOR_ID_COUNT; i++) {
        if (strcmp(slash + 1, id_names[i]) == 0) {
            id = i;
        }
    }
    if (dir < 0 || id < 0) {
        return -1;
    }
    (void)snprintf(path, len, LG_DEV_SENSOR_FMT, dir_names[dir], id_names[id]);
    return 0;
}

static int do_fault(const char *path, int mode, uint32_t param, int read_only) {
    lg_sensor_fault_req_t req;
    int fd = open(path, read_only ? O_RDONLY : O_RDWR);
    int rc;

    if (fd == -1) {
        (void)fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }
    req.mode = (uint32_t)mode;
    req.param = param;
    rc = devctl(fd, DCMD_LG_SENSOR_FAULT, &req, sizeof req, NULL);
    (void)close(fd);
    if (rc != EOK) {
        (void)fprintf(stderr, "devctl FAULT failed: %s\n", strerror(rc));
        return rc == EINVAL ? 3 : (rc == EPERM ? 4 : 1);
    }
    printf("%s: fault %s param=%u accepted\n", path, lg_sensor_fault_name((uint8_t)mode),
           (unsigned)param);
    return 0;
}

static int do_stats(const char *path) {
    lg_sensor_stats_t st;
    int fd = open(path, O_RDONLY);
    int rc;

    if (fd == -1) {
        (void)fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }
    memset(&st, 0, sizeof st);
    rc = devctl(fd, DCMD_LG_SENSOR_STATS, &st, sizeof st, NULL);
    (void)close(fd);
    if (rc != EOK) {
        (void)fprintf(stderr, "devctl STATS failed: %s\n", strerror(rc));
        return 1;
    }
    printf("stats: ticks=%u samples_sent=%u no_sample=%u send_fail=%u no_controller=%u plant_fail=%u "
           "overruns=%u timeouts=%u connects=%u fault=%s fault_param=%u seq=%u sched_policy=%u "
           "sched_priority=%u worker_policy=%u worker_priority=%u\n",
           (unsigned)st.ticks, (unsigned)st.samples_sent, (unsigned)st.no_sample,
           (unsigned)st.send_fail, (unsigned)st.no_controller, (unsigned)st.plant_fail,
           (unsigned)st.overruns, (unsigned)st.timeouts, (unsigned)st.connects,
           lg_sensor_fault_name((uint8_t)st.fault), (unsigned)st.fault_param,
           (unsigned)st.seq, (unsigned)st.sched_policy, (unsigned)st.sched_priority,
           (unsigned)st.worker_policy, (unsigned)st.worker_priority);
    return 0;
}

int main(int argc, char **argv) {
    char path[64];
    int a = 3;
    int read_only = 0;
    int mode;
    char *end = NULL;
    uint32_t param = 0;

    if (argc < 4 || strcmp(argv[1], "sensor") != 0) {
        return usage();
    }
    if (sensor_path(argv[2], path, sizeof path) != 0) {
        (void)fprintf(stderr, "lgctl: unknown sensor '%s'\n", argv[2]);
        return usage();
    }
    if (strcmp(argv[a], "stats") == 0) {
        return argc == 4 ? do_stats(path) : usage();
    }
    if (strcmp(argv[a], "fault") == 0) {
        a++;
    } else if (strcmp(argv[a], "fault-ro") == 0) {
        a++;
        read_only = 1;
    }
    if (a >= argc) {
        return usage();
    }
    mode = parse_mode(argv[a]);
    if (mode < 0) {
        (void)fprintf(stderr, "lgctl: unknown mode '%s'\n", argv[a]);
        return usage();
    }
    a++;
    {
        int takes_param = mode == LG_SFAULT_NOISE || mode == LG_SFAULT_DROPOUT ||
                          mode == LG_SFAULT_DELAY || mode == LG_SFAULT_DRIFT;

        if (takes_param && a >= argc) {
            (void)fprintf(stderr, "lgctl: mode '%s' needs a parameter\n", argv[a - 1]);
            return usage();
        }
        if (!takes_param && a < argc) {
            (void)fprintf(stderr, "lgctl: mode '%s' takes no parameter\n", argv[a - 1]);
            return usage();
        }
    }
    if (a < argc) {
        unsigned long v = strtoul(argv[a], &end, 10);

        if (*argv[a] == '\0' || *end != '\0' || v > 0xFFFFFFFFul || a + 1 < argc) {
            (void)fprintf(stderr, "lgctl: bad parameter '%s'\n", argv[a]);
            return usage();
        }
        param = (uint32_t)v;
    }
    return do_fault(path, mode, param, read_only);
}
