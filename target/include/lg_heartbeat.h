#ifndef LG_HEARTBEAT_H
#define LG_HEARTBEAT_H

/*
 * Heartbeat towards lg-supervisor (README 8.5). Header-only helper, one copy per process.
 *
 *   lg_hb_start(LG_PROC_xxx)   once, after the process is set up: starts the connector thread
 *   lg_hb_beat()               from the thread that PROVES the process is alive (the one doing the
 *                              periodic work, not the one that merely answers a device); cheap,
 *                              never blocks, sends at most one pulse per LG_HB_MIN_GAP_MS
 *
 * The beat is a pulse LG_PULSE_HEARTBEAT to the name lg/supervisor; its value is the process id, the
 * supervisor takes the pid from the connection (see lg_proto.h). name_open and name_close wait for a stuck server, so only the connector
 * thread (low priority) calls them, the liveness thread only calls MsgSendPulse, which does not
 * block. Without a supervisor nothing is sent and nothing waits: a process started by hand works
 * as before. Shared state is touched with atomics only, so the liveness thread never takes a lock.
 * Not authenticated: any process can send a pulse like this (Step 13).
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <time.h>
#include <unistd.h>

#include "lg_config.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_target_util.h"

#define LG_HB_MIN_GAP_MS (LG_HEARTBEAT_PERIOD_MS / 2)
#define LG_HB_CONNECT_PERIOD_MS 200

static int g_hb_coid = -1;   /* connection to lg/supervisor, -1 = none (atomics) */
static int g_hb_stale = -1;  /* a connection that failed, for the connector to close (atomics) */
static uint32_t g_hb_value;  /* the process id sent as the pulse value */
static int g_hb_started;     /* lg_hb_start ran */
static uint64_t g_hb_last_ns;
static uint32_t g_hb_sent;
static uint32_t g_hb_errors;
static uint32_t g_hb_connects;

/* The only place where name_open and name_close are called. */
static void *lg_hb_connector(void *arg) {
    struct timespec pause;

    (void)arg;
    pause.tv_sec = 0;
    pause.tv_nsec = (long)LG_HB_CONNECT_PERIOD_MS * 1000000L;
    for (;;) {
        int stale = __atomic_exchange_n(&g_hb_stale, -1, __ATOMIC_ACQ_REL);

        if (stale >= 0) {
            (void)name_close(stale);
        }
        if (__atomic_load_n(&g_hb_coid, __ATOMIC_ACQUIRE) < 0) {
            int c = name_open(LG_NAME_SUPERVISOR, 0);

            if (c >= 0) {
                __atomic_add_fetch(&g_hb_connects, 1, __ATOMIC_RELAXED);
                __atomic_store_n(&g_hb_coid, c, __ATOMIC_RELEASE);
            }
        }
        (void)nanosleep(&pause, NULL);
    }
    return NULL;
}

/* Returns 0 on success, -1 when the thread could not be created (the process runs without beats). */
static inline int lg_hb_start(uint8_t proc_id) {
    pthread_attr_t attr;
    struct sched_param sp;
    pthread_t th;
    int rc;

    g_hb_value = proc_id;
    g_hb_started = 1;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = LG_PRIO_CONNECTOR;
    (void)pthread_attr_init(&attr);
    (void)pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    (void)pthread_attr_setschedpolicy(&attr, SCHED_RR);
    (void)pthread_attr_setschedparam(&attr, &sp);
    (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&th, &attr, lg_hb_connector, NULL);
    (void)pthread_attr_destroy(&attr);
    return rc == 0 ? 0 : -1;
}

/* Call from the liveness thread only (g_hb_last_ns is not shared). */
static inline void lg_hb_beat(void) {
    uint64_t now = lg_now_ns();
    int coid;

    if (!g_hb_started || now - g_hb_last_ns < (uint64_t)LG_HB_MIN_GAP_MS * 1000000ull) {
        return;
    }
    coid = __atomic_load_n(&g_hb_coid, __ATOMIC_ACQUIRE);
    if (coid < 0) {
        return;
    }
    g_hb_last_ns = now;
    if (MsgSendPulse(coid, SIGEV_PULSE_PRIO_INHERIT, LG_PULSE_HEARTBEAT, (int)g_hb_value) == -1) {
        if (errno != EAGAIN) { /* EAGAIN: no pulse available right now, the connection is fine */
            int expected = coid;

            if (__atomic_compare_exchange_n(&g_hb_coid, &expected, -1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&g_hb_stale, coid, __ATOMIC_RELEASE);
            }
        }
        g_hb_errors++;
        return;
    }
    g_hb_sent++;
}

#endif
