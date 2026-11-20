#ifndef LG_HEALTH_H
#define LG_HEALTH_H

/*
 * Health scoring (README 8.3). Step 4 only provides the score-to-verdict mapping;
 * EWMA, CUSUM, dropout and disagreement statistics arrive in Step 11. Portable.
 */

#include <stdint.h>

#include "lg_config.h"
#include "lg_proto.h"

/* Maps a score 0..100 to OK / WARNING / FAILED using the thresholds in lg_config.h. */
lg_verdict_t lg_health_verdict_from_score(uint8_t score);

#endif
