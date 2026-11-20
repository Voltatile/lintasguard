/*
 * lg-chan-demo: two measurements about QNX channels and pulses that the sensor design relies on.
 *
 *   lg-chan-demo pulse-prio          at what priority does a thread run after it received a timer
 *                                    pulse, for SIGEV_PULSE_PRIO_INHERIT and for an explicit priority,
 *                                    on a default and on a fixed-priority channel
 *   lg-chan-demo private-server <0|1>  creates a channel (1 = _NTO_CHF_PRIVATE), attaches to it itself,
 *                                    and reports what it receives (3 receives, then exits)
 *   lg-chan-demo private-client <pid> <chid>
 *                                    tries to attach to that channel from another process and to send
 *                                    a pulse
 *
 * The results are in evidence/step06_channels.txt (tests/target/step06_channels.sh).
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <time.h>
#include <unistd.h>

#include "lg_names.h"

static int prio_now(void) {
    struct sched_param sp;
    int policy = 0;

    memset(&sp, 0, sizeof sp);
    (void)pthread_getschedparam(pthread_self(), &policy, &sp);
    return sp.sched_priority;
}

static int pulse_case(const char *label, int pulse_prio, unsigned chflags) {
    struct sigevent ev;
    struct itimerspec its;
    timer_t timer;
    struct _pulse pulse;
    int chid = ChannelCreate(chflags);
    int coid = chid == -1 ? -1 : ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    int after = -1;
    int i;

    if (coid == -1) {
        (void)fprintf(stderr, "channel setup failed: %s\n", strerror(errno));
        return -1;
    }
    SIGEV_PULSE_INIT(&ev, coid, pulse_prio, 1, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        (void)fprintf(stderr, "timer_create failed: %s\n", strerror(errno));
        return -1;
    }
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = 10000000L;
    its.it_interval.tv_nsec = 10000000L;
    (void)timer_settime(timer, 0, &its, NULL);
    for (i = 0; i < 5; i++) {
        (void)MsgReceive(chid, &pulse, sizeof pulse, NULL);
        after = prio_now();
    }
    (void)timer_delete(timer);
    printf("pulse: %-46s priority after the pulse: %d\n", label, after);
    return after;
}

static int do_pulse_prio(void) {
    struct sched_param sp;

    printf("pulse: the process started at priority %d\n", prio_now());
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = LG_PRIO_SENSOR;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        (void)fprintf(stderr, "could not set the priority\n");
        return 1;
    }
    printf("pulse: the thread set itself to FIFO %d\n", prio_now());
    (void)pulse_case("SIGEV_PULSE_PRIO_INHERIT, default channel", SIGEV_PULSE_PRIO_INHERIT, 0);
    (void)pulse_case("explicit priority, default channel", LG_PRIO_SENSOR, 0);
    (void)pulse_case("SIGEV_PULSE_PRIO_INHERIT, fixed-priority channel", SIGEV_PULSE_PRIO_INHERIT,
                     _NTO_CHF_FIXED_PRIORITY);
    (void)pulse_case("explicit priority, fixed-priority channel", LG_PRIO_SENSOR, _NTO_CHF_FIXED_PRIORITY);
    return 0;
}

static int do_private_server(int priv) {
    unsigned flags = priv ? _NTO_CHF_PRIVATE : 0u;
    int chid = ChannelCreate(flags);
    int self = chid == -1 ? -1 : ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct _pulse pulse;
    int i;

    if (chid == -1) {
        return 1;
    }
    printf("private: server pid=%d chid=%d private=%d, own attach: %s\n", (int)getpid(), chid, priv,
           self == -1 ? strerror(errno) : "ok");
    (void)fflush(stdout);
    if (self != -1) {
        (void)MsgSendPulse(self, 10, 1, 0);
    }
    for (i = 0; i < 2; i++) {
        uint64_t wait_ns = 2000000000ull;
        int rcvid;

        (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &wait_ns, NULL);
        rcvid = MsgReceive(chid, &pulse, sizeof pulse, NULL);
        if (rcvid == -1) {
            printf("private: server received nothing more\n");
            break;
        }
        printf("private: server received a pulse with code %d\n", pulse.code);
        (void)fflush(stdout);
    }
    return 0;
}

static int do_private_client(pid_t pid, int chid) {
    int coid = ConnectAttach(0, pid, chid, _NTO_SIDE_CHANNEL, 0);

    printf("private: another process attaches: %s\n", coid == -1 ? strerror(errno) : "ok");
    if (coid != -1) {
        printf("private: another process sends a pulse: %s\n",
               MsgSendPulse(coid, 10, 2, 0) == 0 ? "sent" : strerror(errno));
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "pulse-prio") == 0) {
        return do_pulse_prio();
    }
    if (argc >= 3 && strcmp(argv[1], "private-server") == 0) {
        return do_private_server(atoi(argv[2]) != 0);
    }
    if (argc >= 4 && strcmp(argv[1], "private-client") == 0) {
        return do_private_client((pid_t)atoi(argv[2]), atoi(argv[3]));
    }
    (void)fprintf(stderr, "usage: lg-chan-demo pulse-prio | private-server <0|1> | private-client <pid> <chid>\n");
    return 2;
}
