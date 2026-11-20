#include "lg_health.h"

lg_verdict_t lg_health_verdict_from_score(uint8_t score) {
    if (score >= LG_SCORE_OK_MIN) {
        return LG_VERDICT_OK;
    }
    if (score >= LG_SCORE_WARNING_MIN) {
        return LG_VERDICT_WARNING;
    }
    return LG_VERDICT_FAILED;
}
