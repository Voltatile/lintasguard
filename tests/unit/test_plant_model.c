#include <stdio.h>
#include <string.h>

#include "lg_plant_model.h"
#include "lg_test.h"

/*
 * Geometry used here: D = 400 m, island 12 m, zone 2 m. Train 1 comes from the west, strikes in
 * at 5 s, 30 m/s (= 30 mm per ms), 120 m long, 8 axles (one axle per 15 m, the first one 7.5 m
 * behind the front). In mm: front(t) = 30 * (t - 5000).
 */

static lg_crossing_cfg_t cfg = {400000, 12000, 2000};
static lg_scenario_t sc;
static lg_plant_t plant;

static void add_train(uint16_t id, uint8_t dir, uint32_t t_strike_ms, int32_t speed_mmps,
                      int32_t length_mm, uint8_t axles) {
    lg_train_t *t = &sc.trains[sc.n_trains++];

    t->id = id;
    t->dir = dir;
    t->t_strike_ms = t_strike_ms;
    t->speed_mmps = speed_mmps;
    t->length_mm = length_mm;
    t->axles = axles;
}

static void reset(void) {
    memset(&sc, 0, sizeof sc);
    (void)snprintf(sc.name, sizeof sc.name, "unit");
}

static void start(void) {
    lg_plant_init(&plant, &cfg, &sc);
}

static lg_truth_t truth(uint32_t t, uint8_t dir, uint8_t id) {
    return lg_plant_truth(&plant, t, dir, id);
}

static void test_approach_edges(void) {
    reset();
    add_train(1, LG_DIR_WEST, 5000, 30000, 120000, 8);
    start();

    /* Before the strike-in point and exactly at it: nothing yet (front must pass u = 0). */
    CHECK(!truth(0, LG_DIR_WEST, LG_SENSOR_A).occupied);
    CHECK(!truth(4999, LG_DIR_WEST, LG_SENSOR_A).occupied);
    CHECK(!truth(5000, LG_DIR_WEST, LG_SENSOR_A).occupied);
    CHECK(truth(5001, LG_DIR_WEST, LG_SENSOR_A).occupied);
    CHECK(truth(5001, LG_DIR_WEST, LG_SENSOR_B).occupied); /* A and B see the same point */

    /* The rear clears the zone when front - length >= zone: front = 122000 mm, t = 9066.67 ms. */
    CHECK(truth(9066, LG_DIR_WEST, LG_SENSOR_A).occupied);
    CHECK(!truth(9067, LG_DIR_WEST, LG_SENSOR_A).occupied);
    CHECK(!truth(60000, LG_DIR_WEST, LG_SENSOR_A).occupied);
    CHECK(!truth(0xFFFFFFFFu, LG_DIR_WEST, LG_SENSOR_A).occupied);
}

static void test_direction_awareness(void) {
    uint32_t t;

    reset();
    add_train(1, LG_DIR_WEST, 5000, 30000, 120000, 8);
    start();

    /* The east approach sensors never see a west train, not on the way in and not when it leaves. */
    for (t = 0; t < 60000; t += 7) {
        CHECK(!truth(t, LG_DIR_EAST, LG_SENSOR_A).occupied);
        CHECK(!truth(t, LG_DIR_EAST, LG_SENSOR_B).occupied);
        CHECK(truth(t, LG_DIR_EAST, LG_SENSOR_A).axles == 0);
    }
}

static void test_island_edges(void) {
    reset();
    add_train(1, LG_DIR_WEST, 5000, 30000, 120000, 8);
    start();

    /* Entry: front > 400000 mm, t = 18333.33 ms. */
    CHECK(!truth(18333, LG_DIR_WEST, LG_SENSOR_ISLAND).occupied);
    CHECK(truth(18334, LG_DIR_WEST, LG_SENSOR_ISLAND).occupied);
    /* Exit: rear >= 412000 mm, front >= 532000 mm, t = 22733.33 ms. */
    CHECK(truth(22733, LG_DIR_WEST, LG_SENSOR_ISLAND).occupied);
    CHECK(!truth(22734, LG_DIR_WEST, LG_SENSOR_ISLAND).occupied);

    /* Both island sensors see any train: occupancy is physical, not per side. */
    CHECK(truth(20000, LG_DIR_EAST, LG_SENSOR_ISLAND).occupied);
    CHECK(truth(20000, LG_DIR_WEST, LG_SENSOR_ISLAND).island_any);
    CHECK(truth(20000, LG_DIR_WEST, LG_SENSOR_A).island_any); /* island_any is not sensor specific */
    CHECK(!truth(10000, LG_DIR_WEST, LG_SENSOR_A).island_any);
}

static void test_axle_counts(void) {
    uint32_t t;
    uint16_t last = 0;

    reset();
    add_train(1, LG_DIR_WEST, 5000, 30000, 120000, 8);
    start();

    /* Approach: axle k passes u = 0 when front = (k + 0.5) * 15000, i.e. 7500, 22500, ... */
    CHECK(truth(5000 + 249, LG_DIR_WEST, LG_SENSOR_A).axles == 0); /* front 7470 */
    CHECK(truth(5000 + 250, LG_DIR_WEST, LG_SENSOR_A).axles == 1); /* front 7500 */
    CHECK(truth(5000 + 749, LG_DIR_WEST, LG_SENSOR_A).axles == 1); /* front 22470 */
    CHECK(truth(5000 + 750, LG_DIR_WEST, LG_SENSOR_A).axles == 2); /* front 22500 */
    CHECK(truth(5000 + 3750, LG_DIR_WEST, LG_SENSOR_A).axles == 8); /* front 112500: the last one */
    CHECK(truth(5000 + 3749, LG_DIR_WEST, LG_SENSOR_A).axles == 7);
    CHECK(truth(50000, LG_DIR_WEST, LG_SENSOR_A).axles == 8);       /* stays at the total */
    CHECK(truth(50000, LG_DIR_WEST, LG_SENSOR_B).axles == 8);

    /* Counting never goes down, never exceeds the axle number. */
    for (t = 0; t < 60000; t++) {
        uint16_t a = truth(t, LG_DIR_WEST, LG_SENSOR_A).axles;

        CHECK(a >= last && a <= 8);
        last = a;
    }

    /* Island out count: axle k leaves when front - 412000 >= (k + 0.5) * 15000. */
    CHECK(truth(5000 + (412000 + 7499) / 30, LG_DIR_WEST, LG_SENSOR_ISLAND).axles == 0);
    CHECK(truth(5000 + (412000 + 7500 + 29) / 30, LG_DIR_WEST, LG_SENSOR_ISLAND).axles == 1);
    CHECK(truth(60000, LG_DIR_WEST, LG_SENSOR_ISLAND).axles == 8);
    /* The east island sensor counts axles of east trains only: none for this west train. */
    CHECK(truth(60000, LG_DIR_EAST, LG_SENSOR_ISLAND).axles == 0);
    CHECK(truth(20000, LG_DIR_EAST, LG_SENSOR_ISLAND).occupied == 1);
}

static void test_east_train_mirrors(void) {
    reset();
    add_train(7, LG_DIR_EAST, 30000, 30000, 120000, 6);
    start();

    CHECK(!truth(30000, LG_DIR_EAST, LG_SENSOR_A).occupied);
    CHECK(truth(30001, LG_DIR_EAST, LG_SENSOR_A).occupied);
    CHECK(!truth(30001, LG_DIR_WEST, LG_SENSOR_A).occupied);
    CHECK(truth(43334, LG_DIR_EAST, LG_SENSOR_ISLAND).occupied);
    CHECK(truth(60000, LG_DIR_EAST, LG_SENSOR_A).axles == 6);
    CHECK(truth(60000, LG_DIR_EAST, LG_SENSOR_ISLAND).axles == 6);
    CHECK(truth(60000, LG_DIR_WEST, LG_SENSOR_ISLAND).axles == 0);
}

static void test_two_trains(void) {
    reset();
    /* Two west trains one after the other (the second strikes in once the first one has passed). */
    add_train(1, LG_DIR_WEST, 5000, 30000, 120000, 8);
    add_train(2, LG_DIR_WEST, 40000, 30000, 60000, 4);
    start();

    CHECK(truth(20000, LG_DIR_WEST, LG_SENSOR_A).axles == 8);
    CHECK(!truth(20000, LG_DIR_WEST, LG_SENSOR_A).occupied);
    CHECK(truth(40001, LG_DIR_WEST, LG_SENSOR_A).occupied);
    CHECK(truth(60000, LG_DIR_WEST, LG_SENSOR_A).axles == 12); /* 8 + 4 accumulate */
    CHECK(truth(90000, LG_DIR_WEST, LG_SENSOR_ISLAND).axles == 12);

    /* A west and an east train on the island at the same time (two tracks): island sees both. */
    reset();
    add_train(1, LG_DIR_WEST, 5000, 30000, 120000, 8);
    add_train(2, LG_DIR_EAST, 5000, 30000, 120000, 8);
    start();
    CHECK(truth(20000, LG_DIR_WEST, LG_SENSOR_ISLAND).occupied);
    CHECK(truth(5001, LG_DIR_WEST, LG_SENSOR_A).occupied && truth(5001, LG_DIR_EAST, LG_SENSOR_A).occupied);
    CHECK(truth(60000, LG_DIR_WEST, LG_SENSOR_ISLAND).axles == 8);
    CHECK(truth(60000, LG_DIR_EAST, LG_SENSOR_ISLAND).axles == 8);
}

static void test_short_and_slow_trains(void) {
    reset();
    /* A 5 m train at 1 m/s: occupies the 2 m zone from 1 ms until its rear passes it, 7 s later. */
    add_train(1, LG_DIR_WEST, 1000, 1000, 5000, 2);
    start();
    CHECK(!truth(1000, LG_DIR_WEST, LG_SENSOR_A).occupied);
    CHECK(truth(1001, LG_DIR_WEST, LG_SENSOR_A).occupied);
    CHECK(truth(7999, LG_DIR_WEST, LG_SENSOR_A).occupied);  /* front 6999, rear 1999 < 2000 */
    CHECK(!truth(8000, LG_DIR_WEST, LG_SENSOR_A).occupied); /* rear 2000: cleared */
    /* Axles: first axle 1.25 m behind the front (2.5 m / 2 axles), second 3.75 m. */
    CHECK(truth(1000 + 1249, LG_DIR_WEST, LG_SENSOR_A).axles == 0);
    CHECK(truth(1000 + 1250, LG_DIR_WEST, LG_SENSOR_A).axles == 1);
    CHECK(truth(1000 + 3750, LG_DIR_WEST, LG_SENSOR_A).axles == 2);

    /* A train longer than the whole island and zone covers the island and the zone at once. */
    reset();
    add_train(1, LG_DIR_WEST, 0, 100000, 500000, 20);
    start();
    CHECK(truth(4500, LG_DIR_WEST, LG_SENSOR_A).occupied);                   /* front 450000 */
    CHECK(truth(4500, LG_DIR_WEST, LG_SENSOR_ISLAND).occupied);
    CHECK(truth(4500, LG_DIR_WEST, LG_SENSOR_A).island_any);
}

static void test_unknown_inputs(void) {
    lg_truth_t t;

    reset();
    add_train(1, LG_DIR_WEST, 0, 30000, 120000, 8);
    start();

    t = truth(5000, 2, LG_SENSOR_A);
    CHECK(t.occupied == 0 && t.axles == 0 && t.island_any == 0);
    t = truth(5000, LG_DIR_WEST, 3);
    CHECK(t.occupied == 0 && t.axles == 0);
    t = truth(5000, 255, 255);
    CHECK(t.occupied == 0 && t.axles == 0);

    /* An empty scenario is a valid world with nothing in it. */
    reset();
    start();
    CHECK(!truth(1000, LG_DIR_WEST, LG_SENSOR_A).occupied);
    CHECK(truth(1000, LG_DIR_WEST, LG_SENSOR_ISLAND).axles == 0);
}

static void test_maximum_axles(void) {
    uint32_t i;

    /* The scenario limits (64 trains, 200 axles) give at most 12800 axles: far below 16 bits, so the
     * count never overflows. The model still saturates at 65535 as a safety net. */
    reset();
    for (i = 0; i < 64; i++) {
        add_train((uint16_t)(i + 1), LG_DIR_WEST, i * 100000u, 100000, 5000, 200);
    }
    start();
    CHECK(truth(0xFFFFFFFFu, LG_DIR_WEST, LG_SENSOR_A).axles == 64 * 200);
    CHECK(truth(0xFFFFFFFFu, LG_DIR_WEST, LG_SENSOR_ISLAND).axles == 64 * 200);
}

static void test_far_future(void) {
    reset();
    add_train(1, LG_DIR_WEST, 86400000u, 100000, 1000000, 200);
    start();
    /* Large times stay inside the integer range: 100 m/s for 49 days is far below 2^63 mm. */
    CHECK(truth(0xFFFFFFFFu, LG_DIR_WEST, LG_SENSOR_A).axles == 200);
    CHECK(!truth(0xFFFFFFFFu, LG_DIR_WEST, LG_SENSOR_ISLAND).occupied);
}

static void test_counts_and_text(void) {
    lg_plant_counts_t c;
    char buf[160];
    char tiny[6];
    size_t n;

    reset();
    add_train(1, LG_DIR_WEST, 5000, 30000, 120000, 8);
    add_train(2, LG_DIR_EAST, 30000, 30000, 120000, 6);
    start();

    c = lg_plant_counts(&plant, 0);
    CHECK(c.waiting == 2 && c.active == 0 && c.finished == 0 && c.island_any == 0);
    c = lg_plant_counts(&plant, 5001);
    CHECK(c.waiting == 1 && c.active == 1 && c.finished == 0);
    c = lg_plant_counts(&plant, 20000);
    CHECK(c.active == 1 && c.island_any == 1);
    c = lg_plant_counts(&plant, 22733);
    CHECK(c.active == 1 && c.finished == 0 && c.island_any == 1);
    c = lg_plant_counts(&plant, 22734);
    CHECK(c.active == 0 && c.finished == 1 && c.waiting == 1 && c.island_any == 0);
    c = lg_plant_counts(&plant, 100000);
    CHECK(c.waiting == 0 && c.active == 0 && c.finished == 2);

    n = lg_plant_text(&plant, 20000, buf, sizeof buf);
    CHECK(n == strlen(buf) && n > 0 && buf[n - 1] == '\n');
    CHECK(strstr(buf, "t_ms=20000") != NULL && strstr(buf, "island=1") != NULL);
    CHECK(strstr(buf, "scenario=unit") != NULL && strstr(buf, "active=1") != NULL);
    n = lg_plant_text(&plant, 20000, tiny, sizeof tiny);
    CHECK(n < sizeof tiny && tiny[n] == '\0');
    CHECK(lg_plant_text(&plant, 20000, tiny, 0) == 0);
}

static size_t events(uint32_t from, uint32_t to, lg_plant_event_t *ev, size_t max) {
    return lg_plant_events(&plant, from, to, ev, max);
}

static void test_events(void) {
    lg_plant_event_t ev[16];
    size_t n;

    reset();
    add_train(1, LG_DIR_WEST, 5000, 30000, 120000, 8);
    add_train(2, LG_DIR_EAST, 30000, 30000, 120000, 6);
    start();

    /* Strike-in at 5000: (4999, 5000] contains it, (5000, 5001] does not. */
    n = events(4999, 5000, ev, 16);
    CHECK(n == 1 && ev[0].kind == LG_PLANT_EV_STRIKE_IN && ev[0].train_id == 1 &&
          ev[0].t_ms == 5000 && ev[0].dir == LG_DIR_WEST);
    CHECK(events(5000, 5001, ev, 16) == 0);
    CHECK(events(5000, 5000, ev, 16) == 0);
    CHECK(events(6000, 5000, ev, 16) == 0); /* an empty or reversed window */

    /* The four events of train 1 in order. Front at D: 5000 + 13333.33; at D+I: 5000 + 13733.33;
     * rear at D+I: front = 532000, 5000 + 17733.33. */
    n = events(0, 29999, ev, 16);
    CHECK(n == 4);
    CHECK(ev[0].kind == LG_PLANT_EV_STRIKE_IN && ev[0].t_ms == 5000);
    CHECK(ev[1].kind == LG_PLANT_EV_ISLAND_ENTER && ev[1].t_ms == 18333);
    CHECK(ev[2].kind == LG_PLANT_EV_ISLAND_EXIT && ev[2].t_ms == 18733);
    CHECK(ev[3].kind == LG_PLANT_EV_CLEARED && ev[3].t_ms == 22733);

    /* Everything of both trains, sorted by time. */
    n = events(0, 100000, ev, 16);
    CHECK(n == 8);
    {
        size_t i;
        for (i = 1; i < n; i++) {
            CHECK(ev[i - 1].t_ms <= ev[i].t_ms);
        }
    }
    CHECK(ev[4].train_id == 2 && ev[4].kind == LG_PLANT_EV_STRIKE_IN && ev[4].t_ms == 30000);

    /* A small output buffer gets the earliest events and never overflows. */
    n = events(0, 100000, ev, 3);
    CHECK(n == 3 && ev[2].kind == LG_PLANT_EV_ISLAND_EXIT);
    CHECK(events(0, 100000, ev, 0) == 0);

    /* Sliding windows of 50 ms (the plant process does this) see every event exactly once. */
    {
        uint32_t t;
        size_t total = 0;

        for (t = 0; t < 100000; t += 50) {
            total += events(t, t + 50, ev, 16);
        }
        CHECK(total == 8);
    }
}

static void test_event_at_time_zero(void) {
    lg_plant_event_t ev[8];
    size_t n;

    reset();
    add_train(1, LG_DIR_WEST, 0, 30000, 120000, 8); /* strikes in at exactly t = 0 */
    start();
    n = lg_plant_events(&plant, -1, 0, ev, 8);
    CHECK(n == 1 && ev[0].kind == LG_PLANT_EV_STRIKE_IN && ev[0].t_ms == 0 && ev[0].train_id == 1);
    CHECK(lg_plant_events(&plant, 0, 0, ev, 8) == 0);   /* (0, 0] is empty: it was reported above */
    CHECK(lg_plant_events(&plant, 0, 100, ev, 8) == 0); /* and it is not reported twice */
    n = lg_plant_events(&plant, -1, 100000, ev, 8);
    CHECK(n == 4 && ev[0].t_ms == 0);
}

int main(void) {
    test_event_at_time_zero();
    test_approach_edges();
    test_direction_awareness();
    test_island_edges();
    test_axle_counts();
    test_east_train_mirrors();
    test_two_trains();
    test_short_and_slow_trains();
    test_unknown_inputs();
    test_maximum_axles();
    test_far_future();
    test_counts_and_text();
    test_events();
    LG_TEST_DONE("test_plant_model");
}
