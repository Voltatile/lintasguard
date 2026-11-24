/* config/thresholds.json for lg-health (README 8.3, Step 11). */
#include <string.h>

#include "lg_health.h"
#include "lg_json_mini.h"
#include "lg_scenario.h"
#include "lg_test.h"

static lg_json_doc_t g_doc;

static int load_text(const char *text, lg_health_cfg_t *cfg) {
    char err[LG_ERR_LEN];

    *cfg = lg_health_cfg_default();
    if (lg_json_parse(&g_doc, text, strlen(text)) != LG_JSON_OK) {
        return -2;
    }
    return lg_health_cfg_from_json(&g_doc, cfg, err, sizeof err);
}

int main(void) {
    static char text[8192];
    size_t n;
    lg_health_cfg_t cfg;
    lg_health_cfg_t def = lg_health_cfg_default();
    char err[LG_ERR_LEN];

    /* The file in the repository holds exactly the defaults of lg_config.h. */
    CHECK(lg_read_file("config/thresholds.json", text, sizeof text, &n) == 0);
    CHECK(lg_json_parse(&g_doc, text, n) == LG_JSON_OK);
    cfg = def;
    CHECK(lg_health_cfg_from_json(&g_doc, &cfg, err, sizeof err) == 0);
    CHECK(memcmp(&cfg, &def, sizeof cfg) == 0);

    CHECK(load_text("{}", &cfg) == 0 && memcmp(&cfg, &def, sizeof cfg) == 0);
    CHECK(load_text("{\"noise_fail\": 25.5, \"poll_ms\": 300}", &cfg) == 0);
    CHECK(cfg.noise_fail_milli == 25500 && cfg.poll_ms == 300);
    CHECK(load_text("{\"ok_score\": 80}", &cfg) == 0 && cfg.ok_score == 80);

    CHECK(load_text("{\"noise_fial\": 30}", &cfg) == -1);      /* unknown key */
    CHECK(load_text("{\"poll_ms\": 5}", &cfg) == -1);           /* out of range */
    CHECK(load_text("{\"poll_ms\": 200.5}", &cfg) == -1);       /* not whole */
    CHECK(load_text("{\"poll_ms\": \"200\"}", &cfg) == -1);     /* not a number */
    CHECK(load_text("{\"dropout_warn_pm\": 400}", &cfg) == -1); /* warn after fail */
    CHECK(load_text("{\"battery_warn_mv\": 10900}", &cfg) == -1);
    CHECK(load_text("{\"poll_fast_ms\": 500}", &cfg) == -1);    /* faster must not be slower */
    CHECK(load_text("{\"noise_fail\": 4}", &cfg) == -1);        /* below base + learn max */
    CHECK(load_text("[1]", &cfg) == -1);
    CHECK(load_text("{\"cusum_h\": 0}", &cfg) == -1); /* review L2: an alarm level of 0 alarms always */
    /* A refused file changes nothing. */
    cfg = def;
    {
        const char *bad = "{\"poll_ms\": 300, \"bogus\": 1}";

        CHECK(lg_json_parse(&g_doc, bad, strlen(bad)) == LG_JSON_OK);
    }
    CHECK(lg_health_cfg_from_json(&g_doc, &cfg, err, sizeof err) == -1);
    CHECK(memcmp(&cfg, &def, sizeof cfg) == 0);
    CHECK(strstr(err, "bogus") != NULL);
    LG_TEST_DONE("test_health_cfg");
}
