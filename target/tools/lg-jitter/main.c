/*
 * lg-jitter: baseline of the timer on this machine, with nothing else of LintasGuard involved.
 *
 *   lg-jitter [seconds [priority]]      default 10 s at SCHED_FIFO 45
 *
 * A periodic timer pulse every LG_SENSOR_PERIOD_MS is received on a private channel, exactly as
 * the sensor worker does it, and the gaps between pulses are counted. The result is printed in one
 * line. It shows how much of the late ticks that lgctl reports ("overruns") is the jitter of the
 * virtual machine and not the work of the sensor. Not a performance comparison of any kind.
 */

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <time.h>

#include "lg_config.h"
#include "lg_proto.h"
#include "lg_target_util.h"

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    int prio = argc > 2 ? atoi(argv[2]) : 45;
    struct sched_param sp;
    struct sigevent ev;
    struct itimerspec its;
    timer_t timer;
    uint64_t late_limit_ns = (uint64_t)(LG_SENSOR_PERIOD_MS + LG_TICK_TOLERANCE_MS) * 1000000ull;
    uint64_t prev = 0;
    uint64_t largest = 0;
    int ticks;
    int total;
    int late = 0;
    int late20 = 0;
    int chid;
    int coid;
    int i;

    if (seconds <= 0 || prio <= 0 || prio > 254) {
        (void)fprintf(stderr, "usage: lg-jitter [seconds [priority]]\n");
        return 2;
    }
    ticks = seconds * (1000 / LG_SENSOR_PERIOD_MS);
    total = ticks;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        (void)fprintf(stderr, "lg-jitter: could not set priority %d\n", prio);
        return 1;
    }
    chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_PRIVATE);
    coid = chid == -1 ? -1 : ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        (void)fprintf(stderr, "lg-jitter: channel setup failed\n");
        return 1;
    }
    /* Explicit priority: SIGEV_PULSE_PRIO_INHERIT runs the receiver at the priority the process was
     * created with (10 from a shell), not at the priority set above. */
    SIGEV_PULSE_INIT(&ev, coid, prio, LG_PULSE_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        (void)fprintf(stderr, "lg-jitter: timer_create failed\n");
        return 1;
    }
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = (long)LG_SENSOR_PERIOD_MS * 1000000L;
    its.it_interval.tv_nsec = (long)LG_SENSOR_PERIOD_MS * 1000000L;
    if (timer_settime(timer, 0, &its, NULL) == -1) {
        (void)fprintf(stderr, "lg-jitter: timer_settime failed\n");
        return 1;
    }
    for (i = 0; i < ticks; i++) {
        struct _pulse p;
        uint64_t t;

        if (MsgReceive(chid, &p, sizeof p, NULL) == -1) {
            continue;
        }
        t = lg_now_ns();
        if (prev != 0) {
            uint64_t gap = t - prev;

            if (gap > largest) {
                largest = gap;
            }
            if (gap > late_limit_ns) {
                late++;
            }
            if (gap > 20000000ull) {
                late20++;
            }
        }
        prev = t;
    }
    printf("jitter: priority %d, %d ticks: late=%d late_percent=%d over_20ms=%d largest_gap_us=%llu\n", prio,
           total, late, late * 100 / total, late20, (unsigned long long)(largest / 1000ull));
    return 0;
}
