#include <stdio.h>
#include <string.h>

#include "lg_plant_model.h"
#include "lg_test.h"

/*
 * Step 10: train positions for the dashboard (simulated world only; the controller never sees them).
 * Path coordinate as in lg_plant_model.h: 0 at the strike-in point of the train's side, the island
 * from strike_in to strike_in + island. A train is visible from the strike-in until its rear has
 * passed the strike-in point on the far side (2 * strike_in + island).
 */

static lg_crossing_cfg_t g_cfg;
static lg_scenario_t g_sc;
static lg_plant_t g_plant;

static void setup(void) {
    memset(&g_cfg, 0, sizeof g_cfg);
    memset(&g_sc, 0, sizeof g_sc);
    g_cfg.strike_in_mm = 400000;
    g_cfg.island_mm = 12000;
    g_cfg.sensor_zone_mm = 2000;
    (void)snprintf(g_sc.name, sizeof g_sc.name, "pos");
}

static void add(uint16_t id, uint8_t dir, uint32_t t_strike_ms, int32_t speed_mmps, int32_t length_mm) {
    lg_train_t *t = &g_sc.trains[g_sc.n_trains++];

    t->id = id;
    t->dir = dir;
    t->axles = 8;
    t->t_strike_ms = t_strike_ms;
    t->speed_mmps = speed_mmps;
    t->length_mm = length_mm;
}

static void test_positions(void) {
    lg_plant_pos_t pos[4];
    size_t n;

    setup();
    add(1, LG_DIR_WEST, 10000, 25000, 120000);
    add(2, LG_DIR_EAST, 20000, 20000, 150000);
    lg_plant_init(&g_plant, &g_cfg, &g_sc);

    CHECK(lg_plant_positions(&g_plant, 10000, pos, 4) == 0); /* front exactly at the strike-in: not yet */
    n = lg_plant_positions(&g_plant, 12000, pos, 4);
    CHECK(n == 1);
    CHECK(pos[0].id == 1 && pos[0].dir == LG_DIR_WEST);
    CHECK(pos[0].front_mm == 50000 && pos[0].rear_mm == 50000 - 120000);

    n = lg_plant_positions(&g_plant, 30000, pos, 4);
    CHECK(n == 2);
    CHECK(pos[0].id == 1 && pos[0].front_mm == 500000 && pos[0].rear_mm == 380000);
    CHECK(pos[1].id == 2 && pos[1].dir == LG_DIR_EAST && pos[1].front_mm == 200000);

    /* Train 1 is gone once its rear has passed 2 * 400000 + 12000 = 812000 (front 932000, t = 47280). */
    n = lg_plant_positions(&g_plant, 47279, pos, 4);
    CHECK(n == 2 && pos[0].id == 1);
    n = lg_plant_positions(&g_plant, 47281, pos, 4);
    CHECK(n == 1 && pos[0].id == 2);

    /* max limits the output, the order is the scenario order. */
    n = lg_plant_positions(&g_plant, 30000, pos, 1);
    CHECK(n == 1 && pos[0].id == 1);
    CHECK(lg_plant_positions(&g_plant, 30000, pos, 0) == 0);
    CHECK(lg_plant_positions(&g_plant, 30000, NULL, 4) == 0);
}

static void test_text_lines(void) {
    char buf[512];
    char small[90];
    size_t n;

    setup();
    add(1, LG_DIR_WEST, 10000, 25000, 120000);
    add(2, LG_DIR_EAST, 20000, 20000, 150000);
    lg_plant_init(&g_plant, &g_cfg, &g_sc);

    /* No visible train: exactly the old single line. */
    n = lg_plant_text(&g_plant, 0, buf, sizeof buf);
    CHECK(strcmp(buf, "t_ms=0 scenario=pos trains=2 waiting=2 active=0 finished=0 island=0\n") == 0);
    CHECK(n == strlen(buf));

    n = lg_plant_text(&g_plant, 30000, buf, sizeof buf);
    CHECK(n == strlen(buf) && buf[n - 1] == '\n');
    CHECK(strstr(buf, "t_ms=30000 ") == buf);
    CHECK(strstr(buf, "\ntrain id=1 dir=west front_mm=500000 rear_mm=380000\n") != NULL);
    CHECK(strstr(buf, "\ntrain id=2 dir=east front_mm=200000 rear_mm=50000\n") != NULL);

    /* A buffer with room for the first line only: no half train line. */
    n = lg_plant_text(&g_plant, 30000, small, sizeof small);
    CHECK(n == strlen(small) && small[n - 1] == '\n');
    CHECK(strstr(small, "train id=") == NULL || small[n - 1] == '\n');
    {
        const char *last = strrchr(small, '\n');
        const char *p = small;
        int lines = 0;

        while ((p = strchr(p, '\n')) != NULL) {
            lines++;
            p++;
        }
        CHECK(last == small + n - 1);
        CHECK(lines >= 1);
    }
}

int main(void) {
    test_positions();
    test_text_lines();
    LG_TEST_DONE("test_plant_positions");
}
