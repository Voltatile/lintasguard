#include "lg_guards.h"

#include <stdio.h>
#include <string.h>

static int fail(char *err, size_t errlen, const char *msg) {
    if (err != NULL && errlen > 0) {
        (void)snprintf(err, errlen, "%s", msg);
    }
    return -1;
}

static int valid_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int parse(const lg_json_doc_t *doc, lg_guards_t *out, char *err, size_t errlen) {
    const lg_json_node_t *root = lg_json_root(doc);
    const lg_json_node_t *arr;
    uint32_t i;

    memset(out, 0, sizeof *out);
    if (root == NULL || root->type != LG_JSON_OBJECT) {
        return fail(err, errlen, "guards: the top level must be an object");
    }
    arr = lg_json_get(doc, root, "guards");
    if (arr == NULL) {
        return 0; /* no guards configured: nobody can acknowledge */
    }
    if (arr->type != LG_JSON_ARRAY) {
        return fail(err, errlen, "guards must be an array of strings");
    }
    if (arr->count > LG_GUARDS_MAX) {
        return fail(err, errlen, "guards: too many entries");
    }
    for (i = 0; i < arr->count; i++) {
        const lg_json_node_t *n = lg_json_at(doc, arr, i);
        const char *s;
        size_t len;
        size_t k;
        uint8_t j;

        if (n == NULL || n->type != LG_JSON_STRING) {
            return fail(err, errlen, "guards: every entry must be a string");
        }
        s = doc->strs + n->str;
        len = strlen(s);
        if (len < 1 || len >= LG_GUARD_ID_LEN) {
            return fail(err, errlen, "guards: an id must have 1 to 7 characters");
        }
        for (k = 0; k < len; k++) {
            if (!valid_char(s[k])) {
                return fail(err, errlen, "guards: an id may only use letters, digits, _ and -");
            }
        }
        for (j = 0; j < out->count; j++) {
            if (strcmp(out->id[j], s) == 0) {
                return fail(err, errlen, "guards: duplicate id");
            }
        }
        memcpy(out->id[out->count], s, len + 1);
        out->count++;
    }
    return 0;
}

int lg_guards_from_json(const lg_json_doc_t *doc, lg_guards_t *out, char *err, size_t errlen) {
    int rc = parse(doc, out, err, errlen);

    if (rc != 0) {
        memset(out, 0, sizeof *out); /* a refused file never leaves a half-filled list behind */
    }
    return rc;
}

int lg_guards_allowed(const lg_guards_t *g, const char *id, size_t field_len) {
    uint8_t i;

    if (g == NULL || id == NULL || memchr(id, '\0', field_len) == NULL) {
        return 0;
    }
    for (i = 0; i < g->count; i++) {
        if (strcmp(g->id[i], id) == 0) {
            return 1;
        }
    }
    return 0;
}
