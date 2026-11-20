#include "lg_test.h"
#include "lg_voting.h"

static lg_vsensor_t sensor(uint8_t verdict, uint8_t fresh, uint8_t occupied) {
    lg_vsensor_t s;
    s.verdict = verdict;
    s.fresh = fresh;
    s.occupied = occupied;
    s.quality = 100;
    return s;
}

static lg_vote_t vote(lg_vsensor_t a, lg_vsensor_t b) {
    lg_vdir_t d;
    d.a = a;
    d.b = b;
    return lg_vote_direction(&d);
}

static void test_activation(void) {
    lg_vsensor_t ok0 = sensor(LG_VERDICT_OK, 1, 0);
    lg_vsensor_t ok1 = sensor(LG_VERDICT_OK, 1, 1);
    lg_vsensor_t warn1 = sensor(LG_VERDICT_WARNING, 1, 1);
    lg_vsensor_t failed1 = sensor(LG_VERDICT_FAILED, 1, 1);
    lg_vsensor_t failed0 = sensor(LG_VERDICT_FAILED, 1, 0);
    lg_vsensor_t stale1 = sensor(LG_VERDICT_OK, 0, 1);
    lg_vote_t v;

    /* 1oo2 truth table, both healthy. */
    v = vote(ok0, ok0);
    CHECK(v.detection == LG_DET_TRUSTED && !v.approaching);
    v = vote(ok1, ok0);
    CHECK(v.detection == LG_DET_TRUSTED && v.approaching);
    v = vote(ok0, ok1);
    CHECK(v.approaching);
    v = vote(ok1, ok1);
    CHECK(v.approaching);

    /* WARNING sensors still count. */
    v = vote(warn1, ok0);
    CHECK(v.detection == LG_DET_TRUSTED && v.approaching);

    /* A FAILED sensor is ignored, even when it says occupied. */
    v = vote(failed1, ok0);
    CHECK(v.detection == LG_DET_TRUSTED && !v.approaching);
    v = vote(failed1, ok1);
    CHECK(v.detection == LG_DET_TRUSTED && v.approaching);

    /* A stale sample is ignored. */
    v = vote(stale1, ok0);
    CHECK(!v.approaching);

    /* Both unusable: no trustworthy detection (the mode manager decides level 3 or 4). */
    v = vote(failed1, failed0);
    CHECK(v.detection == LG_DET_NONE && !v.approaching);
    v = vote(failed0, stale1);
    CHECK(v.detection == LG_DET_NONE && !v.approaching);
}

static void test_margin(void) {
    lg_vote_cfg_t cfg = lg_vote_cfg_default();

    CHECK(lg_activation_margin_ms(&cfg, LG_LEVEL_NORMAL) == cfg.prewarning_ms);
    CHECK(lg_activation_margin_ms(&cfg, LG_LEVEL_EARLY_WARNING) == cfg.prewarning_ms);
    CHECK(lg_activation_margin_ms(&cfg, LG_LEVEL_DEGRADED) ==
          cfg.prewarning_ms + cfg.extra_margin_ms);
    CHECK(lg_activation_margin_ms(&cfg, LG_LEVEL_UNCERTAIN) ==
          cfg.prewarning_ms + cfg.extra_margin_ms);
    CHECK(lg_activation_margin_ms(&cfg, LG_LEVEL_FAILSAFE) ==
          cfg.prewarning_ms + cfg.extra_margin_ms);
}

/* A clearing input where every condition is met at t = 10000 ms. */
static lg_clear_in_t good_input(void) {
    lg_clear_in_t in;
    in.now_ms = 10000;
    in.level = LG_LEVEL_NORMAL;
    in.island = sensor(LG_VERDICT_OK, 1, 0);
    in.axle_counting_healthy = 1;
    in.axles_in = 8;
    in.axles_out = 8;
    in.other_dir_approaching = 0;
    in.gate_down = 1;
    in.gate_down_since_ms = 1000;
    in.guard_confirm = 0;
    return in;
}

/* Step once at (now - t_clear) and once at now, so the island has been clear for t_clear. */
static uint32_t settled(lg_clear_t *c, const lg_clear_cfg_t *cfg, lg_clear_in_t in) {
    uint32_t t = in.now_ms;
    in.now_ms = t - cfg->t_clear_ms;
    (void)lg_clear_step(c, cfg, &in);
    in.now_ms = t;
    return lg_clear_step(c, cfg, &in);
}

/* The island reports the train first (occupied), then settles clear for t_clear. */
static uint32_t settled_after_sighting(lg_clear_t *c, const lg_clear_cfg_t *cfg, lg_clear_in_t in) {
    uint32_t t = in.now_ms;
    in.now_ms = t - cfg->t_clear_ms - 20;
    in.island.occupied = 1;
    (void)lg_clear_step(c, cfg, &in);
    in.island.occupied = 0;
    in.now_ms = t - cfg->t_clear_ms;
    (void)lg_clear_step(c, cfg, &in);
    in.now_ms = t;
    return lg_clear_step(c, cfg, &in);
}

static void test_clearing(void) {
    lg_clear_cfg_t cfg = lg_clear_cfg_default();
    lg_clear_t c;
    lg_clear_in_t in;

    /* Everything met: the gate may rise. */
    lg_clear_init(&c);
    in = good_input();
    CHECK(settled(&c, &cfg, in) == 0);

    /* Island clear for less than t_clear. */
    lg_clear_init(&c);
    in = good_input();
    in.now_ms = 5000;
    (void)lg_clear_step(&c, &cfg, &in);
    in.now_ms = 5000 + cfg.t_clear_ms - 1;
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_ISLAND_NOT_CLEAR);
    in.now_ms = 5000 + cfg.t_clear_ms;
    CHECK(!(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_ISLAND_NOT_CLEAR));

    /* Island occupied restarts the t_clear timer. */
    lg_clear_init(&c);
    in = good_input();
    in.now_ms = 5000;
    (void)lg_clear_step(&c, &cfg, &in);
    in.island.occupied = 1;
    in.now_ms = 5500;
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_ISLAND_NOT_CLEAR);
    in.island.occupied = 0;
    in.now_ms = 6000;
    (void)lg_clear_step(&c, &cfg, &in);
    in.now_ms = 6000 + cfg.t_clear_ms - 1;
    CHECK(lg_clear_step(&c, &cfg, &in) & LG_CLEAR_ISLAND_NOT_CLEAR);

    /* Failed or stale island sensor never counts as clear (levels 0 to 2). */
    lg_clear_init(&c);
    in = good_input();
    in.island = sensor(LG_VERDICT_FAILED, 1, 0);
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_ISLAND_NOT_CLEAR);
    lg_clear_init(&c);
    in = good_input();
    in.island = sensor(LG_VERDICT_OK, 0, 0);
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_ISLAND_NOT_CLEAR);

    /* Axle mismatch blocks, but only while axle counting is healthy. */
    lg_clear_init(&c);
    in = good_input();
    in.axles_out = 6;
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_AXLE_MISMATCH);
    lg_clear_init(&c);
    in.axle_counting_healthy = 0;
    in.axles_out = 6;
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_ISLAND_NOT_CLEAR); /* no sighting, no healthy counter */
    lg_clear_init(&c);
    CHECK(settled_after_sighting(&c, &cfg, in) == 0);          /* sighting replaces the counter */

    /* Another direction approaching blocks. */
    lg_clear_init(&c);
    in = good_input();
    in.other_dir_approaching = 1;
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_OTHER_APPROACHING);

    /* Minimum gate-down time, and the gate must be DOWN at all. */
    lg_clear_init(&c);
    in = good_input();
    in.gate_down_since_ms = in.now_ms - cfg.min_gate_down_ms + 1;
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_MIN_DOWN_TIME);
    lg_clear_init(&c);
    in = good_input();
    in.gate_down = 0;
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_GATE_NOT_DOWN);

    /* Level 4 never clears through this path, even with a guard confirmation. */
    lg_clear_init(&c);
    in = good_input();
    in.level = LG_LEVEL_FAILSAFE;
    in.guard_confirm = 1;
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_LEVEL4);
}

static void test_clearing_level3(void) {
    lg_clear_cfg_t cfg = lg_clear_cfg_default();
    lg_clear_t c;
    lg_clear_in_t in = good_input();

    in.level = LG_LEVEL_UNCERTAIN;

    /* Island clear but the train never seen: blocked (the gate may have lowered early). */
    lg_clear_init(&c);
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_ISLAND_NOT_CLEAR);

    /* Island saw the train and is now clear: may rise without a guard. */
    lg_clear_init(&c);
    CHECK(settled_after_sighting(&c, &cfg, in) == 0);

    /* Island sensor lost: blocked until a guard confirms. */
    lg_clear_init(&c);
    in.island = sensor(LG_VERDICT_FAILED, 1, 0);
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_ISLAND_NOT_CLEAR);
    lg_clear_init(&c);
    in.guard_confirm = 1;
    CHECK(settled(&c, &cfg, in) == 0);

    /* The guard confirmation does not override the other conditions. */
    lg_clear_init(&c);
    in.other_dir_approaching = 1;
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_OTHER_APPROACHING);
    lg_clear_init(&c);
    in.other_dir_approaching = 0;
    in.gate_down_since_ms = in.now_ms;
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_MIN_DOWN_TIME);

    /* A guard confirmation outside level 3 is ignored. */
    lg_clear_init(&c);
    in = good_input();
    in.level = LG_LEVEL_DEGRADED;
    in.island = sensor(LG_VERDICT_FAILED, 1, 0);
    in.guard_confirm = 1;
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_ISLAND_NOT_CLEAR);
}

static void test_clearing_wrap(void) {
    lg_clear_cfg_t cfg = lg_clear_cfg_default();
    lg_clear_t c;
    lg_clear_in_t in = good_input();

    /* The 32-bit millisecond counter wraps; elapsed time must still be right. */
    lg_clear_init(&c);
    in.now_ms = 0xFFFFFFFFu - 500u;
    in.gate_down_since_ms = in.now_ms - 10000u;
    (void)lg_clear_step(&c, &cfg, &in);
    in.now_ms += cfg.t_clear_ms; /* wraps past zero */
    CHECK(lg_clear_step(&c, &cfg, &in) == 0);
}

static void test_quality(void) {
    lg_vsensor_t ok = sensor(LG_VERDICT_OK, 1, 0);
    lg_vsensor_t blind = sensor(LG_VERDICT_OK, 1, 0);
    lg_vsensor_t weak = sensor(LG_VERDICT_WARNING, 1, 1);
    lg_clear_cfg_t cfg = lg_clear_cfg_default();
    lg_clear_t c;
    lg_clear_in_t in = good_input();

    /* A blind sensor (quality 0) reports a clear track, but it is not usable. */
    blind.quality = 0;
    CHECK(!lg_vsensor_usable(&blind));
    CHECK(lg_vote_direction(&(lg_vdir_t){blind, blind}).detection == LG_DET_NONE);
    CHECK(lg_vote_direction(&(lg_vdir_t){blind, ok}).detection == LG_DET_TRUSTED);

    /* Quality 1 is the lowest usable value; a noisy WARNING sensor is still used. */
    weak.quality = 1;
    CHECK(lg_vsensor_usable(&weak));
    CHECK(lg_vote_direction(&(lg_vdir_t){weak, ok}).approaching);
    weak.quality = 0;
    CHECK(!lg_vsensor_usable(&weak));
    CHECK(!lg_vote_direction(&(lg_vdir_t){weak, ok}).approaching); /* its occupied=1 is not trusted either */

    /* A blind island sensor never counts as clear, whatever it reports. */
    in.island = blind;
    lg_clear_init(&c);
    CHECK(settled(&c, &cfg, in) & LG_CLEAR_ISLAND_NOT_CLEAR);
    in.island = ok;
    lg_clear_init(&c);
    CHECK(settled(&c, &cfg, in) == 0);
}

int main(void) {
    test_quality();
    test_activation();
    test_margin();
    test_clearing();
    test_clearing_level3();
    test_clearing_wrap();
    LG_TEST_DONE("test_voting");
}
