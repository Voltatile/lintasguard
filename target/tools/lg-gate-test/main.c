/*
 * lg-gate-test: stand-in for the controller and for lgctl until Steps 6 to 8.
 *
 *   lg-gate-test cmd <lower|raise|lights-on|lights-off|bell-on|bell-off> [hold_ms]
 *       Sends one GATE_CMD, prints the state in the reply. With hold_ms it then keeps sending
 *       LG_PULSE_HEARTBEAT every LG_HEARTBEAT_PERIOD_MS for that long, like a live controller.
 *       Kill it (kill -9) during the hold: the heartbeats stop and the gate watchdog fires.
 *   lg-gate-test fault <none|slow|high-current|stuck-up|stuck-down|crash> [factor_pct]
 *   lg-gate-test fault-ro <mode> [factor_pct]   same through a read-only descriptor (must be refused)
 *   lg-gate-test stats
 *   lg-gate-test raw <type>      send a message of the given numeric type (negative test)
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <time.h>
#include <unistd.h>

#include "lg_config.h"
#include "lg_devctl.h"
#include "lg_gate_model.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_target_util.h"

static uint32_t g_seq;

static void fill_hdr(lg_hdr_t *h, uint16_t type) {
    h->type = type;
    h->version = LG_PROTO_VERSION;
    h->seq = ++g_seq;
    h->t_ns = lg_now_ns();
}

static void sleep_ms(uint32_t ms) {
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

static int parse_cmd(const char *s) {
    if (strcmp(s, "lower") == 0) return LG_GATE_CMD_LOWER;
    if (strcmp(s, "raise") == 0) return LG_GATE_CMD_RAISE;
    if (strcmp(s, "lights-on") == 0) return LG_GATE_CMD_LIGHTS_ON;
    if (strcmp(s, "lights-off") == 0) return LG_GATE_CMD_LIGHTS_OFF;
    if (strcmp(s, "bell-on") == 0) return LG_GATE_CMD_BELL_ON;
    if (strcmp(s, "bell-off") == 0) return LG_GATE_CMD_BELL_OFF;
    return -1;
}

static int parse_fault(const char *s) {
    if (strcmp(s, "none") == 0) return LG_GFAULT_NONE;
    if (strcmp(s, "slow") == 0) return LG_GFAULT_SLOW;
    if (strcmp(s, "high-current") == 0) return LG_GFAULT_HIGH_CURRENT;
    if (strcmp(s, "stuck-up") == 0) return LG_GFAULT_STUCK_UP;
    if (strcmp(s, "stuck-down") == 0) return LG_GFAULT_STUCK_DOWN;
    if (strcmp(s, "crash") == 0) return LG_GFAULT_CRASH;
    return -1;
}

static void print_state(const lg_gate_state_msg_t *r) {
    printf("reply: state=%s position=%d current_ma=%u travel_ms=%u\n", lg_gate_state_name(r->state),
           (int)r->position_deg, (unsigned)r->current_ma, (unsigned)r->travel_ms);
    (void)fflush(stdout);
}

static int do_cmd(const char *name, uint32_t hold_ms) {
    lg_gate_cmd_msg_t msg;
    lg_gate_state_msg_t rep;
    int cmd = parse_cmd(name);
    int coid;

    if (cmd < 0) {
        (void)fprintf(stderr, "unknown command '%s'\n", name);
        return 2;
    }
    coid = name_open(LG_NAME_GATE, 0);
    if (coid == -1) {
        (void)fprintf(stderr, "name_open(%s) failed: %s\n", LG_NAME_GATE, strerror(errno));
        return 1;
    }

    memset(&msg, 0, sizeof msg);
    fill_hdr(&msg.hdr, LG_MSG_GATE_CMD);
    msg.cmd = (uint8_t)cmd;
    msg.deadline_ms = 5000;
    memset(&rep, 0, sizeof rep);
    if (MsgSend(coid, &msg, sizeof msg, &rep, sizeof rep) == -1) {
        (void)fprintf(stderr, "MsgSend failed: %s\n", strerror(errno));
        (void)name_close(coid);
        return 1;
    }
    print_state(&rep);

    if (hold_ms > 0) {
        uint32_t waited = 0;

        printf("holding %u ms with heartbeats every %d ms (pid %d)\n", (unsigned)hold_ms,
               LG_HEARTBEAT_PERIOD_MS, (int)getpid());
        (void)fflush(stdout);
        while (waited < hold_ms) {
            if (MsgSendPulse(coid, SIGEV_PULSE_PRIO_INHERIT, LG_PULSE_HEARTBEAT, 0) == -1) {
                (void)fprintf(stderr, "MsgSendPulse failed: %s\n", strerror(errno));
                break;
            }
            sleep_ms(LG_HEARTBEAT_PERIOD_MS);
            waited += LG_HEARTBEAT_PERIOD_MS;
        }
    }
    (void)name_close(coid);
    return 0;
}

/* Exit codes: 0 ok, 1 failure, 2 usage, 3 rejected as invalid (EINVAL), 4 refused (EPERM). */
static int do_fault(const char *name, uint32_t factor_pct, int read_only) {
    lg_gate_fault_req_t req;
    int mode = parse_fault(name);
    int fd;
    int rc;

    if (mode < 0) {
        (void)fprintf(stderr, "unknown fault '%s'\n", name);
        return 2;
    }
    fd = open(LG_DEV_GATE, read_only ? O_RDONLY : O_RDWR);
    if (fd == -1) {
        (void)fprintf(stderr, "open(%s) failed: %s\n", LG_DEV_GATE, strerror(errno));
        return 1;
    }
    req.mode = (uint32_t)mode;
    req.factor_pct = factor_pct;
    rc = devctl(fd, DCMD_LG_GATE_FAULT, &req, sizeof req, NULL);
    if (rc != EOK) {
        (void)fprintf(stderr, "devctl FAULT failed: %s\n", strerror(rc));
        (void)close(fd);
        return rc == EINVAL ? 3 : (rc == EPERM ? 4 : 1);
    }
    printf("fault %s factor_pct=%u accepted\n", name, (unsigned)factor_pct);
    (void)close(fd);
    return 0;
}

static int do_stats(void) {
    lg_gate_stats_t st;
    int fd = open(LG_DEV_GATE, O_RDONLY);
    int rc;

    if (fd == -1) {
        (void)fprintf(stderr, "open(%s) failed: %s\n", LG_DEV_GATE, strerror(errno));
        return 1;
    }
    memset(&st, 0, sizeof st);
    rc = devctl(fd, DCMD_LG_GATE_STATS, &st, sizeof st, NULL);
    if (rc != EOK) {
        (void)fprintf(stderr, "devctl STATS failed: %s\n", strerror(rc));
        (void)close(fd);
        return 1;
    }
    printf("stats: state=%s target=%u position_mdeg=%d current_ma=%u last_travel_ms=%u fault=%s "
           "fault_pct=%u watchdog_trips=%u rejected=%u ticks=%u sched_policy=%u "
           "sched_priority=%u\n",
           lg_gate_state_name((uint8_t)st.state), (unsigned)st.target, (int)st.position_mdeg,
           (unsigned)st.current_ma, (unsigned)st.last_travel_ms,
           lg_gate_fault_name((uint8_t)st.fault), (unsigned)st.fault_pct,
           (unsigned)st.watchdog_trips, (unsigned)st.rejected, (unsigned)st.ticks,
           (unsigned)st.sched_policy, (unsigned)st.sched_priority);
    (void)close(fd);
    return 0;
}

/* Negative test: a message with an unknown type, and a GATE_CMD with a bad command value. */
static int do_raw(unsigned type) {
    lg_gate_cmd_msg_t msg;
    lg_gate_state_msg_t rep;
    int coid = name_open(LG_NAME_GATE, 0);
    long rc;

    if (coid == -1) {
        (void)fprintf(stderr, "name_open(%s) failed: %s\n", LG_NAME_GATE, strerror(errno));
        return 1;
    }
    memset(&msg, 0, sizeof msg);
    fill_hdr(&msg.hdr, (uint16_t)type);
    msg.cmd = 99; /* not a valid command */
    rc = MsgSend(coid, &msg, sizeof msg, &rep, sizeof rep);
    if (rc == -1) {
        printf("raw type 0x%x: rejected, errno=%d (%s)\n", type, errno, strerror(errno));
    } else {
        printf("raw type 0x%x: accepted (unexpected)\n", type);
    }
    (void)name_close(coid);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "cmd") == 0) {
        return do_cmd(argv[2], argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : 0u);
    }
    if (argc >= 3 && strcmp(argv[1], "fault") == 0) {
        return do_fault(argv[2], argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : 100u, 0);
    }
    if (argc >= 3 && strcmp(argv[1], "fault-ro") == 0) {
        return do_fault(argv[2], argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : 100u, 1);
    }
    if (argc >= 2 && strcmp(argv[1], "stats") == 0) {
        return do_stats();
    }
    if (argc >= 3 && strcmp(argv[1], "raw") == 0) {
        return do_raw((unsigned)strtoul(argv[2], NULL, 0));
    }
    (void)fprintf(stderr,
                  "usage: lg-gate-test cmd <lower|raise|lights-on|lights-off|bell-on|bell-off> "
                  "[hold_ms]\n"
                  "       lg-gate-test fault <none|slow|high-current|stuck-up|stuck-down|crash> "
                  "[factor_pct]\n"
                  "       lg-gate-test stats\n"
                  "       lg-gate-test raw <type>\n");
    return 2;
}
