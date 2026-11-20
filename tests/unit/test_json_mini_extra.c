#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lg_json_mini.h"
#include "lg_test.h"

/*
 * Extra tests for core/json_mini.c: pool and node limits at the exact boundary, number edge cases,
 * depth shapes, strict syntax, reads past len, and a seeded mutation and truncation fuzz that
 * checks the structural invariants of every successful parse.
 */

static lg_json_doc_t doc;
static lg_json_doc_t doc2;

static int parse(const char *s) {
    return lg_json_parse(&doc, s, strlen(s));
}

static int num(const char *s, int64_t *out) {
    int rc = parse(s);
    const lg_json_node_t *r = lg_json_root(&doc);

    if (rc == LG_JSON_OK && r != NULL && r->type == LG_JSON_NUMBER) {
        *out = r->milli;
    }
    return rc;
}

/* ---- structural invariants ------------------------------------------------------------------ */

static uint32_t g_visits;
static uint8_t g_seen[LG_JSON_MAX_NODES];

static int pool_string_ok(const lg_json_doc_t *d, int32_t off) {
    uint32_t i;

    if (off < 0 || (uint32_t)off >= d->str_len) {
        return 0;
    }
    for (i = (uint32_t)off; i < d->str_len; i++) {
        if (d->strs[i] == '\0') {
            return 1;
        }
    }
    return 0; /* not terminated inside the used pool */
}

static int walk(const lg_json_doc_t *d, int32_t idx, int depth, int in_object) {
    const lg_json_node_t *n;
    int32_t c;
    uint32_t chain = 0;

    if (idx < 0 || (uint32_t)idx >= d->n_nodes || depth > LG_JSON_MAX_DEPTH + 1 || g_seen[idx]) {
        return 0;
    }
    g_seen[idx] = 1;
    g_visits++;
    n = &d->nodes[idx];
    if (n->type > LG_JSON_OBJECT) {
        return 0;
    }
    if (in_object ? !pool_string_ok(d, n->key) : n->key != -1) {
        return 0;
    }
    if (n->type == LG_JSON_STRING && !pool_string_ok(d, n->str)) {
        return 0;
    }
    if (n->type != LG_JSON_ARRAY && n->type != LG_JSON_OBJECT) {
        return n->first_child == -1 && n->count == 0;
    }
    for (c = n->first_child; c != -1; c = d->nodes[c].next) {
        if (c < 0 || (uint32_t)c >= d->n_nodes) {
            return 0;
        }
        if (!walk(d, c, depth + 1, n->type == LG_JSON_OBJECT)) {
            return 0;
        }
        chain++;
        if (chain > LG_JSON_MAX_NODES) {
            return 0;
        }
    }
    return chain == n->count;
}

static int invariants(const lg_json_doc_t *d, int rc) {
    if (rc != LG_JSON_OK) {
        return d->n_nodes == 0 && d->str_len == 0 && lg_json_root(d) == NULL;
    }
    if (d->n_nodes < 1 || d->n_nodes > LG_JSON_MAX_NODES || d->str_len > LG_JSON_MAX_STR) {
        return 0;
    }
    if (d->str_len > 0 && d->strs[d->str_len - 1] != '\0') {
        return 0;
    }
    memset(g_seen, 0, sizeof g_seen);
    g_visits = 0;
    if (!walk(d, 0, 0, 0)) {
        return 0;
    }
    return g_visits == d->n_nodes; /* every node belongs to the tree: nothing leaked */
}

/* ---- numbers -------------------------------------------------------------------------------- */

static void test_numbers(void) {
    int64_t v = 12345;

    CHECK(num("-0", &v) == LG_JSON_OK && v == 0);
    CHECK(num("-0.000", &v) == LG_JSON_OK && v == 0);
    CHECK(num("0.000", &v) == LG_JSON_OK && v == 0);
    CHECK(num("-0.5", &v) == LG_JSON_OK && v == -500);
    CHECK(num("-12.345", &v) == LG_JSON_OK && v == -12345);
    CHECK(num("0.001", &v) == LG_JSON_OK && v == 1);
    CHECK(num("-0.001", &v) == LG_JSON_OK && v == -1);
    CHECK(num("0.01", &v) == LG_JSON_OK && v == 10);
    CHECK(num("7.1", &v) == LG_JSON_OK && v == 7100);
    CHECK(num("999999999999", &v) == LG_JSON_OK && v == 999999999999000LL);
    CHECK(num("-999999999999", &v) == LG_JSON_OK && v == -999999999999000LL);
    CHECK(num("999999999999.999", &v) == LG_JSON_OK && v == 999999999999999LL);
    CHECK(num("-999999999999.999", &v) == LG_JSON_OK && v == -999999999999999LL);
    CHECK(num("100000000000", &v) == LG_JSON_OK && v == 100000000000000LL);
    CHECK(parse("1000000000000") == LG_JSON_ERR_NUMBER);  /* 13 integer digits */
    CHECK(parse("-1000000000000") == LG_JSON_ERR_NUMBER);
    CHECK(parse("9999999999999999999999999999") == LG_JSON_ERR_NUMBER);
    CHECK(parse("0.0000") == LG_JSON_ERR_NUMBER); /* 4 fractional digits, even zeros */
    CHECK(parse("1.2345") == LG_JSON_ERR_NUMBER);
    CHECK(parse("0.0001") == LG_JSON_ERR_NUMBER);
    CHECK(parse("00") == LG_JSON_ERR_NUMBER);
    CHECK(parse("-00") == LG_JSON_ERR_NUMBER);
    CHECK(parse("01") == LG_JSON_ERR_NUMBER);
    CHECK(parse("-01") == LG_JSON_ERR_NUMBER);
    CHECK(parse("0.5.5") != LG_JSON_OK);
    CHECK(parse("-") == LG_JSON_ERR_NUMBER);
    CHECK(parse("--1") == LG_JSON_ERR_NUMBER);
    CHECK(parse("-a") == LG_JSON_ERR_NUMBER);
    CHECK(parse("1.") == LG_JSON_ERR_NUMBER);
    CHECK(parse("1.e3") == LG_JSON_ERR_NUMBER);
    CHECK(parse("-.5") == LG_JSON_ERR_NUMBER);
    CHECK(parse(".5") != LG_JSON_OK);
    CHECK(parse("+1") != LG_JSON_OK);
    CHECK(parse("1e0") == LG_JSON_ERR_NUMBER);
    CHECK(parse("1E0") == LG_JSON_ERR_NUMBER);
    CHECK(parse("0e0") == LG_JSON_ERR_NUMBER);
    CHECK(parse("1.5e") == LG_JSON_ERR_NUMBER);
    CHECK(parse("1.5E+3") == LG_JSON_ERR_NUMBER);
    CHECK(parse("0x10") != LG_JSON_OK);
    CHECK(parse("1x") != LG_JSON_OK);
    CHECK(parse("1 2") == LG_JSON_ERR_TRAILING);
    CHECK(parse("[1,-]") == LG_JSON_ERR_NUMBER);
    CHECK(parse("[1.]") == LG_JSON_ERR_NUMBER);
    CHECK(parse("Infinity") != LG_JSON_OK);
    CHECK(parse("NaN") != LG_JSON_OK);
    CHECK(parse("-Infinity") != LG_JSON_OK);
    CHECK(invariants(&doc, LG_JSON_ERR_NUMBER));
}

/* ---- pools ---------------------------------------------------------------------------------- */

static char big[200000];

static void test_string_pool_boundaries(void) {
    size_t n;
    size_t i;
    int k;

    /* One string value of 8191 bytes + NUL fills the pool exactly. */
    n = 0;
    big[n++] = '[';
    big[n++] = '"';
    for (i = 0; i < 8191; i++) {
        big[n++] = 'a';
    }
    big[n++] = '"';
    big[n++] = ']';
    CHECK(lg_json_parse(&doc, big, n) == LG_JSON_OK);
    CHECK(doc.str_len == 8192);
    CHECK(invariants(&doc, LG_JSON_OK));

    n = 0;
    big[n++] = '[';
    big[n++] = '"';
    for (i = 0; i < 8192; i++) {
        big[n++] = 'a';
    }
    big[n++] = '"';
    big[n++] = ']';
    CHECK(lg_json_parse(&doc, big, n) == LG_JSON_ERR_STRINGS);
    CHECK(invariants(&doc, LG_JSON_ERR_STRINGS));

    /* A key of 8191 bytes fills it as well. */
    n = 0;
    big[n++] = '{';
    big[n++] = '"';
    for (i = 0; i < 8191; i++) {
        big[n++] = 'k';
    }
    big[n++] = '"';
    big[n++] = ':';
    big[n++] = '1';
    big[n++] = '}';
    CHECK(lg_json_parse(&doc, big, n) == LG_JSON_OK);
    CHECK(invariants(&doc, LG_JSON_OK));
    n = 0;
    big[n++] = '{';
    big[n++] = '"';
    for (i = 0; i < 8192; i++) {
        big[n++] = 'k';
    }
    big[n++] = '"';
    big[n++] = ':';
    big[n++] = '1';
    big[n++] = '}';
    CHECK(lg_json_parse(&doc, big, n) == LG_JSON_ERR_STRINGS);

    /* Key and value compete: a + 1 + b + 1 == 8192 fits, one more does not. */
    for (k = 0; k < 2; k++) {
        size_t a = 4000;
        size_t b = 8192 - a - 2 + (size_t)k;

        n = 0;
        big[n++] = '{';
        big[n++] = '"';
        for (i = 0; i < a; i++) {
            big[n++] = 'k';
        }
        big[n++] = '"';
        big[n++] = ':';
        big[n++] = '"';
        for (i = 0; i < b; i++) {
            big[n++] = 'v';
        }
        big[n++] = '"';
        big[n++] = '}';
        CHECK(lg_json_parse(&doc, big, n) == (k == 0 ? LG_JSON_OK : LG_JSON_ERR_STRINGS));
        CHECK(invariants(&doc, k == 0 ? LG_JSON_OK : LG_JSON_ERR_STRINGS));
    }

    /* Many equal strings: 32 x 255 bytes (+NUL) is exactly 8192; one more byte anywhere is not. */
    for (k = 0; k < 3; k++) {
        int count = 32;
        int j;

        n = 0;
        big[n++] = '[';
        for (j = 0; j < count; j++) {
            size_t len = 255;

            if (k == 1 && j == count - 1) {
                len = 256; /* one byte too many in the last string */
            }
            if (k == 2 && j == 0) {
                len = 254;
            }
            if (j > 0) {
                big[n++] = ',';
            }
            big[n++] = '"';
            for (i = 0; i < len; i++) {
                big[n++] = 's';
            }
            big[n++] = '"';
        }
        big[n++] = ']';
        CHECK(lg_json_parse(&doc, big, n) == (k == 1 ? LG_JSON_ERR_STRINGS : LG_JSON_OK));
        CHECK(invariants(&doc, k == 1 ? LG_JSON_ERR_STRINGS : LG_JSON_OK));
    }

    /* Escapes cost one pool byte per decoded character, not two. */
    n = 0;
    big[n++] = '[';
    big[n++] = '"';
    for (i = 0; i < 4095; i++) {
        big[n++] = '\\';
        big[n++] = 'n';
    }
    big[n++] = '"';
    big[n++] = ']';
    CHECK(lg_json_parse(&doc, big, n) == LG_JSON_OK);
    CHECK(doc.str_len == 4096);
    CHECK(invariants(&doc, LG_JSON_OK));

    /* Strings only in the pool after a failed parse: the next parse starts clean. */
    CHECK(parse("[\"abc\"]") == LG_JSON_OK && doc.str_len == 4);
}

static void test_node_boundaries(void) {
    size_t n;
    int i;
    int k;

    /* 1023 numbers + the array = 1024 nodes: fits. One more: too many. */
    for (k = 0; k < 2; k++) {
        int count = 1023 + k;

        n = 0;
        big[n++] = '[';
        for (i = 0; i < count; i++) {
            if (i > 0) {
                big[n++] = ',';
            }
            big[n++] = '1';
        }
        big[n++] = ']';
        CHECK(lg_json_parse(&doc, big, n) == (k == 0 ? LG_JSON_OK : LG_JSON_ERR_NODES));
        CHECK(invariants(&doc, k == 0 ? LG_JSON_OK : LG_JSON_ERR_NODES));
        if (k == 0) {
            CHECK(doc.n_nodes == 1024 && doc.nodes[0].count == 1023);
        }
    }

    /* Objects with unique keys: 1023 members fits, 1024 does not. */
    for (k = 0; k < 2; k++) {
        int count = 1023 + k;

        n = 0;
        big[n++] = '{';
        for (i = 0; i < count; i++) {
            if (i > 0) {
                big[n++] = ',';
            }
            n += (size_t)sprintf(big + n, "\"k%d\":null", i);
        }
        big[n++] = '}';
        CHECK(lg_json_parse(&doc, big, n) == (k == 0 ? LG_JSON_OK : LG_JSON_ERR_NODES));
        CHECK(invariants(&doc, k == 0 ? LG_JSON_OK : LG_JSON_ERR_NODES));
        if (k == 0) {
            CHECK(lg_json_get(&doc, lg_json_root(&doc), "k1022") != NULL);
            CHECK(lg_json_get(&doc, lg_json_root(&doc), "k1023") == NULL);
        }
    }

    /* Nested shape: 16 levels with one scalar each, plus siblings: node count is exact. */
    n = 0;
    for (i = 0; i < 16; i++) {
        big[n++] = '[';
        big[n++] = '0';
        big[n++] = ',';
    }
    big[n++] = '0';
    for (i = 0; i < 16; i++) {
        big[n++] = ']';
    }
    CHECK(lg_json_parse(&doc, big, n) == LG_JSON_OK);
    CHECK(doc.n_nodes == 16 + 17);
    CHECK(invariants(&doc, LG_JSON_OK));

    /* A node limit hit inside a nested container leaves nothing behind. */
    n = 0;
    big[n++] = '[';
    for (i = 0; i < 1023; i++) {
        big[n++] = '[';
        big[n++] = ']';
        if (i < 1022) {
            big[n++] = ',';
        }
    }
    big[n++] = ']';
    CHECK(lg_json_parse(&doc, big, n) == LG_JSON_OK && doc.n_nodes == 1024);
    CHECK(invariants(&doc, LG_JSON_OK));
    big[n - 1] = ',';
    big[n++] = '[';
    big[n++] = ']';
    big[n++] = ']';
    CHECK(lg_json_parse(&doc, big, n) == LG_JSON_ERR_NODES);
    CHECK(invariants(&doc, LG_JSON_ERR_NODES));
}

static void test_depth(void) {
    char buf[200];
    int d;
    int shape;

    /* shape 0: arrays, 1: objects, 2: alternating. 16 levels fit, 17 do not. */
    for (shape = 0; shape < 3; shape++) {
        for (d = 14; d <= 18; d++) {
            size_t n = 0;
            int i;
            int rc;

            for (i = 0; i < d; i++) {
                int obj = shape == 1 || (shape == 2 && (i % 2) == 1);

                if (obj) {
                    n += (size_t)sprintf(buf + n, "{\"a\":");
                } else {
                    buf[n++] = '[';
                }
            }
            buf[n++] = '1';
            for (i = d - 1; i >= 0; i--) {
                int obj = shape == 1 || (shape == 2 && (i % 2) == 1);

                buf[n++] = obj ? '}' : ']';
            }
            rc = lg_json_parse(&doc, buf, n);
            if (d <= 16) {
                CHECK(rc == LG_JSON_OK);
            } else {
                CHECK(rc == LG_JSON_ERR_DEPTH);
            }
            CHECK(invariants(&doc, rc));
        }
    }
    /* Unclosed deep nesting is a depth error, not a crash or a syntax error past the limit. */
    memset(buf, '[', 100);
    CHECK(lg_json_parse(&doc, buf, 100) == LG_JSON_ERR_DEPTH);
    CHECK(invariants(&doc, LG_JSON_ERR_DEPTH));
    /* Empty containers at the depth limit. */
    CHECK(parse("[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]") == LG_JSON_OK);
    CHECK(parse("[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]") == LG_JSON_ERR_DEPTH);
}

/* ---- syntax --------------------------------------------------------------------------------- */

static void test_syntax(void) {
    static const char *const bad[] = {
        "",           " ",          "\t\n\r ",    "[",          "{",           "[1",         "[1,",
        "{\"a\"",     "{\"a\":",    "{\"a\":1",   "{\"a\":}",   "{,}",         "[,]",        "[1,]",
        "{\"a\":1,}", "[1 2]",      "{\"a\":1 \"b\":2}", "{1:2}", "{\"a\" 1}",   "{'a':1}",    "{a:1}",
        "tru",        "nul",        "fals",       "True",       "NULL",        "nulll",      "truee",
        "\"abc",      "\"abc\\",    "\"\\u0041\"", "\"\\x\"",   "\"\\",        "\"a\nb\"",   "\"a\tb\"",
        "\v1",        "\f1",        "\xEF\xBB\xBF{}", "\xFF",   "\x80",        "[1]]",       "[1]x",
        "{}{}",       "[][]",       "1,2",        "{\"a\":1}}", "[\"a\" \"b\"]", ":",          ",",
        "}",          "]",          "[}",         "{]",         "[1}",         "{\"a\":1]",  "// c\n1",
    };
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = parse(bad[i]);

        if (rc == LG_JSON_OK) {
            printf("accepted but must fail: index %u\n", (unsigned)i);
        }
        CHECK(rc != LG_JSON_OK);
        CHECK(invariants(&doc, rc));
    }
    CHECK(parse("") == LG_JSON_ERR_SYNTAX);
    CHECK(parse("   \n\t ") == LG_JSON_ERR_SYNTAX);
    CHECK(parse("[1]x") == LG_JSON_ERR_TRAILING);
    CHECK(parse("{}{}") == LG_JSON_ERR_TRAILING);
    CHECK(parse("{\"a\":1,\"a\":2}") == LG_JSON_ERR_DUPLICATE);

    /* Whitespace around a lone value and everywhere inside. */
    CHECK(parse("  \t\r\n 7 \n") == LG_JSON_OK);
    CHECK(parse(" [ 1 , { \"a\" : [ ] } , \"x\" , true , false , null ] ") == LG_JSON_OK);
    CHECK(doc.n_nodes == 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1);
    CHECK(invariants(&doc, LG_JSON_OK));
    CHECK(parse("[true,false,null]") == LG_JSON_OK);
    CHECK(lg_json_at(&doc, lg_json_root(&doc), 0)->boolean == 1);
    CHECK(lg_json_at(&doc, lg_json_root(&doc), 1)->boolean == 0);
    CHECK(lg_json_at(&doc, lg_json_root(&doc), 2)->type == LG_JSON_NULL);
    CHECK(parse("{}") == LG_JSON_OK && lg_json_root(&doc)->count == 0);
    CHECK(parse("[ ]") == LG_JSON_OK && lg_json_root(&doc)->count == 0);
}

static void test_nul_and_high_bytes(void) {
    static const char a[] = "\"a\0b\"";            /* NUL inside a string, inside len */
    static const char b[] = "1\0";                 /* NUL after the value, inside len */
    static const char c[] = "\0001";               /* NUL before the value */
    static const char d[] = "[\"a\xFF\xC3\x28z\"]"; /* high bytes inside a string */
    static const char e[] = "[\"x\"\0]";
    static const char f[] = "{\"a\0\":1}";
    const lg_json_node_t *r;
    const char *s = NULL;

    CHECK(lg_json_parse(&doc, a, sizeof a - 1) == LG_JSON_ERR_SYNTAX);
    CHECK(invariants(&doc, LG_JSON_ERR_SYNTAX));
    CHECK(lg_json_parse(&doc, b, sizeof b - 1) == LG_JSON_ERR_TRAILING);
    CHECK(lg_json_parse(&doc, b, 1) == LG_JSON_OK);
    CHECK(lg_json_parse(&doc, c, sizeof c - 1) == LG_JSON_ERR_SYNTAX);
    CHECK(lg_json_parse(&doc, e, sizeof e - 1) == LG_JSON_ERR_SYNTAX);
    CHECK(lg_json_parse(&doc, f, sizeof f - 1) == LG_JSON_ERR_SYNTAX);
    CHECK(invariants(&doc, LG_JSON_ERR_SYNTAX));
    CHECK(lg_json_parse(&doc, d, sizeof d - 1) == LG_JSON_OK);
    CHECK(invariants(&doc, LG_JSON_OK));
    r = lg_json_at(&doc, lg_json_root(&doc), 0);
    CHECK(r != NULL && r->type == LG_JSON_STRING);
    CHECK(r != NULL && strlen(doc.strs + r->str) == 5);
    /* A lone high byte at the top is a syntax error (also with a high-bit char as plain char). */
    CHECK(lg_json_parse(&doc, "\xFF", 1) == LG_JSON_ERR_SYNTAX);
    CHECK(lg_json_parse(&doc, "\x80", 1) == LG_JSON_ERR_SYNTAX);
    CHECK(lg_json_parse(&doc, "[\"\xFF\"]", 5) == LG_JSON_OK);
    (void)s;
}

static void test_strings_and_keys(void) {
    const char *s = NULL;
    const lg_json_node_t *r;

    /* Escaped and plain spellings of one key are the same key. */
    CHECK(parse("{\"a/b\":1,\"a\\/b\":2}") == LG_JSON_ERR_DUPLICATE);
    CHECK(parse("{\"\":1,\"\":2}") == LG_JSON_ERR_DUPLICATE);
    CHECK(parse("{\"a\":1,\"A\":2}") == LG_JSON_OK); /* case differs: not a duplicate */
    CHECK(parse("{\"a\":1,\"a \":2}") == LG_JSON_OK);
    CHECK(parse("{\"a\":{\"a\":1},\"b\":{\"a\":2}}") == LG_JSON_OK); /* other objects: fine */
    CHECK(parse("[{\"a\":1},{\"a\":2}]") == LG_JSON_OK);
    CHECK(parse("{\"a\":{\"x\":1,\"x\":2}}") == LG_JSON_ERR_DUPLICATE);
    CHECK(invariants(&doc, LG_JSON_ERR_DUPLICATE));
    /* A key that is a prefix of another is not a duplicate. */
    CHECK(parse("{\"ab\":1,\"a\":2,\"abc\":3}") == LG_JSON_OK);
    CHECK(lg_json_get(&doc, lg_json_root(&doc), "a")->milli == 2000);
    CHECK(lg_json_get(&doc, lg_json_root(&doc), "ab")->milli == 1000);
    CHECK(lg_json_get(&doc, lg_json_root(&doc), "abc")->milli == 3000);
    CHECK(lg_json_get(&doc, lg_json_root(&doc), "abcd") == NULL);
    CHECK(lg_json_get(&doc, lg_json_root(&doc), "") == NULL);
    CHECK(parse("{\"\":5}") == LG_JSON_OK);
    CHECK(lg_json_get(&doc, lg_json_root(&doc), "")->milli == 5000);

    CHECK(parse("[\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\"]") == LG_JSON_OK);
    r = lg_json_at(&doc, lg_json_root(&doc), 0);
    CHECK(r != NULL && strcmp(doc.strs + r->str, "a\"b\\c/d\b\f\n\r\t") == 0);
    CHECK(parse("[\"\\u0041\"]") == LG_JSON_ERR_SYNTAX);
    CHECK(parse("[\"\\a\"]") == LG_JSON_ERR_SYNTAX);
    CHECK(parse("[\"\\'\"]") == LG_JSON_ERR_SYNTAX);
    CHECK(parse("[\"\\ \"]") == LG_JSON_ERR_SYNTAX);

    /* Typed accessors on wrong shapes. */
    CHECK(parse("{\"n\":1,\"s\":\"x\",\"o\":{},\"a\":[]}") == LG_JSON_OK);
    {
        int64_t v = 777;
        const lg_json_node_t *root = lg_json_root(&doc);

        CHECK(lg_json_get_milli(&doc, root, "n", &v) == 0 && v == 1000);
        v = 777;
        CHECK(lg_json_get_milli(&doc, root, "s", &v) == -1 && v == 777);
        CHECK(lg_json_get_milli(&doc, root, "missing", &v) == -1);
        CHECK(lg_json_get_str(&doc, root, "s", &s) == 0 && strcmp(s, "x") == 0);
        CHECK(lg_json_get_str(&doc, root, "n", &s) == -1);
        CHECK(lg_json_get(&doc, lg_json_get(&doc, root, "n"), "x") == NULL);
        CHECK(lg_json_get(&doc, NULL, "x") == NULL);
        CHECK(lg_json_get(&doc, root, NULL) == NULL);
        CHECK(lg_json_at(&doc, root, 0) == NULL);
        CHECK(lg_json_at(&doc, lg_json_get(&doc, root, "a"), 0) == NULL);
        CHECK(lg_json_at(&doc, NULL, 0) == NULL);
    }
    CHECK(parse("[1,2,3]") == LG_JSON_OK);
    CHECK(lg_json_at(&doc, lg_json_root(&doc), 2)->milli == 3000);
    CHECK(lg_json_at(&doc, lg_json_root(&doc), 3) == NULL);
    CHECK(lg_json_at(&doc, lg_json_root(&doc), 0xFFFFFFFFu) == NULL);
    CHECK(lg_json_get(&doc, lg_json_root(&doc), "a") == NULL);
}

static void test_args_and_reset(void) {
    CHECK(lg_json_parse(NULL, "1", 1) == LG_JSON_ERR_ARGS);
    CHECK(lg_json_parse(&doc, NULL, 1) == LG_JSON_ERR_ARGS);
    CHECK(lg_json_parse(&doc, NULL, 0) == LG_JSON_ERR_SYNTAX);
    CHECK(invariants(&doc, LG_JSON_ERR_SYNTAX));
    CHECK(lg_json_parse(&doc, "123", 0) == LG_JSON_ERR_SYNTAX);
    CHECK(lg_json_parse(&doc, "1234", 2) == LG_JSON_OK && lg_json_root(&doc)->milli == 12000);
    CHECK(lg_json_parse(&doc, "[1,2,3]", 3) == LG_JSON_ERR_SYNTAX);
    CHECK(lg_json_parse(&doc, "[1,2,3]", 7) == LG_JSON_OK);
    CHECK(lg_json_parse(&doc, "tru", 3) == LG_JSON_ERR_SYNTAX);
    CHECK(lg_json_parse(&doc, "true", 4) == LG_JSON_OK);
    CHECK(lg_json_parse(&doc, "true", 3) == LG_JSON_ERR_SYNTAX);
    CHECK(lg_json_parse(&doc, "null", 4) == LG_JSON_OK);
    CHECK(lg_json_parse(&doc, "\"abc\"", 4) == LG_JSON_ERR_SYNTAX);
    CHECK(lg_json_parse(&doc, "\"abc\\\"", 5) == LG_JSON_ERR_SYNTAX);
    CHECK(lg_json_parse(&doc, "\"abc\\n\"", 6) == LG_JSON_ERR_SYNTAX);

    /* A good parse then a bad one: the old tree is not visible any more. */
    CHECK(parse("{\"a\":1}") == LG_JSON_OK && lg_json_root(&doc) != NULL);
    CHECK(parse("{\"a\":") == LG_JSON_ERR_SYNTAX);
    CHECK(lg_json_root(&doc) == NULL);
    CHECK(parse("[1]") == LG_JSON_OK && doc.n_nodes == 2 && doc.str_len == 0);
    CHECK(strcmp(lg_json_error_name(LG_JSON_ERR_ARGS), "bad arguments") == 0);
    CHECK(strcmp(lg_json_error_name(-99), "unknown error") == 0);
    CHECK(strcmp(lg_json_error_name(1), "unknown error") == 0);
}

/* ---- reading past len ------------------------------------------------------------------------ */

/*
 * The text is placed in a buffer followed by two different kinds of bytes. A parser that peeks
 * past len would give a different answer for the two paddings.
 */
static int same_with_both_paddings(const char *text, size_t len) {
    static char buf1[4096];
    static char buf2[4096];
    int rc1;
    int rc2;

    if (len + 64 > sizeof buf1) {
        return 1;
    }
    memcpy(buf1, text, len);
    memcpy(buf2, text, len);
    memset(buf1 + len, 'x', 64);
    memcpy(buf1 + len, "0123456789", 10);
    memset(buf2 + len, ' ', 64);
    memcpy(buf2 + len, "\"]}tru.e1E-,:", 13);
    rc1 = lg_json_parse(&doc, buf1, len);
    rc2 = lg_json_parse(&doc2, buf2, len);
    if (rc1 != rc2) {
        return 0;
    }
    if (rc1 == LG_JSON_OK) {
        return doc.n_nodes == doc2.n_nodes && doc.str_len == doc2.str_len &&
               memcmp(doc.nodes, doc2.nodes, doc.n_nodes * sizeof doc.nodes[0]) == 0 &&
               memcmp(doc.strs, doc2.strs, doc.str_len) == 0;
    }
    return 1;
}

static const char *const corpus[] = {
    "{\"strike_in_m\":400,\"island_m\":12.5,\"sensor_zone_m\":2}",
    "{\"name\":\"x\",\"trains\":[{\"id\":1,\"dir\":\"west\",\"t_strike_s\":5,\"speed_mps\":30,"
    "\"length_m\":120,\"axles\":8},{\"id\":2,\"dir\":\"east\",\"t_strike_s\":50.5,\"speed_mps\":20,"
    "\"length_m\":100.25,\"axles\":4}]}",
    "[1,-2.5,\"a\\nb\",true,false,null,[],{},[[1,[2]]],{\"k\":{\"j\":[]}}]",
    "  { \"a\" : [ 1 , 2 ] , \"b\" : { \"c\" : \"\\\"q\\\"\" } }  ",
    "[\"tab\\there\",-0,0.001,999999999999.999,\"\"]",
};
#define N_CORPUS (sizeof corpus / sizeof corpus[0])

static void test_prefixes_do_not_peek(void) {
    size_t c;
    size_t len;

    for (c = 0; c < N_CORPUS; c++) {
        size_t full = strlen(corpus[c]);

        for (len = 0; len <= full; len++) {
            int ok = same_with_both_paddings(corpus[c], len);

            if (!ok) {
                printf("padding changed the result: corpus %u len %u\n", (unsigned)c, (unsigned)len);
            }
            CHECK(ok);
            if (len < full) {
                /* A proper prefix of these documents is never a complete document, except when
                 * the cut falls after a whole top-level number or a leading-whitespace-only text. */
                int rc = lg_json_parse(&doc, corpus[c], len);

                CHECK(invariants(&doc, rc));
            }
        }
        CHECK(lg_json_parse(&doc, corpus[c], full) == LG_JSON_OK);
        CHECK(invariants(&doc, LG_JSON_OK));
    }
}

/* ---- fuzz ----------------------------------------------------------------------------------- */

static uint32_t rng_state = 0xC0FFEE11u;

static uint32_t rnd(void) {
    uint32_t x = rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static const char interesting[] = "{}[]\",:-.0123456789eE+\\ tnfu\t\n\r/a";

static void test_fuzz(void) {
    static char buf[8192];
    unsigned iter;
    unsigned ok_count = 0;
    unsigned fail_count = 0;
    unsigned bad_inv = 0;
    unsigned bad_pad = 0;

    printf("json fuzz seed 0xC0FFEE11\n");
    for (iter = 0; iter < 300000; iter++) {
        const char *src = corpus[rnd() % N_CORPUS];
        size_t len = strlen(src);
        unsigned nmut = 1 + rnd() % 4;
        unsigned m;
        int rc;

        memcpy(buf, src, len);
        for (m = 0; m < nmut; m++) {
            uint32_t kind = rnd() % 6;
            size_t pos = len > 0 ? rnd() % len : 0;

            switch (kind) {
            case 0: /* flip a byte to an interesting character */
                if (len > 0) {
                    buf[pos] = interesting[rnd() % (sizeof interesting - 1)];
                }
                break;
            case 1: /* random byte, including NUL and high bit */
                if (len > 0) {
                    buf[pos] = (char)(rnd() & 0xFF);
                }
                break;
            case 2: /* delete a byte */
                if (len > 0) {
                    memmove(buf + pos, buf + pos + 1, len - pos - 1);
                    len--;
                }
                break;
            case 3: /* insert an interesting byte */
                if (len + 1 < sizeof buf) {
                    memmove(buf + pos + 1, buf + pos, len - pos);
                    buf[pos] = interesting[rnd() % (sizeof interesting - 1)];
                    len++;
                }
                break;
            case 4: /* truncate */
                len = pos;
                break;
            default: /* duplicate a chunk */
                if (len > 0 && len * 2 < sizeof buf) {
                    size_t n = 1 + rnd() % (len - pos);

                    memmove(buf + pos + n, buf + pos, len - pos);
                    len += n;
                }
                break;
            }
        }
        rc = lg_json_parse(&doc, buf, len);
        if (rc == LG_JSON_OK) {
            ok_count++;
        } else {
            fail_count++;
        }
        if (!invariants(&doc, rc)) {
            bad_inv++;
        }
        if ((iter % 16) == 0 && !same_with_both_paddings(buf, len)) {
            bad_pad++;
        }
    }
    printf("json fuzz: %u accepted, %u rejected\n", ok_count, fail_count);
    CHECK(bad_inv == 0);
    CHECK(bad_pad == 0);
    CHECK(ok_count > 1000 && fail_count > 1000); /* the fuzz reaches both paths */
}

/* Every raw control character, 0x01..0x1F, is refused inside a string (also as a key). */
static void test_raw_control_chars(void) {
    char text[8];
    int c;

    for (c = 1; c < 0x20; c++) {
        text[0] = '"';
        text[1] = (char)c;
        text[2] = '"';
        CHECK(lg_json_parse(&doc, text, 3) == LG_JSON_ERR_SYNTAX);
        text[0] = '{';
        text[1] = '"';
        text[2] = (char)c;
        text[3] = '"';
        text[4] = ':';
        text[5] = '1';
        text[6] = '}';
        CHECK(lg_json_parse(&doc, text, 7) == LG_JSON_ERR_SYNTAX);
    }
    CHECK(lg_json_parse(&doc, "\" \"", 3) == LG_JSON_OK); /* 0x20 itself is fine */
}

int main(void) {
    test_raw_control_chars();
    test_numbers();
    test_string_pool_boundaries();
    test_node_boundaries();
    test_depth();
    test_syntax();
    test_nul_and_high_bytes();
    test_strings_and_keys();
    test_args_and_reset();
    test_prefixes_do_not_peek();
    test_fuzz();
    LG_TEST_DONE("test_json_mini_extra");
}
