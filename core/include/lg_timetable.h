#ifndef LG_TIMETABLE_H
#define LG_TIMETABLE_H

/*
 * Level 3 timetable windows (README 3, 8.4, Step 12). Portable and pure: nothing is read from the system.
 *
 * Two halves:
 *
 * 1. lg_tt_t, used by lg-schedule: the timetable (config file) merged with live station departures.
 *    Times are "world" milliseconds since the scenario epoch (the same clock lg-plant runs on); lg-schedule
 *    turns them into CLOCK_MONOTONIC for the controller. Rules (Decisions log, Step 12):
 *     - the timetable alone decides whether there is information for a direction (lg_tt_info): a station
 *       message never makes information valid, so the network cannot choose level 3 over level 4;
 *     - a station departure may only move a window EARLIER or add one, never delay or remove one:
 *       it is matched to the earliest listed entry of its direction whose eta is at or after the reported
 *       eta and at most match_early_ms later (that entry moves to the reported eta, source STATION); if
 *       none, but the reported eta lies inside the window of a listed entry that is already due (a train
 *       running late), it is ignored (that window keeps the gate down until the island confirms);
 *       otherwise it becomes a new STATION entry;
 *     - an entry is listed while world time <= eta + tol_ms.
 *
 * 2. lg_win_dir_t, used by lg-controller: the windows of one direction, by entry id. A window is PENDING
 *    until its start, OPEN from its start (only while the direction has no trustworthy approach sensor),
 *    and CONFIRMED when the island confirmed a train of this direction or a guard confirmed. An OPEN window
 *    keeps the gate down until it is confirmed, also after its end (the train is late: an overdue
 *    notification goes to the guard) and also when the entry is no longer listed by lg-schedule.
 *    Controller times are 32-bit milliseconds, compared wrap-safe (lg_time.h).
 */

#include <stddef.h>
#include <stdint.h>

#include "lg_config.h"
#include "lg_json_mini.h"
#include "lg_proto.h"

/* ---- lg-schedule side ---------------------------------------------------------------------------- */

#define LG_TT_MAX 64
#define LG_TT_ETA_MAX_MS 86400000u        /* a timetable covers at most one day of world time */
#define LG_TT_LEAD_MAX_MS 600000u         /* 10 min */
#define LG_TT_TOL_MAX_MS 1800000u         /* 30 min */
#define LG_TT_MATCH_EARLY_MS 900000u      /* a station eta up to 15 min before an entry still belongs to it */
#define LG_TT_CONF_TIMETABLE 60
#define LG_TT_CONF_STATION 90

typedef struct {
    uint16_t id;     /* timetable ids 1..0x7FFF, station-only entries from LG_SCHED_STATION_ID_BASE */
    uint8_t dir;     /* lg_dir_t */
    uint8_t source;  /* lg_sched_source_t */
    uint32_t eta_ms; /* world ms, possibly moved earlier by a station message */
    uint32_t tt_eta_ms; /* world ms as planned (station-only entries: the reported eta) */
} lg_tt_entry_t;

typedef struct {
    uint32_t lead_ms;
    uint32_t tol_ms;
    uint8_t count;
    uint8_t has_dir[LG_DIR_COUNT]; /* the timetable lists this direction: information exists */
    uint8_t outage[LG_DIR_COUNT];  /* injected loss of information (lgctl schedule outage) */
    uint16_t next_station_id;
    lg_tt_entry_t e[LG_TT_MAX];
    /* Counters for the status text. */
    uint32_t station_rx;
    uint32_t station_moved;
    uint32_t station_added;
    uint32_t station_ignored; /* a delay (late train inside a due window), or the table was full */
    uint32_t station_rejected; /* out of range */
} lg_tt_t;

/* Empty timetable (no information for either direction), default lead and tolerance. */
void lg_tt_init(lg_tt_t *tt);

/*
 * Reads a timetable file:
 *   {"name": "...", "lead_s": 60, "tolerance_s": 180,
 *    "trains": [{"id": 1, "dir": "west", "eta_s": 36}, ...]}
 * name, lead_s and tolerance_s are optional (defaults LG_WINDOW_LEAD_MS / LG_WINDOW_TOLERANCE_MS);
 * eta_s is the planned arrival at the crossing in seconds since the scenario epoch (up to 3 decimals);
 * ids 1..32767 and unique; no other members. 0 on success; -1 with a reason in err (the timetable is
 * then empty: no information).
 */
int lg_tt_from_json(const lg_json_doc_t *doc, lg_tt_t *tt, char *err, size_t errlen);

/* Information for a direction: the timetable lists it and no outage is injected. */
int lg_tt_info(const lg_tt_t *tt, uint8_t dir);

/* dir_mask bit d set = outage for direction d; 0 clears every outage. */
void lg_tt_set_outage(lg_tt_t *tt, uint8_t dir_mask);

/*
 * A station departure: arrival at world time eta_ms, reported at world time now_ms (eta_ms >= now_ms,
 * eta_ms - now_ms <= LG_STATION_ETA_MAX_MS). Returns 1 moved earlier, 2 added, 0 ignored, -1 rejected.
 */
int lg_tt_station(lg_tt_t *tt, uint8_t dir, uint32_t eta_ms, uint32_t now_ms);

/* The listed entries of a direction at world time now_ms, earliest first (ties by id), at most max. */
uint32_t lg_tt_next(const lg_tt_t *tt, uint8_t dir, uint32_t now_ms, lg_tt_entry_t *out, uint32_t max);

/*
 * The schedule reply for a direction at world time now_ms (everything but the header): with information,
 * LG_SCHED_INFO and the listed entries (eta_ns = (epoch_ms + eta) ms in ns, CLOCK_MONOTONIC when epoch_ms is
 * the monotonic time of the scenario start); during an outage or without a timetable for the direction,
 * no flags and no entries (the controller then keeps only its OPEN windows).
 */
void lg_tt_reply(const lg_tt_t *tt, uint8_t dir, uint32_t now_ms, uint64_t epoch_ms, lg_sched_reply_t *r);

/* One text block for /dev/lg/schedule (one line per listed entry plus a summary per direction). */
size_t lg_tt_text(const lg_tt_t *tt, uint32_t now_ms, char *buf, size_t len);

/* ---- lg-controller side -------------------------------------------------------------------------- */

#define LG_WIN_MAX 8

typedef enum { LG_WIN_FREE = 0, LG_WIN_PENDING = 1, LG_WIN_OPEN = 2, LG_WIN_CONFIRMED = 3 } lg_win_state_t;

typedef struct {
    uint8_t state;   /* lg_win_state_t */
    uint8_t source;  /* lg_sched_source_t */
    uint8_t listed;  /* in the latest reply */
    uint8_t overdue; /* OPEN past its end: the guard was notified */
    uint8_t armed;   /* PENDING past its start while the direction was trustworthy: axles_at_open is set */
    uint8_t reserved[3];
    uint16_t id;
    uint16_t axles_at_open; /* island axle count of this direction when the window opened */
    uint32_t start_ms;
    uint32_t eta_ms;
    uint32_t end_ms;
} lg_win_t;

typedef struct {
    lg_win_t w[LG_WIN_MAX];
    uint32_t opened;    /* windows opened */
    uint32_t confirmed_normal; /* a train of the direction passed while detection was trustworthy */
    uint32_t confirmed_island;
    uint32_t confirmed_guard;
    uint32_t overdue;   /* overdue notifications */
    uint32_t dropped;   /* a reply entry found no free slot (every slot OPEN) */
} lg_win_dir_t;

/* One reply entry in controller time. */
typedef struct {
    uint16_t id;
    uint8_t source;
    uint8_t reserved;
    uint32_t eta_ms;
    uint32_t lead_ms;
    uint32_t tol_ms;
} lg_win_entry_t;

void lg_win_init(lg_win_dir_t *wd);

/*
 * A reply for this direction with n entries (n <= LG_SCHED_ENTRIES). Known ids get the new times (an OPEN
 * window stays OPEN; a CONFIRMED one stays CONFIRMED), new ids become PENDING. PENDING windows that are
 * not listed any more are forgotten; OPEN ones are kept until confirmed; CONFIRMED ones until their end.
 */
void lg_win_update(lg_win_dir_t *wd, const lg_win_entry_t *e, uint32_t n);

/*
 * Every controller step. uncertain: the direction has no trustworthy approach sensor; only then PENDING
 * windows whose start has come are opened. While the direction is trustworthy a PENDING window whose start
 * has come is armed (island_axles is remembered); when the island axle count of the direction then moves,
 * the oldest armed window is CONFIRMED (its train passed under normal detection) and is never opened later,
 * when the direction becomes uncertain after a transient loss (found on QNX, Step 12 run 1: a VM stall made
 * East silent for a moment and reopened the window of an East train that had passed long before). An armed
 * window that opens keeps the count from its arming, so a train passing at the moment of the change counts.
 * Returns the number of OPEN windows that became overdue in this call (now past their end), so the caller
 * notifies the guard once.
 */
uint32_t lg_win_tick(lg_win_dir_t *wd, uint8_t uncertain, uint16_t island_axles, uint32_t now_ms);

/* Some window is OPEN (the gate must be down for this direction). */
int lg_win_open(const lg_win_dir_t *wd);

/* The island axle count of the direction moved since the oldest OPEN window opened (a train of this
 * direction passed the island exit). */
int lg_win_axles_moved(const lg_win_dir_t *wd, uint16_t island_axles);

/*
 * The island confirmed one train: the oldest OPEN window is CONFIRMED and the other OPEN windows start
 * counting axles from island_axles. Returns 1 when a window was confirmed.
 */
int lg_win_confirm_island(lg_win_dir_t *wd, uint16_t island_axles);

/* A guard confirmed the direction clear: every OPEN window is CONFIRMED. Returns how many. */
uint32_t lg_win_confirm_guard(lg_win_dir_t *wd);

/* Counts by state, for the status text. */
uint32_t lg_win_count(const lg_win_dir_t *wd, uint8_t state);

#endif
