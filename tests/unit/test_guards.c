#include <string.h>

#include "lg_guards.h"
#include "lg_test.h"

static lg_json_doc_t g_doc;

static int load(const char *json, lg_guards_t *g, char *err, size_t errlen) {
    CHECK(lg_json_parse(&g_doc, json, strlen(json)) == LG_JSON_OK);
    return lg_guards_from_json(&g_doc, g, err, errlen);
}

int main(void) {
    lg_guards_t g;
    char err[96];
    char field[LG_GUARD_ID_LEN];

    /* No member: empty list, nobody may acknowledge. */
    CHECK(load("{\"strike_in_m\": 400}", &g, err, sizeof err) == 0);
    CHECK(g.count == 0 && !lg_guards_allowed(&g, "G01", 4));

    CHECK(load("{\"guards\": [\"G01\", \"night-2\"]}", &g, err, sizeof err) == 0);
    CHECK(g.count == 2);
    CHECK(lg_guards_allowed(&g, "G01", 4) && lg_guards_allowed(&g, "night-2", 8));
    CHECK(!lg_guards_allowed(&g, "G02", 4) && !lg_guards_allowed(&g, "g01", 4) && !lg_guards_allowed(&g, "", 1));
    CHECK(!lg_guards_allowed(&g, "G0", 3) && !lg_guards_allowed(&g, "G011", 5));
    CHECK(!lg_guards_allowed(&g, NULL, 4) && !lg_guards_allowed(NULL, "G01", 4));

    /* A message field is only trusted when it holds a NUL inside its own bytes. */
    memcpy(field, "G01XXXXX", 8);
    CHECK(!lg_guards_allowed(&g, field, sizeof field));
    memcpy(field, "G01\0XXXX", 8);
    CHECK(lg_guards_allowed(&g, field, sizeof field));
    memcpy(field, "G01\0", 4);
    CHECK(!lg_guards_allowed(&g, field, 3)); /* the NUL lies outside the 3 bytes handed in */

    /* Refused configurations. */
    CHECK(load("{\"guards\": \"G01\"}", &g, err, sizeof err) == -1 && strstr(err, "array") != NULL);
    CHECK(load("{\"guards\": [1]}", &g, err, sizeof err) == -1);
    CHECK(load("{\"guards\": [\"\"]}", &g, err, sizeof err) == -1);
    CHECK(load("{\"guards\": [\"ABCDEFGH\"]}", &g, err, sizeof err) == -1); /* 8 characters */
    CHECK(load("{\"guards\": [\"ABCDEFG\"]}", &g, err, sizeof err) == 0);   /* 7 is the longest */
    CHECK(load("{\"guards\": [\"a b\"]}", &g, err, sizeof err) == -1);
    CHECK(load("{\"guards\": [\"G01\", \"G01\"]}", &g, err, sizeof err) == -1 && strstr(err, "duplicate") != NULL);
    CHECK(load("{\"guards\": [\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\"]}", &g, err, sizeof err) == 0 && g.count == 8);
    CHECK(load("{\"guards\": [\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\"]}", &g, err, sizeof err) == -1);
    CHECK(load("[1]", &g, err, sizeof err) == -1);
    CHECK(g.count == 0); /* a refused file leaves an empty list behind */
    CHECK(load("{\"guards\": [\"G01\", \"G02\", \"G01\"]}", &g, err, sizeof err) == -1);
    CHECK(g.count == 0 && !lg_guards_allowed(&g, "G01", 4)); /* not even the entries before the bad one */

    LG_TEST_DONE("test_guards");
}
