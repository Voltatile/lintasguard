#include "lg_plant_model.h"

#include <stdio.h>
#include <string.h>

void lg_plant_init(lg_plant_t *p, const lg_crossing_cfg_t *cfg, const lg_scenario_t *sc) {
    p->cfg = *cfg;
    p->sc = *sc;
}

/* Front of the train on its path, mm (negative before the strike-in point). */
static int64_t front_mm(const lg_train_t *t, uint32_t now_ms) {
    return (int64_t)t->speed_mmps * ((int64_t)now_ms - (int64_t)t->t_strike_ms) / 1000;
}

/* Axles of the train that have passed the path point x, given the front position relative to x. */
static uint32_t axles_passed(const lg_train_t *t, int64_t front_rel_mm) {
    int64_t n = t->axles;
    int64_t c;

    if (front_rel_mm < 0) {
        return 0;
    }
    /* axle k is at front - (k + 0.5) L / N: it has passed when (2k + 1) L <= 2 N front */
    c = (2 * n * front_rel_mm + t->length_mm) / (2 * (int64_t)t->length_mm);
    if (c > n) {
        c = n;
    }
    return (uint32_t)c;
}

static int on_island(const lg_plant_t *p, const lg_train_t *t, int64_t front) {
    int64_t rear = front - t->length_mm;

    return front > p->cfg.strike_in_mm && rear < (int64_t)p->cfg.strike_in_mm + p->cfg.island_mm;
}

lg_truth_t lg_plant_truth(const lg_plant_t *p, uint32_t t_ms, uint8_t dir, uint8_t id) {
    lg_truth_t r;
    uint32_t axles = 0;
    uint32_t i;
    int64_t island_exit = (int64_t)p->cfg.strike_in_mm + p->cfg.island_mm;

    memset(&r, 0, sizeof r);
    if (dir >= LG_DIR_COUNT || id >= LG_SENSOR_ID_COUNT) {
        return r;
    }

    for (i = 0; i < p->sc.n_trains; i++) {
        const lg_train_t *t = &p->sc.trains[i];
        int64_t front = front_mm(t, t_ms);
        int64_t rear = front - t->length_mm;

        if (on_island(p, t, front)) {
            r.island_any = 1;
        }
        if (id == LG_SENSOR_ISLAND) {
            if (on_island(p, t, front)) {
                r.occupied = 1;
            }
            if (t->dir == dir) {
                axles += axles_passed(t, front - island_exit);
            }
        } else if (t->dir == dir) {
            /* approach sensor: direction aware, zone [0, Z] on the path of trains from this side */
            if (front > 0 && rear < p->cfg.sensor_zone_mm) {
                r.occupied = 1;
            }
            axles += axles_passed(t, front);
        }
    }
    r.axles = (uint16_t)(axles > 65535u ? 65535u : axles);
    return r;
}

/* Exact crossing time of path point x by the front, in ms, rounded down. */
static uint32_t crossing_ms(const lg_train_t *t, int64_t x_mm) {
    return (uint32_t)((int64_t)t->t_strike_ms + x_mm * 1000 / t->speed_mmps);
}

static int event_before(const lg_plant_event_t *a, const lg_plant_event_t *b) {
    if (a->t_ms != b->t_ms) {
        return a->t_ms < b->t_ms;
    }
    if (a->train_id != b->train_id) {
        return a->train_id < b->train_id;
    }
    return a->kind < b->kind;
}

size_t lg_plant_events(const lg_plant_t *p, int64_t t_prev_ms, int64_t t_now_ms,
                       lg_plant_event_t *out, size_t max) {
    lg_plant_event_t all[LG_SCENARIO_MAX_TRAINS * 4];
    size_t n = 0;
    size_t i;
    size_t k;
    int64_t island_in = p->cfg.strike_in_mm;
    int64_t island_out = (int64_t)p->cfg.strike_in_mm + p->cfg.island_mm;

    if (t_now_ms <= t_prev_ms || max == 0) {
        return 0;
    }
    for (i = 0; i < p->sc.n_trains; i++) {
        const lg_train_t *t = &p->sc.trains[i];
        uint32_t when[4];
        uint8_t kinds[4] = {LG_PLANT_EV_STRIKE_IN, LG_PLANT_EV_ISLAND_ENTER, LG_PLANT_EV_ISLAND_EXIT,
                            LG_PLANT_EV_CLEARED};

        when[0] = crossing_ms(t, 0);
        when[1] = crossing_ms(t, island_in);
        when[2] = crossing_ms(t, island_out);
        when[3] = crossing_ms(t, island_out + t->length_mm); /* the rear passes the island exit */
        for (k = 0; k < 4; k++) {
            if (when[k] > t_prev_ms && when[k] <= t_now_ms) {
                all[n].t_ms = when[k];
                all[n].train_id = t->id;
                all[n].kind = kinds[k];
                all[n].dir = t->dir;
                n++;
            }
        }
    }

    /* insertion sort: n is at most 256 */
    for (i = 1; i < n; i++) {
        lg_plant_event_t cur = all[i];
        size_t j = i;

        while (j > 0 && event_before(&cur, &all[j - 1])) {
            all[j] = all[j - 1];
            j--;
        }
        all[j] = cur;
    }
    if (n > max) {
        n = max;
    }
    memcpy(out, all, n * sizeof all[0]);
    return n;
}

lg_plant_counts_t lg_plant_counts(const lg_plant_t *p, uint32_t t_ms) {
    lg_plant_counts_t c;
    uint32_t i;
    int64_t island_exit = (int64_t)p->cfg.strike_in_mm + p->cfg.island_mm;

    memset(&c, 0, sizeof c);
    for (i = 0; i < p->sc.n_trains; i++) {
        const lg_train_t *t = &p->sc.trains[i];
        int64_t front = front_mm(t, t_ms);
        int64_t rear = front - t->length_mm;

        if (front <= 0) {
            c.waiting++;
        } else if (rear >= island_exit) {
            c.finished++;
        } else {
            c.active++;
        }
        if (on_island(p, t, front)) {
            c.island_any = 1;
        }
    }
    return c;
}

static int train_pos(const lg_plant_t *p, const lg_train_t *t, uint32_t t_ms, lg_plant_pos_t *out);

size_t lg_plant_text(const lg_plant_t *p, uint32_t t_ms, char *buf, size_t len) {
    lg_plant_counts_t c = lg_plant_counts(p, t_ms);
    size_t used;
    uint32_t i;

    if (len == 0) {
        return 0;
    }
    (void)snprintf(buf, len, "t_ms=%u scenario=%s trains=%u waiting=%u active=%u finished=%u island=%u\n",
                   (unsigned)t_ms, p->sc.name, (unsigned)p->sc.n_trains, (unsigned)c.waiting,
                   (unsigned)c.active, (unsigned)c.finished, (unsigned)c.island_any);
    used = strlen(buf);
    /* One line per visible train (Step 10, for lg-bridge), only when the whole line fits. */
    for (i = 0; i < p->sc.n_trains && i < LG_SCENARIO_MAX_TRAINS; i++) {
        lg_plant_pos_t pos;
        char line[96];
        int n;

        if (!train_pos(p, &p->sc.trains[i], t_ms, &pos)) {
            continue;
        }
        n = snprintf(line, sizeof line, "train id=%u dir=%s front_mm=%ld rear_mm=%ld\n", (unsigned)pos.id,
                     pos.dir == LG_DIR_WEST ? "west" : "east", (long)pos.front_mm, (long)pos.rear_mm);
        if (n < 0 || (size_t)n >= sizeof line || used + (size_t)n >= len) {
            break;
        }
        memcpy(buf + used, line, (size_t)n + 1u);
        used += (size_t)n;
    }
    return used;
}

/* The position of one train when it is visible (see the header); 0 when it is not. */
static int train_pos(const lg_plant_t *p, const lg_train_t *t, uint32_t t_ms, lg_plant_pos_t *out) {
    int64_t front = front_mm(t, t_ms);
    int64_t rear = front - t->length_mm;
    int64_t gone = 2 * (int64_t)p->cfg.strike_in_mm + p->cfg.island_mm;

    if (front <= 0 || rear >= gone) {
        return 0;
    }
    memset(out, 0, sizeof *out);
    out->id = t->id;
    out->dir = t->dir;
    out->front_mm = (int32_t)front; /* visible: front < gone + length, well inside int32 */
    out->rear_mm = (int32_t)rear;
    return 1;
}

size_t lg_plant_positions(const lg_plant_t *p, uint32_t t_ms, lg_plant_pos_t *out, size_t max) {
    size_t n = 0;
    uint32_t i;

    if (out == NULL) {
        return 0;
    }
    for (i = 0; i < p->sc.n_trains && i < LG_SCENARIO_MAX_TRAINS && n < max; i++) {
        if (train_pos(p, &p->sc.trains[i], t_ms, &out[n])) {
            n++;
        }
    }
    return n;
}
