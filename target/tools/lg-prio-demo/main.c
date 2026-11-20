/*
 * lg-prio-demo: shows what priority a QNX server thread runs at while it serves a client.
 *
 *   lg-prio-demo server <fixed 0|1>   server at SCHED_FIFO LG_PRIO_GATE; fixed=1 creates the
 *                                     channel with _NTO_CHF_FIXED_PRIORITY. Serves 3 messages.
 *   lg-prio-demo client <pid> <chid>  default priority client, sends 3 messages.
 *
 * Why it exists: lg-gate must keep its own priority whoever sends it a message. This tool is the
 * evidence for the choice of _NTO_CHF_FIXED_PRIORITY (see docs/ARCHITECTURE.md Decisions log).
 */

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <unistd.h>

#include "lg_names.h"

#define MSG_COUNT 3

static int current_prio(void) {
    struct sched_param sp;
    int policy = 0;

    memset(&sp, 0, sizeof sp);
    (void)pthread_getschedparam(pthread_self(), &policy, &sp);
    return sp.sched_priority;
}

static int run_server(int fixed) {
    struct sched_param sp;
    char buf[32];
    int chid;
    int i;

    memset(&sp, 0, sizeof sp);
    sp.sched_priority = LG_PRIO_GATE;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        (void)fprintf(stderr, "pthread_setschedparam failed\n");
        return 1;
    }
    chid = ChannelCreate(fixed ? _NTO_CHF_FIXED_PRIORITY : 0u);
    if (chid == -1) {
        (void)fprintf(stderr, "ChannelCreate failed\n");
        return 1;
    }
    printf("server pid=%d chid=%d fixed_priority=%d base_prio=%d\n", (int)getpid(), chid, fixed,
           current_prio());
    (void)fflush(stdout);

    for (i = 0; i < MSG_COUNT; i++) {
        int rcvid = MsgReceive(chid, buf, sizeof buf, NULL);
        int serving = current_prio();

        (void)MsgReply(rcvid, 0, "ok", 3);
        printf("msg %d: prio while serving=%d, right after reply=%d\n", i, serving, current_prio());
        (void)fflush(stdout);
    }
    printf("prio before the next receive=%d\n", current_prio());
    return 0;
}

static int run_client(pid_t pid, int chid) {
    char buf[32];
    int coid = ConnectAttach(0, pid, chid, _NTO_SIDE_CHANNEL, 0);
    int i;

    if (coid == -1) {
        (void)fprintf(stderr, "ConnectAttach failed\n");
        return 1;
    }
    printf("client prio=%d\n", current_prio());
    for (i = 0; i < MSG_COUNT; i++) {
        (void)MsgSend(coid, "hi", 3, buf, sizeof buf);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "server") == 0) {
        return run_server(atoi(argv[2]) != 0);
    }
    if (argc >= 4 && strcmp(argv[1], "client") == 0) {
        return run_client((pid_t)atoi(argv[2]), atoi(argv[3]));
    }
    (void)fprintf(stderr, "usage: lg-prio-demo server <0|1> | client <pid> <chid>\n");
    return 2;
}
