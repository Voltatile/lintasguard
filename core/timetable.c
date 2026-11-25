#include "lg_timetable.h"

#include <stdio.h>
#include <string.h>

#include "lg_time.h"

/* ---- lg-schedule side ---------------------------------------------------------------------------- */

void lg_tt_init(lg_tt_t *tt) {
    memset(tt, 0, sizeof *tt);
    tt->lead_ms = LG_WINDOW_LEAD_MS;
    tt->tol_ms = LG_WINDOW_TOLERANCE_MS;
    tt->next_station_id = (uint16_t)LG_SCHED_STATION_ID_BASE;
}

static int fail(char *err, size_t errlen, const char *msg) {
    if (err != NULL && errlen > 0) {
        (void)snprintf(err, errlen, "%s", msg);
    }
    return -1;
}

static const lg_json_node_t *child_of(const lg_json_doc_t *doc, const lg_json_node_t *n) {
    return (n->first_child >= 0 && (uint32_t)n->first_child < doc->n_nodes) ? &doc->nodes[n->first_child] : NULL;
}

static const lg_json_node_t *next_of(const lg_json_doc_t *doc, const lg_json_node_t *n) {
    return (n->next >= 0 && (uint32_t)n->next < doc->n_nodes) ? &doc->nodes[n->next] : NULL;
}

/* Every member key of obj is one of the allowed ones. */
static int only_keys(const lg_json_doc_t *doc, const lg_json_node_t *obj, const char *const *allowed, int n) {
    const lg_json_node_t *ch;

    for (ch = child_of(doc, obj); ch != NULL; ch = next_of(doc, ch)) {
        int ok = 0;
        int i;

        if (ch->key < 0) {
            return 0;
        }
        for (i = 0; i < n; i++) {
            if (strcmp(doc->strs + ch->key, allowed[i]) == 0) {
                ok = 1;
                break;
            }
        }
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

/* An optional number of seconds (milli units = ms) within [min_ms, max_ms]. */
static int opt_ms(const lg_json_doc_t *doc, const lg_json_node_t *obj, const char *key, uint32_t min_ms,
                  uint32_t max_ms, uint32_t *out) {
    const lg_json_node_t *n = lg_json_get(doc, obj, key);

    if (n == NULL) {
        return 0;
    }
    if (n->type != LG_JSON_NUMBER || n->milli < (int64_t)min_ms || n->milli > (int64_t)max_ms) {
        return -1;
    }
    *out = (uint32_t)n->milli;
    return 0;
}

static int dir_from_name(const char *s, uint8_t *dir) {
    if (strcmp(s, "west") == 0) {
        *dir = LG_DIR_WEST;
        return 0;
    }
    if (strcmp(s, "east") == 0) {
        *dir = LG_DIR_EAST;
        return 0;
    }
    return -1;
}

static int parse(const lg_json_doc_t *doc, lg_tt_t *tt, char *err, size_t errlen) {
    static const char *const top_keys[] = {"name", "lead_s", "tolerance_s", "trains"};
    static const char *const train_keys[] = {"id", "dir", "eta_s"};
    const lg_json_node_t *root = lg_json_root(doc);
    const lg_json_node_t *trains;
    const lg_json_node_t *name;
    uint32_t i;

    if (root == NULL || root->type != LG_JSON_OBJECT) {
        return fail(err, errlen, "timetable: the top level must be an object");
    }
    if (!only_keys(doc, root, top_keys, 4)) {
        return fail(err, errlen, "timetable: unknown member (allowed: name, lead_s, tolerance_s, trains)");
    }
    name = lg_json_get(doc, root, "name");
    if (name != NULL && name->type != LG_JSON_STRING) {
        return fail(err, errlen, "timetable: name must be a string");
    }
    if (opt_ms(doc, root, "lead_s", 1000u, LG_TT_LEAD_MAX_MS, &tt->lead_ms) != 0) {
        return fail(err, errlen, "timetable: lead_s must be a number from 1 to 600");
    }
    if (opt_ms(doc, root, "tolerance_s", 1000u, LG_TT_TOL_MAX_MS, &tt->tol_ms) != 0) {
        return fail(err, errlen, "timetable: tolerance_s must be a number from 1 to 1800");
    }
    trains = lg_json_get(doc, root, "trains");
    if (trains == NULL || trains->type != LG_JSON_ARRAY) {
        return fail(err, errlen, "timetable: trains must be an array");
    }
    if (trains->count > LG_TT_MAX) {
        return fail(err, errlen, "timetable: too many trains (at most 64)");
    }
    for (i = 0; i < trains->count; i++) {
        const lg_json_node_t *t = lg_json_at(doc, trains, i);
        const char *dname;
        int64_t id;
        int64_t eta;
        uint8_t dir;
        uint8_t k;

        if (t == NULL || t->type != LG_JSON_OBJECT || !only_keys(doc, t, train_keys, 3)) {
            return fail(err, errlen, "timetable: every train must be an object with id, dir and eta_s only");
        }
        if (lg_json_get_milli(doc, t, "id", &id) != 0 || id % 1000 != 0 || id < 1000 ||
            id > (int64_t)(LG_SCHED_STATION_ID_BASE - 1u) * 1000) {
            return fail(err, errlen, "timetable: id must be an integer from 1 to 32767");
        }
        if (lg_json_get_str(doc, t, "dir", &dname) != 0 || dir_from_name(dname, &dir) != 0) {
            return fail(err, errlen, "timetable: dir must be \"west\" or \"east\"");
        }
        if (lg_json_get_milli(doc, t, "eta_s", &eta) != 0 || eta < 0 || eta > (int64_t)LG_TT_ETA_MAX_MS) {
            return fail(err, errlen, "timetable: eta_s must be a number from 0 to 86400");
        }
        for (k = 0; k < tt->count; k++) {
            if (tt->e[k].id == (uint16_t)(id / 1000)) {
                return fail(err, errlen, "timetable: duplicate id");
            }
        }
        tt->e[tt->count].id = (uint16_t)(id / 1000);
        tt->e[tt->count].dir = dir;
        tt->e[tt->count].source = LG_SRC_TIMETABLE;
        tt->e[tt->count].eta_ms = (uint32_t)eta;
        tt->e[tt->count].tt_eta_ms = (uint32_t)eta;
        tt->has_dir[dir] = 1;
        tt->count++;
    }
    return 0;
}

int lg_tt_from_json(const lg_json_doc_t *doc, lg_tt_t *tt, char *err, size_t errlen) {
    int rc;

    lg_tt_init(tt);
    rc = parse(doc, tt, err, errlen);
    if (rc != 0) {
        lg_tt_init(tt); /* a refused file leaves no half-filled timetable: no information at all */
    }
    return rc;
}

int lg_tt_info(const lg_tt_t *tt, uint8_t dir) {
    return dir < LG_DIR_COUNT && tt->has_dir[dir] && !tt->outage[dir];
}

void lg_tt_set_outage(lg_tt_t *tt, uint8_t dir_mask) {
    uint8_t d;

    for (d = 0; d < LG_DIR_COUNT; d++) {
        tt->outage[d] = (uint8_t)((dir_mask >> d) & 1u);
    }
}

static int listed(const lg_tt_t *tt, const lg_tt_entry_t *e, uint32_t now_ms) {
    return (uint64_t)now_ms <= (uint64_t)e->eta_ms + tt->tol_ms;
}

static int id_in_use(const lg_tt_t *tt, uint16_t id) {
    uint8_t k;

    for (k = 0; k < tt->count; k++) {
        if (tt->e[k].id == id) {
            return 1;
        }
    }
    return 0;
}

static uint16_t new_station_id(lg_tt_t *tt) {
    uint32_t tries;

    for (tries = 0; tries < 0x8000u; tries++) {
        uint16_t id = tt->next_station_id;

        tt->next_station_id = (uint16_t)(id == 0xFFFFu ? LG_SCHED_STATION_ID_BASE : id + 1u);
        if (!id_in_use(tt, id)) {
            return id;
        }
    }
    return 0; /* cannot happen with LG_TT_MAX entries */
}

int lg_tt_station(lg_tt_t *tt, uint8_t dir, uint32_t eta_ms, uint32_t now_ms) {
    lg_tt_entry_t *cand = NULL;
    int late_covered = 0;
    int slot = -1;
    uint8_t k;

    tt->station_rx++;
    if (dir >= LG_DIR_COUNT || eta_ms < now_ms || eta_ms - now_ms > LG_STATION_ETA_MAX_MS) {
        tt->station_rejected++;
        return -1;
    }
    for (k = 0; k < tt->count; k++) {
        lg_tt_entry_t *e = &tt->e[k];

        if (e->dir != dir || !listed(tt, e, now_ms)) {
            continue;
        }
        if (e->eta_ms >= eta_ms && e->eta_ms - eta_ms <= LG_TT_MATCH_EARLY_MS) {
            if (cand == NULL || e->eta_ms < cand->eta_ms || (e->eta_ms == cand->eta_ms && e->id < cand->id)) {
                cand = e;
            }
        } else if (e->eta_ms < eta_ms && (uint64_t)eta_ms <= (uint64_t)e->eta_ms + tt->tol_ms) {
            late_covered = 1;
        }
    }
    if (cand != NULL) {
        cand->eta_ms = eta_ms; /* earlier or equal: never later */
        cand->source = LG_SRC_STATION;
        tt->station_moved++;
        return 1;
    }
    if (late_covered) {
        tt->station_ignored++; /* that window already keeps the gate down until the island confirms */
        return 0;
    }
    if (tt->count < LG_TT_MAX) {
        slot = tt->count++;
    } else {
        for (k = 0; k < tt->count; k++) {
            if (!listed(tt, &tt->e[k], now_ms)) {
                slot = k;
                break;
            }
        }
    }
    if (slot < 0) {
        tt->station_ignored++;
        return 0;
    }
    tt->e[slot].id = new_station_id(tt);
    tt->e[slot].dir = dir;
    tt->e[slot].source = LG_SRC_STATION;
    tt->e[slot].eta_ms = eta_ms;
    tt->e[slot].tt_eta_ms = eta_ms;
    tt->station_added++;
    return 2;
}

static int before(const lg_tt_entry_t *a, const lg_tt_entry_t *b) {
    return a->eta_ms < b->eta_ms || (a->eta_ms == b->eta_ms && a->id < b->id);
}

uint32_t lg_tt_next(const lg_tt_t *tt, uint8_t dir, uint32_t now_ms, lg_tt_entry_t *out, uint32_t max) {
    const lg_tt_entry_t *last = NULL;
    uint32_t n = 0;

    if (out == NULL || dir >= LG_DIR_COUNT) {
        return 0;
    }
    while (n < max) {
        const lg_tt_entry_t *best = NULL;
        uint8_t k;

        for (k = 0; k < tt->count; k++) {
            const lg_tt_entry_t *e = &tt->e[k];

            if (e->dir != dir || !listed(tt, e, now_ms) || (last != NULL && !before(last, e))) {
                continue;
            }
            if (best == NULL || before(e, best)) {
                best = e;
            }
        }
        if (best == NULL) {
            break;
        }
        out[n++] = *best;
        last = best;
    }
    return n;
}

void lg_tt_reply(const lg_tt_t *tt, uint8_t dir, uint32_t now_ms, uint64_t epoch_ms, lg_sched_reply_t *r) {
    lg_tt_entry_t next[LG_SCHED_ENTRIES];
    uint32_t n = 0;
    uint32_t i;

    memset((uint8_t *)r + sizeof r->hdr, 0, sizeof *r - sizeof r->hdr);
    r->dir = dir;
    if (!lg_tt_info(tt, dir)) {
        return;
    }
    r->flags = LG_SCHED_INFO;
    n = lg_tt_next(tt, dir, now_ms, next, LG_SCHED_ENTRIES);
    for (i = 0; i < n; i++) {
        r->entry[i].eta_ns = (epoch_ms + next[i].eta_ms) * 1000000ull;
        r->entry[i].lead_ms = tt->lead_ms;
        r->entry[i].tol_ms = tt->tol_ms;
        r->entry[i].id = next[i].id;
        r->entry[i].source = next[i].source;
        r->entry[i].confidence =
            next[i].source == LG_SRC_STATION ? (uint8_t)LG_TT_CONF_STATION : (uint8_t)LG_TT_CONF_TIMETABLE;
    }
    r->count = (uint8_t)n;
}

static const char *dir_str(uint8_t d) {
    return d == LG_DIR_WEST ? "west" : "east";
}

size_t lg_tt_text(const lg_tt_t *tt, uint32_t now_ms, char *buf, size_t len) {
    size_t used = 0;
    uint8_t d;

    if (buf == NULL || len == 0) {
        return 0;
    }
    buf[0] = '\0';
#define LG_TT_APPEND(...)                                                                                  \
    do {                                                                                                   \
        int w_ = snprintf(buf + used, len - used, __VA_ARGS__);                                            \
        if (w_ < 0 || (size_t)w_ >= len - used) {                                                          \
            buf[len - 1] = '\0';                                                                           \
            return len - 1;                                                                                \
        }                                                                                                  \
        used += (size_t)w_;                                                                                \
    } while (0)
    for (d = 0; d < LG_DIR_COUNT; d++) {
        lg_tt_entry_t next[LG_SCHED_ENTRIES];
        uint32_t n = lg_tt_next(tt, d, now_ms, next, LG_SCHED_ENTRIES);
        uint32_t i;

        LG_TT_APPEND("dir=%s info=%d outage=%u listed=%u world_ms=%u lead_ms=%u tol_ms=%u\n", dir_str(d),
                     lg_tt_info(tt, d), (unsigned)tt->outage[d], (unsigned)n, (unsigned)now_ms,
                     (unsigned)tt->lead_ms, (unsigned)tt->tol_ms);
        for (i = 0; i < n; i++) {
            long in_s = ((long)next[i].eta_ms - (long)now_ms) / 1000L;

            LG_TT_APPEND("entry id=%u dir=%s source=%s eta_ms=%u planned_ms=%u in_s=%ld\n", (unsigned)next[i].id,
                         dir_str(d), next[i].source == LG_SRC_STATION ? "STATION" : "TIMETABLE",
                         (unsigned)next[i].eta_ms, (unsigned)next[i].tt_eta_ms, in_s);
        }
    }
    LG_TT_APPEND("station rx=%u moved=%u added=%u ignored=%u rejected=%u\n", (unsigned)tt->station_rx,
                 (unsigned)tt->station_moved, (unsigned)tt->station_added, (unsigned)tt->station_ignored,
                 (unsigned)tt->station_rejected);
#undef LG_TT_APPEND
    return used;
}

/* ---- lg-controller side -------------------------------------------------------------------------- */

void lg_win_init(lg_win_dir_t *wd) {
    memset(wd, 0, sizeof *wd);
}

static lg_win_t *find_id(lg_win_dir_t *wd, uint16_t id) {
    int k;

    for (k = 0; k < LG_WIN_MAX; k++) {
        if (wd->w[k].state != LG_WIN_FREE && wd->w[k].id == id) {
            return &wd->w[k];
        }
    }
    return NULL;
}

static void set_times(lg_win_t *w, const lg_win_entry_t *e) {
    w->eta_ms = e->eta_ms;
    w->start_ms = e->eta_ms - e->lead_ms;
    w->end_ms = e->eta_ms + e->tol_ms;
    w->source = e->source;
}

void lg_win_update(lg_win_dir_t *wd, const lg_win_entry_t *e, uint32_t n) {
    uint32_t i;
    int k;

    if (n > LG_SCHED_ENTRIES) {
        n = LG_SCHED_ENTRIES;
    }
    for (k = 0; k < LG_WIN_MAX; k++) {
        wd->w[k].listed = 0;
    }
    for (i = 0; i < n; i++) {
        lg_win_t *w = find_id(wd, e[i].id);

        if (w == NULL) {
            for (k = 0; k < LG_WIN_MAX && w == NULL; k++) {
                if (wd->w[k].state == LG_WIN_FREE) {
                    w = &wd->w[k];
                }
            }
            /* No free slot: reuse a confirmed one that this reply does not list. */
            for (k = 0; k < LG_WIN_MAX && w == NULL; k++) {
                if (wd->w[k].state == LG_WIN_CONFIRMED && !wd->w[k].listed) {
                    uint32_t j;
                    int in_reply = 0;

                    for (j = 0; j < n; j++) {
                        in_reply = in_reply || e[j].id == wd->w[k].id;
                    }
                    if (!in_reply) {
                        w = &wd->w[k];
                    }
                }
            }
            if (w == NULL) {
                wd->dropped++; /* every slot holds an OPEN window: those keep the gate down anyway */
                continue;
            }
            memset(w, 0, sizeof *w);
            w->state = LG_WIN_PENDING;
            w->id = e[i].id;
        }
        set_times(w, &e[i]);
        w->listed = 1;
    }
    for (k = 0; k < LG_WIN_MAX; k++) {
        lg_win_t *w = &wd->w[k];

        /* A PENDING window that is not listed any more is forgotten. An OPEN one stays until it is
         * confirmed; a CONFIRMED one stays until its end (lg_win_tick), so that an entry that drops out
         * of the reply for a moment and comes back is not opened a second time. */
        if (!w->listed && w->state == LG_WIN_PENDING) {
            memset(w, 0, sizeof *w);
        }
    }
}

/* The oldest armed PENDING window: earliest eta, ties by id. */
static lg_win_t *oldest_armed(lg_win_dir_t *wd) {
    lg_win_t *best = NULL;
    int k;

    for (k = 0; k < LG_WIN_MAX; k++) {
        lg_win_t *w = &wd->w[k];

        if (w->state != LG_WIN_PENDING || !w->armed) {
            continue;
        }
        if (best == NULL || !lg_elapsed_ge(w->eta_ms, best->eta_ms, 0u) ||
            (w->eta_ms == best->eta_ms && w->id < best->id)) {
            best = w;
        }
    }
    return best;
}

uint32_t lg_win_tick(lg_win_dir_t *wd, uint8_t uncertain, uint16_t island_axles, uint32_t now_ms) {
    uint32_t overdue = 0;
    lg_win_t *a;
    int k;

    /* A train of this direction passed under normal detection: its window is done. */
    a = oldest_armed(wd);
    if (!uncertain && a != NULL && a->axles_at_open != island_axles) {
        a->state = LG_WIN_CONFIRMED;
        wd->confirmed_normal++;
        for (k = 0; k < LG_WIN_MAX; k++) {
            if (wd->w[k].state == LG_WIN_PENDING && wd->w[k].armed) {
                wd->w[k].axles_at_open = island_axles; /* the next train has to move the count again */
            }
        }
    }
    for (k = 0; k < LG_WIN_MAX; k++) {
        lg_win_t *w = &wd->w[k];

        if (w->state == LG_WIN_PENDING && lg_elapsed_ge(now_ms, w->start_ms, 0u)) {
            if (!w->armed) {
                w->armed = 1;
                w->axles_at_open = island_axles;
            }
            if (uncertain) {
                w->state = LG_WIN_OPEN; /* keeps the count from its arming */
                wd->opened++;
            }
        }
        if (w->state == LG_WIN_OPEN && !w->overdue && lg_elapsed_ge(now_ms, w->end_ms, 1u)) {
            w->overdue = 1;
            wd->overdue++;
            overdue++;
        }
        if (w->state == LG_WIN_CONFIRMED && !w->listed && lg_elapsed_ge(now_ms, w->end_ms, 1u)) {
            memset(w, 0, sizeof *w);
        }
    }
    return overdue;
}

int lg_win_open(const lg_win_dir_t *wd) {
    return lg_win_count(wd, LG_WIN_OPEN) > 0;
}

/* The oldest OPEN window: earliest eta, ties by id. */
static lg_win_t *oldest_open(lg_win_dir_t *wd) {
    lg_win_t *best = NULL;
    int k;

    for (k = 0; k < LG_WIN_MAX; k++) {
        lg_win_t *w = &wd->w[k];

        if (w->state != LG_WIN_OPEN) {
            continue;
        }
        if (best == NULL || !lg_elapsed_ge(w->eta_ms, best->eta_ms, 0u) ||
            (w->eta_ms == best->eta_ms && w->id < best->id)) {
            best = w;
        }
    }
    return best;
}

int lg_win_axles_moved(const lg_win_dir_t *wd, uint16_t island_axles) {
    const lg_win_t *o = oldest_open((lg_win_dir_t *)wd);

    return o != NULL && o->axles_at_open != island_axles;
}

int lg_win_confirm_island(lg_win_dir_t *wd, uint16_t island_axles) {
    lg_win_t *o = oldest_open(wd);
    int k;

    if (o == NULL) {
        return 0;
    }
    o->state = LG_WIN_CONFIRMED;
    wd->confirmed_island++;
    for (k = 0; k < LG_WIN_MAX; k++) {
        if (wd->w[k].state == LG_WIN_OPEN) {
            wd->w[k].axles_at_open = island_axles; /* the next train has to move the count again */
        }
    }
    return 1;
}

uint32_t lg_win_confirm_guard(lg_win_dir_t *wd) {
    uint32_t n = 0;
    int k;

    for (k = 0; k < LG_WIN_MAX; k++) {
        if (wd->w[k].state == LG_WIN_OPEN) {
            wd->w[k].state = LG_WIN_CONFIRMED;
            n++;
        }
    }
    wd->confirmed_guard += n;
    return n;
}

uint32_t lg_win_count(const lg_win_dir_t *wd, uint8_t state) {
    uint32_t n = 0;
    int k;

    for (k = 0; k < LG_WIN_MAX; k++) {
        n += wd->w[k].state == state;
    }
    return n;
}
