#ifndef LG_SCENARIO_H
#define LG_SCENARIO_H

/*
 * Crossing geometry and train scenarios, loaded from JSON (config/crossing.json and the
 * files in scenarios/). Portable. Every value is range checked: a file that does not make sense is
 * refused with a message, never silently clamped. Units: mm, mm/s, ms (no floating point).
 *
 * The model has one track per direction (a double track): trains of different directions never
 * share the track, trains of the same direction must not overlap on it.
 */

#include <stddef.h>
#include <stdint.h>

#include "lg_json_mini.h"
#include "lg_proto.h"

#define LG_SCENARIO_MAX_TRAINS 64
#define LG_SCENARIO_NAME_LEN 32
#define LG_ERR_LEN 120

typedef struct {
    int32_t strike_in_mm;   /* distance from the strike-in sensors to the island, on each side */
    int32_t island_mm;      /* length of the island */
    int32_t sensor_zone_mm; /* length of the zone one sensor covers */
} lg_crossing_cfg_t;

typedef struct {
    uint16_t id;
    uint8_t dir; /* lg_dir_t of the side the train comes from */
    uint8_t axles;
    uint32_t t_strike_ms;  /* when the front reaches the strike-in point of its side */
    int32_t speed_mmps;
    int32_t length_mm;
} lg_train_t;

typedef struct {
    char name[LG_SCENARIO_NAME_LEN];
    uint32_t n_trains;
    lg_train_t trains[LG_SCENARIO_MAX_TRAINS];
} lg_scenario_t;

/* Both return 0 on success. On failure they return -1 and put a short reason in err. */
int lg_crossing_from_json(const lg_json_doc_t *doc, lg_crossing_cfg_t *out, char *err, size_t errlen);
int lg_scenario_from_json(const lg_json_doc_t *doc, const lg_crossing_cfg_t *cfg, lg_scenario_t *out,
                          char *err, size_t errlen);

/* Reads a whole file into buf (NUL terminated). 0 on success, -1 on error or when it does not fit. */
int lg_read_file(const char *path, char *buf, size_t cap, size_t *len);

#endif
