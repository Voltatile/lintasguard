#ifndef LG_SENSOR_MODEL_H
#define LG_SENSOR_MODEL_H

/*
 * One simulated track sensor: ground truth in, a noisy sample out, with the fault modes of
 * README 7.4. Portable and pure: the caller passes the time and the truth; the only state is the
 * PRNG, the fault and a short history for DELAY. No allocation, no floating point.
 *
 * Raw level (ADC counts, 0..1023): LG_SENSOR_CLEAR_LEVEL with the track clear,
 * LG_SENSOR_OCCUPIED_LEVEL with a train, plus a uniform error of +- noise * counts_per_noise.
 * The sample reads occupied when the raw level is above LG_SENSOR_THRESHOLD, so high noise makes
 * the occupancy flip. Quality is 100 minus the noise units (0..100).
 *
 * Faults:
 *  NONE            healthy, base noise LG_SENSOR_BASE_NOISE
 *  NOISE(level)    noise units = level (0..100)
 *  DROPOUT(pct)    pct percent of the ticks produce no sample at all (0..100)
 *  STUCK_OCCUPIED  reads occupied whatever the truth, quality unchanged (a stuck sensor looks healthy)
 *  STUCK_CLEAR     reads clear whatever the truth, quality unchanged
 *  DELAY(ms)       reports the truth as it was ms ago (0..LG_SENSOR_DELAY_MAX_MS), axle counts too
 *  DRIFT(rate)     noise units start at the base noise and grow by rate per second since the
 *                  fault was set, capped at 100 (the quality only ever falls)
 *  HANG            no sample at all until cleared (the process stays up; supervisor in Step 8)
 *  CRASH           is a process-level fault (abort) and is rejected by the model
 */

#include <stddef.h>
#include <stdint.h>

#include "lg_config.h"
#include "lg_proto.h"

/*
 * Changes of the truth remembered for DELAY. The history stores one entry per change seen by a
 * step, so with the real 10 ms period at most 200 entries fall into the longest delay of 2 s and
 * every delay is exact. A caller that steps much faster (every ms) with changes at every step
 * gets exact results only up to a delay of 511 ms; an older target then reads as clear.
 * DRIFT and DELAY use 32-bit millisecond differences: spans above 2^31 ms (24 days) are not
 * supported.
 */
#define LG_SENSOR_HISTORY 512
#define LG_SENSOR_RAW_MAX 1023

typedef struct {
    uint32_t t_ms;
    uint16_t axles;
    uint8_t occupied;
    uint8_t reserved;
} lg_sensor_hist_t;

typedef struct {
    uint32_t rng;
    uint8_t fault; /* lg_sensor_fault_t */
    uint8_t reserved[3];
    uint32_t param;
    uint32_t fault_since_ms;
    lg_sensor_hist_t hist[LG_SENSOR_HISTORY]; /* ring of changes, oldest overwritten */
    uint32_t hist_head;                      /* index of the newest entry */
    uint32_t hist_count;
} lg_sensor_model_t;

typedef struct {
    uint8_t occupied;
    uint8_t quality; /* 0..100 */
    uint16_t axle_count;
    uint16_t raw_level;
} lg_sensor_out_t;

/* seed: any value (0 is replaced by a fixed constant); the same seed gives the same sequence. */
void lg_sensor_model_init(lg_sensor_model_t *m, uint32_t seed, uint32_t now_ms);

/* 0 on success, -1 for CRASH, an unknown mode or an out-of-range parameter (nothing changes). */
int lg_sensor_model_set_fault(lg_sensor_model_t *m, uint8_t mode, uint32_t param, uint32_t now_ms);

/*
 * One tick. Returns 1 and fills out when the sensor produced a sample, 0 when this tick has no
 * sample (DROPOUT, HANG). The history of the truth advances either way.
 */
int lg_sensor_model_step(lg_sensor_model_t *m, uint32_t now_ms, uint8_t truth_occupied,
                         uint16_t truth_axles, lg_sensor_out_t *out);

/* Noise units in effect at now_ms (0..100). */
uint32_t lg_sensor_model_noise(const lg_sensor_model_t *m, uint32_t now_ms);

const char *lg_sensor_fault_name(uint8_t mode);

#endif
