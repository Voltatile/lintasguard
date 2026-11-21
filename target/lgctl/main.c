/*
 * lgctl: command line control of LintasGuard (README 4.2, 10.2).
 *
 *   lgctl kill sensor <dir>/<id> | gate | controller | plant
 *                                      SIGKILL the process the supervisor started (pid from
 *                                      /dev/lg/supervisor): what a crash looks like to the system
 *   lgctl guard-ack --id <guard>       acknowledge level 4 (message to lg/controller). The guard id
 *                                      must be in the "guards" list of the crossing config; the PIN
 *                                      is NOT checked yet (Step 13)
 *   lgctl status                       print /dev/lg/controller and /dev/lg/supervisor
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
 * Fault injection and kill need root: access control proper is Step 13.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>

#include "lg_devctl.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_sensor_model.h"
#include "lg_target_util.h"

static const char *const dir_names[LG_DIR_COUNT] = {LG_DIR_NAME_WEST, LG_DIR_NAME_EAST};
static const char *const id_names[LG_SENSOR_ID_COUNT] = {LG_SENSOR_NAME_A, LG_SENSOR_NAME_B,
                                                         LG_SENSOR_NAME_ISLAND};

static int usage(void) {
    (void)fprintf(stderr,
                  "usage: lgctl sensor <dir>/<id> [fault] <mode> [param]\n"
                  "       lgctl sensor <dir>/<id> stats\n"
                  "       lgctl kill sensor <dir>/<id> | gate | controller | plant\n"
                  "       lgctl guard-ack --id <guard>\n"
                  "       lgctl status\n"
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

/* ---- kill, guard-ack, status ---------------------------------------------------------------- */

/* Reads a text device into buf (NUL terminated). Returns the length, -1 on failure. */
static int read_text(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    size_t used = 0;

    if (fd == -1) {
        (void)fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
        return -1;
    }
    for (;;) {
        ssize_t n = read(fd, buf + used, cap - 1 - used);

        if (n < 0) {
            (void)fprintf(stderr, "read(%s) failed: %s\n", path, strerror(errno));
            (void)close(fd);
            return -1;
        }
        used += (size_t)n;
        if (n == 0 || used >= cap - 1) {
            break;
        }
    }
    (void)close(fd);
    buf[used] = '\0';
    return (int)used;
}

/* The pid of the process called `name` in the supervisor text, 0 when it has none, -1 when unknown. */
static long pid_in_table(const char *text, const char *name) {
    char key[64];
    const char *line = text;

    (void)snprintf(key, sizeof key, "proc=%s ", name);
    while (*line != '\0') {
        const char *eol = strchr(line, '\n');
        const char *p;

        if (strncmp(line, key, strlen(key)) == 0) {
            p = strstr(line, " pid=");
            if (p == NULL || (eol != NULL && p > eol)) {
                return -1;
            }
            return strtol(p + 5, NULL, 10);
        }
        if (eol == NULL) {
            break;
        }
        line = eol + 1;
    }
    return -1;
}

static int cmd_kill(int argc, char **argv) {
    char name[48];
    char text[4096];
    long pid;

    if (argc == 4 && strcmp(argv[2], "sensor") == 0) {
        char path[64];

        if (sensor_path(argv[3], path, sizeof path) != 0) {
            (void)fprintf(stderr, "lgctl: unknown sensor '%s'\n", argv[3]);
            return usage();
        }
        (void)snprintf(name, sizeof name, "sensor/%s", argv[3]);
    } else if (argc == 3 && (strcmp(argv[2], "gate") == 0 || strcmp(argv[2], "controller") == 0 ||
                             strcmp(argv[2], "plant") == 0)) {
        (void)snprintf(name, sizeof name, "%s", argv[2]);
    } else {
        return usage();
    }
    if (read_text(LG_DEV_SUPERVISOR, text, sizeof text) < 0) {
        (void)fprintf(stderr, "lgctl: no supervisor running? (%s)\n", LG_DEV_SUPERVISOR);
        return 1;
    }
    pid = pid_in_table(text, name);
    if (pid < 0) {
        (void)fprintf(stderr, "lgctl: '%s' is not in the supervisor table\n", name);
        return 1;
    }
    if (pid <= 1) {
        (void)fprintf(stderr, "lgctl: '%s' has no running process (pid %ld)\n", name, pid);
        return 1; /* never kill(0, ...) or kill(1, ...): that is not a process of ours */
    }
    if (kill((pid_t)pid, SIGKILL) == -1) {
        (void)fprintf(stderr, "kill(%ld) failed: %s\n", pid, strerror(errno));
        return errno == EPERM ? 4 : 1;
    }
    printf("killed %s pid %ld (SIGKILL)\n", name, pid);
    return 0;
}

static int cmd_guard_ack(int argc, char **argv) {
    lg_guard_ack_t msg;
    lg_ack_t ack;
    uint64_t timeout_ns = 2000000000ull;
    int coid;
    long rc;

    if (argc != 4 || strcmp(argv[2], "--id") != 0) {
        return usage();
    }
    if (strlen(argv[3]) < 1 || strlen(argv[3]) >= LG_GUARD_ID_LEN) {
        (void)fprintf(stderr, "lgctl: the guard id has 1 to %d characters\n", LG_GUARD_ID_LEN - 1);
        return 3;
    }
    memset(&msg, 0, sizeof msg);
    msg.hdr.type = LG_MSG_GUARD_ACK;
    msg.hdr.version = LG_PROTO_VERSION;
    msg.hdr.seq = 1;
    msg.hdr.t_ns = lg_now_ns();
    (void)strcpy(msg.guard_id, argv[3]);
    msg.action = LG_GUARD_ACTION_ACK; /* the PIN hash stays zero: not checked yet (Step 13) */
    coid = name_open(LG_NAME_CONTROLLER, 0);
    if (coid == -1) {
        (void)fprintf(stderr, "name_open(%s) failed: %s\n", LG_NAME_CONTROLLER, strerror(errno));
        return 1;
    }
    memset(&ack, 0, sizeof ack);
    (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout_ns, NULL);
    rc = MsgSend(coid, &msg, sizeof msg, &ack, sizeof ack);
    if (rc == -1) {
        (void)fprintf(stderr, "MsgSend failed: %s\n", strerror(errno));
        (void)name_close(coid);
        return 1;
    }
    (void)name_close(coid);
    if (ack.hdr.type != LG_MSG_ACK || ack.hdr.version != LG_PROTO_VERSION) {
        (void)fprintf(stderr, "lgctl: the controller did not answer with an ack\n");
        return 1;
    }
    if (ack.status == EPERM) {
        (void)fprintf(stderr, "guard ack for '%s' refused: not in the guard list\n", argv[3]);
        return 4;
    }
    if (ack.status != 0) {
        (void)fprintf(stderr, "guard ack rejected: %s\n", strerror(ack.status));
        return ack.status == EINVAL ? 3 : 1;
    }
    printf("guard ack for '%s' sent (level 4 is left only if all inputs are healthy and stable)\n", argv[3]);
    return 0;
}

static int cmd_status(int argc) {
    char text[4096];
    int bad = 0;

    if (argc != 2) {
        return usage();
    }
    printf("== controller (/dev/lg/controller)\n");
    if (read_text("/dev/lg/controller", text, sizeof text) < 0) {
        bad = 1;
    } else {
        fputs(text, stdout);
    }
    printf("== supervisor (%s)\n", LG_DEV_SUPERVISOR);
    if (read_text(LG_DEV_SUPERVISOR, text, sizeof text) < 0) {
        bad = 1;
    } else {
        fputs(text, stdout);
    }
    return bad;
}

int main(int argc, char **argv) {
    char path[64];
    int a = 3;
    int read_only = 0;
    int mode;
    char *end = NULL;
    uint32_t param = 0;

    if (argc >= 2 && strcmp(argv[1], "kill") == 0) {
        return cmd_kill(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "guard-ack") == 0) {
        return cmd_guard_ack(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        return cmd_status(argc);
    }
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
