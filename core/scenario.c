#include "lg_scenario.h"

#include <stdio.h>
#include <string.h>

static int fail(char *err, size_t errlen, const char *what) {
    if (err != NULL && errlen > 0) {
        (void)snprintf(err, errlen, "%s", what);
    }
    return -1;
}

/* A member that must be a number in [lo, hi] (in whole units, milli precision allowed). */
static int num_in_range(const lg_json_doc_t *doc, const lg_json_node_t *obj, const char *key,
                        int64_t lo_milli, int64_t hi_milli, int64_t *out, char *err, size_t errlen) {
    int64_t v;
    char msg[LG_ERR_LEN];

    if (lg_json_get_milli(doc, obj, key, &v) != 0) {
        (void)snprintf(msg, sizeof msg, "missing or not a number: %s", key);
        return fail(err, errlen, msg);
    }
    if (v < lo_milli || v > hi_milli) {
        (void)snprintf(msg, sizeof msg, "%s out of range [%lld, %lld]", key,
                       (long long)(lo_milli / 1000), (long long)(hi_milli / 1000));
        return fail(err, errlen, msg);
    }
    *out = v;
    return 0;
}

int lg_crossing_from_json(const lg_json_doc_t *doc, lg_crossing_cfg_t *out, char *err, size_t errlen) {
    const lg_json_node_t *root = lg_json_root(doc);
    int64_t v;

    if (root == NULL || root->type != LG_JSON_OBJECT) {
        return fail(err, errlen, "crossing: the top level must be an object");
    }
    if (num_in_range(doc, root, "strike_in_m", 50 * 1000, 5000 * 1000, &v, err, errlen) != 0) {
        return -1;
    }
    out->strike_in_mm = (int32_t)v;
    if (num_in_range(doc, root, "island_m", 1 * 1000, 200 * 1000, &v, err, errlen) != 0) {
        return -1;
    }
    out->island_mm = (int32_t)v;
    if (num_in_range(doc, root, "sensor_zone_m", 1 * 1000, 20 * 1000, &v, err, errlen) != 0) {
        return -1;
    }
    out->sensor_zone_mm = (int32_t)v;
    if (out->sensor_zone_mm >= out->strike_in_mm) {
        return fail(err, errlen, "sensor_zone_m must be smaller than strike_in_m");
    }
    return 0;
}

/*
 * Two trains of one direction on one track must never overlap. The gap between the rear of the
 * leader and the front of the follower is linear in time, so it is enough to check it when the
 * follower appears and when the leader has left the island.
 */
static int overlaps(const lg_crossing_cfg_t *cfg, const lg_train_t *lead, const lg_train_t *follow) {
    int64_t path_end = (int64_t)cfg->strike_in_mm + cfg->island_mm; /* island exit, mm */
    int64_t t_follow = follow->t_strike_ms;
    int64_t t_lead_gone;
    int64_t gap_start;
    int64_t gap_end;

    /* u_front(t) = v * (t - t_strike) / 1000; the leader is gone when its rear passes path_end. */
    t_lead_gone = lead->t_strike_ms + ((path_end + lead->length_mm) * 1000) / lead->speed_mmps;
    gap_start = (int64_t)lead->speed_mmps * (t_follow - lead->t_strike_ms) / 1000 - lead->length_mm;
    gap_end = ((int64_t)lead->speed_mmps * (t_lead_gone - lead->t_strike_ms) / 1000 - lead->length_mm) -
              (int64_t)follow->speed_mmps * (t_lead_gone - t_follow) / 1000;
    if (t_follow >= t_lead_gone) {
        return 0; /* the leader was gone before the follower appeared */
    }
    return gap_start < 0 || gap_end < 0;
}

int lg_scenario_from_json(const lg_json_doc_t *doc, const lg_crossing_cfg_t *cfg, lg_scenario_t *out,
                          char *err, size_t errlen) {
    const lg_json_node_t *root = lg_json_root(doc);
    const lg_json_node_t *trains;
    const char *name = NULL;
    uint32_t i;
    uint32_t j;
    char msg[LG_ERR_LEN];

    memset(out, 0, sizeof *out);
    if (root == NULL || root->type != LG_JSON_OBJECT) {
        return fail(err, errlen, "scenario: the top level must be an object");
    }
    if (lg_json_get_str(doc, root, "name", &name) != 0 || name[0] == '\0') {
        return fail(err, errlen, "scenario: missing or empty name");
    }
    if (strlen(name) >= LG_SCENARIO_NAME_LEN) {
        return fail(err, errlen, "scenario: name too long");
    }
    (void)snprintf(out->name, sizeof out->name, "%s", name);

    trains = lg_json_get(doc, root, "trains");
    if (trains == NULL || trains->type != LG_JSON_ARRAY) {
        return fail(err, errlen, "scenario: missing trains array");
    }
    if (trains->count > LG_SCENARIO_MAX_TRAINS) {
        return fail(err, errlen, "scenario: too many trains");
    }

    for (i = 0; i < trains->count; i++) {
        const lg_json_node_t *t = lg_json_at(doc, trains, i);
        lg_train_t *tr = &out->trains[i];
        const char *dir = NULL;
        int64_t v;

        if (t == NULL || t->type != LG_JSON_OBJECT) {
            (void)snprintf(msg, sizeof msg, "train %u: not an object", (unsigned)i);
            return fail(err, errlen, msg);
        }
        if (num_in_range(doc, t, "id", 1000, 65535 * 1000, &v, err, errlen) != 0) {
            return -1;
        }
        tr->id = (uint16_t)(v / 1000);
        if (v % 1000 != 0) {
            return fail(err, errlen, "train id must be a whole number");
        }
        for (j = 0; j < i; j++) {
            if (out->trains[j].id == tr->id) {
                (void)snprintf(msg, sizeof msg, "duplicate train id %u", (unsigned)tr->id);
                return fail(err, errlen, msg);
            }
        }
        if (lg_json_get_str(doc, t, "dir", &dir) != 0) {
            (void)snprintf(msg, sizeof msg, "train %u: missing dir", (unsigned)tr->id);
            return fail(err, errlen, msg);
        }
        if (strcmp(dir, "west") == 0) {
            tr->dir = LG_DIR_WEST;
        } else if (strcmp(dir, "east") == 0) {
            tr->dir = LG_DIR_EAST;
        } else {
            (void)snprintf(msg, sizeof msg, "train %u: dir must be west or east", (unsigned)tr->id);
            return fail(err, errlen, msg);
        }
        if (num_in_range(doc, t, "t_strike_s", 0, 86400 * 1000, &v, err, errlen) != 0) {
            return -1;
        }
        tr->t_strike_ms = (uint32_t)v;
        if (num_in_range(doc, t, "speed_mps", 1 * 1000, 100 * 1000, &v, err, errlen) != 0) {
            return -1;
        }
        tr->speed_mmps = (int32_t)v; /* m/s in milli units is exactly mm/s */
        if (num_in_range(doc, t, "length_m", 5 * 1000, 1000 * 1000, &v, err, errlen) != 0) {
            return -1;
        }
        tr->length_mm = (int32_t)v;
        if (num_in_range(doc, t, "axles", 1000, 200 * 1000, &v, err, errlen) != 0) {
            return -1;
        }
        if (v % 1000 != 0) {
            return fail(err, errlen, "axles must be a whole number");
        }
        tr->axles = (uint8_t)(v / 1000);
    }
    out->n_trains = trains->count;

    for (i = 0; i < out->n_trains; i++) {
        for (j = 0; j < out->n_trains; j++) {
            const lg_train_t *a = &out->trains[i];
            const lg_train_t *b = &out->trains[j];

            /* a leads b: same direction and a strikes in no later than b (ties: lower index leads) */
            if (i == j || a->dir != b->dir) {
                continue;
            }
            if (a->t_strike_ms > b->t_strike_ms || (a->t_strike_ms == b->t_strike_ms && i > j)) {
                continue;
            }
            if (overlaps(cfg, a, b)) {
                (void)snprintf(msg, sizeof msg, "trains %u and %u overlap on the same track",
                               (unsigned)a->id, (unsigned)b->id);
                return fail(err, errlen, msg);
            }
        }
    }
    return 0;
}

int lg_read_file(const char *path, char *buf, size_t cap, size_t *len) {
    FILE *f;
    size_t n;
    int read_error;
    int more = 0;

    if (path == NULL || buf == NULL || len == NULL || cap == 0) {
        return -1;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    n = fread(buf, 1, cap - 1, f);
    read_error = ferror(f);
    if (!read_error) {
        more = fgetc(f) != EOF; /* data left over: the file does not fit */
    }
    (void)fclose(f);
    if (read_error || more) {
        return -1;
    }
    buf[n] = '\0';
    *len = n;
    return 0;
}
