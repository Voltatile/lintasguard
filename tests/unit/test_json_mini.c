#include <stdio.h>
#include <string.h>

#include "lg_json_mini.h"
#include "lg_test.h"

static lg_json_doc_t doc;

static int parse_str(const char *s) {
    return lg_json_parse(&doc, s, strlen(s));
}

static void test_scalars(void) {
    const lg_json_node_t *r;

    CHECK(parse_str("42") == LG_JSON_OK);
    r = lg_json_root(&doc);
    CHECK(r != NULL && r->type == LG_JSON_NUMBER && r->milli == 42000);

    CHECK(parse_str("  -0.25 ") == LG_JSON_OK);
    CHECK(lg_json_root(&doc)->milli == -250);
    CHECK(parse_str("12.5") == LG_JSON_OK && lg_json_root(&doc)->milli == 12500);
    CHECK(parse_str("0") == LG_JSON_OK && lg_json_root(&doc)->milli == 0);
    CHECK(parse_str("0.001") == LG_JSON_OK && lg_json_root(&doc)->milli == 1);
    CHECK(parse_str("1.000") == LG_JSON_OK && lg_json_root(&doc)->milli == 1000);
    CHECK(parse_str("999999999999") == LG_JSON_OK && lg_json_root(&doc)->milli == 999999999999000LL);
    CHECK(parse_str("-7") == LG_JSON_OK && lg_json_root(&doc)->milli == -7000);

    CHECK(parse_str("true") == LG_JSON_OK && lg_json_root(&doc)->type == LG_JSON_BOOL &&
          lg_json_root(&doc)->boolean == 1);
    CHECK(parse_str("false") == LG_JSON_OK && lg_json_root(&doc)->boolean == 0);
    CHECK(parse_str("null") == LG_JSON_OK && lg_json_root(&doc)->type == LG_JSON_NULL);

    CHECK(parse_str("\"abc\"") == LG_JSON_OK && lg_json_root(&doc)->type == LG_JSON_STRING);
    CHECK(strcmp(doc.strs + lg_json_root(&doc)->str, "abc") == 0);
    CHECK(parse_str("\"a\\\"b\\\\c\\/d\\n\"") == LG_JSON_OK);
    CHECK(strcmp(doc.strs + lg_json_root(&doc)->str, "a\"b\\c/d\n") == 0);
    CHECK(parse_str("\"\"") == LG_JSON_OK && doc.strs[lg_json_root(&doc)->str] == '\0');
    CHECK(parse_str("\"caf\xc3\xa9\"") == LG_JSON_OK); /* raw UTF-8 is passed through */
}

static void test_structures(void) {
    const lg_json_node_t *o;
    const lg_json_node_t *a;
    const char *s = NULL;
    int64_t v = 0;

    CHECK(parse_str("{\"name\":\"normal_day\",\"n\":3,\"trains\":[{\"id\":1},{\"id\":2},{\"id\":3}],"
                    "\"nested\":{\"x\":{\"y\":true}},\"empty_a\":[],\"empty_o\":{}}") == LG_JSON_OK);
    o = lg_json_root(&doc);
    CHECK(o->type == LG_JSON_OBJECT && o->count == 6);
    CHECK(lg_json_get_str(&doc, o, "name", &s) == 0 && strcmp(s, "normal_day") == 0);
    CHECK(lg_json_get_milli(&doc, o, "n", &v) == 0 && v == 3000);

    a = lg_json_get(&doc, o, "trains");
    CHECK(a != NULL && a->type == LG_JSON_ARRAY && a->count == 3);
    CHECK(lg_json_get_milli(&doc, lg_json_at(&doc, a, 0), "id", &v) == 0 && v == 1000);
    CHECK(lg_json_get_milli(&doc, lg_json_at(&doc, a, 2), "id", &v) == 0 && v == 3000);
    CHECK(lg_json_at(&doc, a, 3) == NULL);
    CHECK(lg_json_at(&doc, o, 0) == NULL); /* not an array */

    CHECK(lg_json_get(&doc, lg_json_get(&doc, lg_json_get(&doc, o, "nested"), "x"), "y")->boolean == 1);
    CHECK(lg_json_get(&doc, o, "empty_a")->count == 0 && lg_json_at(&doc, lg_json_get(&doc, o, "empty_a"), 0) == NULL);
    CHECK(lg_json_get(&doc, o, "empty_o")->count == 0);

    /* Missing member, wrong type, null arguments. */
    CHECK(lg_json_get(&doc, o, "missing") == NULL);
    CHECK(lg_json_get(&doc, a, "id") == NULL); /* not an object */
    CHECK(lg_json_get(&doc, NULL, "x") == NULL && lg_json_get(&doc, o, NULL) == NULL);
    CHECK(lg_json_get_milli(&doc, o, "name", &v) == -1); /* a string, not a number */
    CHECK(lg_json_get_str(&doc, o, "n", &s) == -1);      /* a number, not a string */
    CHECK(lg_json_get_milli(&doc, o, "missing", &v) == -1);

    CHECK(parse_str("[1,2 , 3]\n") == LG_JSON_OK && lg_json_root(&doc)->count == 3);
    CHECK(parse_str("[[],[[]],{}]") == LG_JSON_OK);
    CHECK(parse_str(" \t\r\n{ \"a\" : 1 , \"b\" : [ ] } \n") == LG_JSON_OK);

    /* Keys with equal prefixes and values with the same text are not confused. */
    CHECK(parse_str("{\"ab\":1,\"a\":2,\"abc\":3}") == LG_JSON_OK);
    CHECK(lg_json_get_milli(&doc, lg_json_root(&doc), "a", &v) == 0 && v == 2000);
    CHECK(lg_json_get_milli(&doc, lg_json_root(&doc), "abc", &v) == 0 && v == 3000);
}

static void test_malformed(void) {
    static const char *bad[] = {
        "", "   ", "{", "}", "[", "]", "[1,]", "[,1]", "[1 2]", "{\"a\":}", "{\"a\" 1}", "{\"a\":1,}",
        "{,}", "{\"a\":1 \"b\":2}", "{'a':1}", "{a:1}", "{1:2}", "[1}", "{\"a\":1]", "tru", "nul",
        "fals", "True", "NULL", "\"abc", "\"abc\\\"", "\"\\u0041\"", "\"\\x\"", "\"a\nb\"",
        "\"a\x01\"", "01", "00", "1.", ".5", "-", "-.5", "+1", "1e3", "1E3", "1.2345", "1.0e0",
        "1234567890123", "--1", "0x10", "[1]x", "{} {}", "1 2", "\"a\" \"b\"", "[", "[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]",
        "{\"a\":1,\"a\":2}", "{\"a\":1,\"b\":2,\"a\":3}", "NaN", "Infinity", "-Infinity", "[1,2,3",
    };
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = parse_str(bad[i]);

        if (rc == LG_JSON_OK) {
            printf("accepted malformed input #%zu: %s\n", i, bad[i]);
        }
        CHECK(rc != LG_JSON_OK);
        CHECK(lg_json_root(&doc) == NULL); /* nothing half-parsed is visible */
    }

    CHECK(parse_str("{\"a\":1,\"a\":2}") == LG_JSON_ERR_DUPLICATE);
    CHECK(parse_str("1.2345") == LG_JSON_ERR_NUMBER);
    CHECK(parse_str("[1]x") == LG_JSON_ERR_TRAILING);
    CHECK(parse_str("[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]") == LG_JSON_ERR_DEPTH);
    CHECK(lg_json_parse(NULL, "1", 1) == LG_JSON_ERR_ARGS);
    CHECK(lg_json_parse(&doc, NULL, 3) == LG_JSON_ERR_ARGS);
}

static void test_limits(void) {
    static char big[LG_JSON_MAX_NODES * 4 + 64];
    size_t n = 0;
    size_t i;

    /* Exactly the node limit is accepted, one more is refused. The array node counts as one. */
    big[n++] = '[';
    for (i = 0; i < LG_JSON_MAX_NODES - 1; i++) {
        if (i > 0) {
            big[n++] = ',';
        }
        big[n++] = '1';
    }
    big[n++] = ']';
    CHECK(lg_json_parse(&doc, big, n) == LG_JSON_OK && lg_json_root(&doc)->count == LG_JSON_MAX_NODES - 1);

    n = 0;
    big[n++] = '[';
    for (i = 0; i < LG_JSON_MAX_NODES; i++) {
        if (i > 0) {
            big[n++] = ',';
        }
        big[n++] = '1';
    }
    big[n++] = ']';
    CHECK(lg_json_parse(&doc, big, n) == LG_JSON_ERR_NODES);

    /* String pool: one long string up to the pool limit is fine, beyond it is refused. */
    {
        static char text[LG_JSON_MAX_STR + 16];

        n = 0;
        text[n++] = '"';
        for (i = 0; i < LG_JSON_MAX_STR - 1; i++) {
            text[n++] = 'a';
        }
        text[n++] = '"';
        CHECK(lg_json_parse(&doc, text, n) == LG_JSON_OK);
        n = 0;
        text[n++] = '"';
        for (i = 0; i < LG_JSON_MAX_STR; i++) {
            text[n++] = 'a';
        }
        text[n++] = '"';
        CHECK(lg_json_parse(&doc, text, n) == LG_JSON_ERR_STRINGS);
    }

    /* Depth: 16 levels is the limit. */
    {
        char text[64];

        n = 0;
        for (i = 0; i < LG_JSON_MAX_DEPTH; i++) {
            text[n++] = '[';
        }
        for (i = 0; i < LG_JSON_MAX_DEPTH; i++) {
            text[n++] = ']';
        }
        CHECK(lg_json_parse(&doc, text, n) == LG_JSON_OK);
    }
}

static void test_length_is_respected(void) {
    /* The parser must not read past len: no NUL terminator is required or looked for. */
    char buf[8] = {'[', '1', ',', '2', ']', 'x', 'y', 'z'};

    CHECK(lg_json_parse(&doc, buf, 5) == LG_JSON_OK && lg_json_root(&doc)->count == 2);
    CHECK(lg_json_parse(&doc, buf, 4) != LG_JSON_OK);
    CHECK(lg_json_parse(&doc, buf, 6) == LG_JSON_ERR_TRAILING);
    CHECK(lg_json_parse(&doc, buf, 0) != LG_JSON_OK);
}

static void test_reparse_resets(void) {
    CHECK(parse_str("{\"a\":1,\"b\":2}") == LG_JSON_OK);
    CHECK(parse_str("[5]") == LG_JSON_OK);
    CHECK(lg_json_root(&doc)->type == LG_JSON_ARRAY && doc.n_nodes == 2);
    CHECK(parse_str("{\"a\":") != LG_JSON_OK);
    CHECK(doc.n_nodes == 0 && doc.str_len == 0);
}

/* Deterministic byte mutations of a valid document: never crash, always a clean verdict. */
static void test_mutation_fuzz(void) {
    static const char seed_doc[] =
        "{\"name\":\"test\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t\":5.5,\"axles\":8},"
        "{\"id\":2,\"dir\":\"east\",\"t\":40,\"axles\":6}],\"on\":true,\"none\":null}";
    char buf[sizeof seed_doc];
    uint32_t rng = 0x12345678u;
    int accepted = 0;
    int i;

    for (i = 0; i < 30000; i++) {
        size_t len = sizeof seed_doc - 1;
        int flips = 1 + (int)(rng % 4);
        int f;

        memcpy(buf, seed_doc, sizeof seed_doc);
        for (f = 0; f < flips; f++) {
            rng = rng * 1664525u + 1013904223u;
            buf[(rng >> 8) % len] = (char)(rng >> 24);
        }
        rng = rng * 1664525u + 1013904223u;
        if ((rng & 7u) == 0) {
            len = (rng >> 8) % len; /* truncated copy */
        }
        {
            int rc = lg_json_parse(&doc, buf, len);

            if (rc == LG_JSON_OK) {
                accepted++;
                CHECK(lg_json_root(&doc) != NULL);
                CHECK(doc.n_nodes >= 1 && doc.n_nodes <= LG_JSON_MAX_NODES);
            } else {
                CHECK(rc < 0 && lg_json_root(&doc) == NULL);
            }
        }
    }
    CHECK(accepted > 0); /* the mutations are not so destructive that every case fails */
}

static void test_error_names(void) {
    CHECK(strcmp(lg_json_error_name(LG_JSON_OK), "ok") == 0);
    CHECK(strcmp(lg_json_error_name(LG_JSON_ERR_DUPLICATE), "duplicate key") == 0);
    CHECK(strcmp(lg_json_error_name(-99), "unknown error") == 0);
}

int main(void) {
    test_scalars();
    test_structures();
    test_malformed();
    test_limits();
    test_length_is_respected();
    test_reparse_resets();
    test_mutation_fuzz();
    test_error_names();
    LG_TEST_DONE("test_json_mini");
}
