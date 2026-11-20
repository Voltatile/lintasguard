#ifndef LG_PLANT_MODEL_H
#define LG_PLANT_MODEL_H

/*
 * The simulated world: trains on the track and what each sensor would physically see.
 * Portable and pure (README 4.2): a function of the scenario and the time, integers only.
 * The controller never sees this; only the sensors ask for it (as "ground truth").
 *
 * Geometry (mm). Every train has a path coordinate u that starts at the strike-in point of the
 * side it comes from. The island entry is at u = D (strike_in), the island exit at u = D + I.
 * The front of a train is at u_front(t) = speed * (t - t_strike), its rear at u_front - length.
 * The train covers [u_rear, u_front]. Its axles sit at u_front - (k + 0.5) * length / axles.
 * The model starts a train at the strike-in point: nothing before u = 0 matters to any sensor.
 *
 * What a sensor sees:
 *  - approach sensor A or B of side S (zone [0, Z] on the path of the trains coming from S):
 *    occupied while a train FROM S covers the zone; it counts the axles of trains from S that have
 *    passed u = 0. Trains from the other side never trigger it, also not when they leave over it
 *    (direction aware: otherwise a departing train would close the gate again).
 *  - island sensor of side S: occupied while ANY train covers [D, D + I]; its axle count is the
 *    number of axles of trains from S that have passed the island exit (u = D + I).
 */

#include <stddef.h>
#include <stdint.h>

#include "lg_proto.h"
#include "lg_scenario.h"

typedef struct {
    lg_crossing_cfg_t cfg;
    lg_scenario_t sc;
} lg_plant_t;

typedef struct {
    uint8_t occupied;   /* the sensor sees a train */
    uint8_t island_any; /* any train is on the island (not sensor specific) */
    uint16_t axles;     /* cumulative axle count of this sensor (saturates at 65535) */
} lg_truth_t;

void lg_plant_init(lg_plant_t *p, const lg_crossing_cfg_t *cfg, const lg_scenario_t *sc);

/* Ground truth for one sensor (dir, id) at scenario time t_ms. Unknown dir or id: all zero. */
lg_truth_t lg_plant_truth(const lg_plant_t *p, uint32_t t_ms, uint8_t dir, uint8_t id);

typedef enum {
    LG_PLANT_EV_STRIKE_IN = 1,   /* the front reaches the strike-in point */
    LG_PLANT_EV_ISLAND_ENTER = 2, /* the front reaches the island */
    LG_PLANT_EV_ISLAND_EXIT = 3,  /* the front reaches the island exit */
    LG_PLANT_EV_CLEARED = 4       /* the rear passes the island exit: the island is free */
} lg_plant_event_kind_t;

typedef struct {
    uint32_t t_ms; /* when it happens */
    uint16_t train_id;
    uint8_t kind; /* lg_plant_event_kind_t */
    uint8_t dir;
} lg_plant_event_t;

/*
 * Events with t_prev_ms < t <= t_now_ms, sorted by time (ties: train id, then kind). Returns how
 * many were stored (at most max). Used for the log of the plant process. Pass t_prev_ms = -1 for
 * the first window so that an event at exactly t = 0 is not missed.
 *
 * The event time is the exact crossing time rounded down to a millisecond, while the truth turns
 * on the first millisecond after the crossing (strict comparison): an event can be reported up
 * to 2 ms before the matching change in lg_plant_truth. It is meant for the log only.
 */
size_t lg_plant_events(const lg_plant_t *p, int64_t t_prev_ms, int64_t t_now_ms,
                       lg_plant_event_t *out, size_t max);

typedef struct {
    uint32_t waiting;  /* trains that have not reached the strike-in point */
    uint32_t active;   /* between the strike-in point and the island exit (front passed, rear not) */
    uint32_t finished; /* rear is past the island exit */
    uint8_t island_any;
} lg_plant_counts_t;

lg_plant_counts_t lg_plant_counts(const lg_plant_t *p, uint32_t t_ms);

/* One line of text for /dev/lg/plant, newline and NUL terminated. Returns the length. */
size_t lg_plant_text(const lg_plant_t *p, uint32_t t_ms, char *buf, size_t len);

#endif
