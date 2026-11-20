#ifndef LG_VOTING_H
#define LG_VOTING_H

/* Activation voting (1oo2 per direction) and gate clearing rules. README 8.1, 8.2. Portable. */

#include <stdint.h>

#include "lg_config.h"
#include "lg_proto.h"

/* One sensor as seen by the controller. A FAILED or stale sensor is not usable. */
typedef struct {
    uint8_t verdict;  /* lg_verdict_t from lg-health */
    uint8_t fresh;    /* sample younger than LG_SAMPLE_MAX_AGE_MS */
    uint8_t occupied; /* last sample */
} lg_vsensor_t;

typedef struct {
    lg_vsensor_t a;
    lg_vsensor_t b;
} lg_vdir_t;

typedef enum {
    LG_DET_TRUSTED = 0, /* at least one usable approach sensor */
    LG_DET_NONE = 1     /* no trustworthy approach detection in this direction */
} lg_detection_t;

typedef struct {
    lg_detection_t detection;
    uint8_t approaching; /* any usable approach sensor reports occupied */
} lg_vote_t;

/* A sensor is usable when it is not FAILED and its sample is fresh. WARNING is still used. */
int lg_vsensor_usable(const lg_vsensor_t *s);

lg_vote_t lg_vote_direction(const lg_vdir_t *d);

typedef struct {
    uint32_t prewarning_ms;
    uint32_t extra_margin_ms; /* added in level 2 */
} lg_vote_cfg_t;

lg_vote_cfg_t lg_vote_cfg_default(void);

/* Pre-warning time before the gate lowers, for the current level. */
uint32_t lg_activation_margin_ms(const lg_vote_cfg_t *cfg, uint8_t level);

/* ---- Clearing --------------------------------------------------------- */

/* Bits of the "unmet conditions" mask returned by lg_clear_step. 0 means the gate may rise. */
#define LG_CLEAR_ISLAND_NOT_CLEAR (1u << 0)
#define LG_CLEAR_AXLE_MISMATCH (1u << 1)
#define LG_CLEAR_OTHER_APPROACHING (1u << 2)
#define LG_CLEAR_MIN_DOWN_TIME (1u << 3)
#define LG_CLEAR_LEVEL4 (1u << 4)
#define LG_CLEAR_GATE_NOT_DOWN (1u << 5)

typedef struct {
    uint32_t t_clear_ms;
    uint32_t min_gate_down_ms;
} lg_clear_cfg_t;

lg_clear_cfg_t lg_clear_cfg_default(void);

typedef struct {
    uint32_t island_clear_since_ms;
    uint8_t island_clear_tracking;
    uint8_t island_seen_occupied; /* the island sensor reported the train since the last reset */
} lg_clear_t;

typedef struct {
    uint32_t now_ms;
    uint8_t level;                 /* lg_level_t */
    lg_vsensor_t island;           /* island sensor of the direction being cleared */
    uint8_t axle_counting_healthy; /* axle counts are only compared when this is set */
    uint16_t axles_in;
    uint16_t axles_out;
    uint8_t other_dir_approaching;
    uint8_t gate_down; /* gate state is DOWN */
    uint32_t gate_down_since_ms;
    uint8_t guard_confirm; /* authorised guard confirmation, counts only in level 3 */
} lg_clear_in_t;

void lg_clear_init(lg_clear_t *c);
void lg_clear_reset(lg_clear_t *c); /* call when a new activation starts */

/*
 * Call every tick while the gate is down for a train. Returns the mask of unmet
 * conditions (0 = the gate may rise). Level 4 never clears through this path.
 * "Island clear" means: the island sensor saw the train and has been clear for t_clear.
 * The sighting is only required where nothing else proves the train passed: in level 3,
 * or when axle counting is not healthy (otherwise equal axle counts cover it).
 * In level 3 a guard confirmation replaces the island condition only; a healthy axle
 * counter, other-direction traffic and the minimum down time still apply.
 * Out-of-range levels are treated as level 4.
 */
uint32_t lg_clear_step(lg_clear_t *c, const lg_clear_cfg_t *cfg, const lg_clear_in_t *in);

#endif
