#include "lg_json_mini.h"

#include <string.h>

typedef struct {
    lg_json_doc_t *doc;
    const char *p;
    const char *end;
} parser_t;

static int parse_value(parser_t *ps, int depth, int32_t *out);

static void skip_ws(parser_t *ps) {
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
        ps->p++;
    }
}

static int new_node(parser_t *ps, uint8_t type, int32_t *idx) {
    lg_json_node_t *n;

    if (ps->doc->n_nodes >= LG_JSON_MAX_NODES) {
        return LG_JSON_ERR_NODES;
    }
    *idx = (int32_t)ps->doc->n_nodes++;
    n = &ps->doc->nodes[*idx];
    memset(n, 0, sizeof *n);
    n->type = type;
    n->first_child = -1;
    n->next = -1;
    n->key = -1;
    n->str = -1;
    return LG_JSON_OK;
}

/* Parses a string (the opening quote is at ps->p) into the pool. Returns the offset in *off. */
static int parse_string(parser_t *ps, int32_t *off) {
    lg_json_doc_t *d = ps->doc;
    uint32_t start = d->str_len;

    ps->p++; /* opening quote */
    for (;;) {
        unsigned char c;

        if (ps->p >= ps->end) {
            return LG_JSON_ERR_SYNTAX; /* unterminated */
        }
        c = (unsigned char)*ps->p++;
        if (c == '"') {
            break;
        }
        if (c < 0x20) {
            return LG_JSON_ERR_SYNTAX; /* raw control character */
        }
        if (c == '\\') {
            if (ps->p >= ps->end) {
                return LG_JSON_ERR_SYNTAX;
            }
            switch (*ps->p++) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            default: return LG_JSON_ERR_SYNTAX; /* \u and unknown escapes */
            }
        }
        if (d->str_len + 2 > LG_JSON_MAX_STR) { /* this char and the terminating NUL */
            return LG_JSON_ERR_STRINGS;
        }
        d->strs[d->str_len++] = (char)c;
    }
    if (d->str_len + 1 > LG_JSON_MAX_STR) {
        return LG_JSON_ERR_STRINGS;
    }
    d->strs[d->str_len++] = '\0';
    *off = (int32_t)start;
    return LG_JSON_OK;
}

static int parse_number(parser_t *ps, int32_t idx) {
    int negative = 0;
    int int_digits = 0;
    int frac_digits = 0;
    int64_t whole = 0;
    int64_t frac = 0;

    if (ps->p < ps->end && *ps->p == '-') {
        negative = 1;
        ps->p++;
    }
    if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') {
        return LG_JSON_ERR_NUMBER;
    }
    if (*ps->p == '0') {
        ps->p++;
        int_digits = 1;
        if (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
            return LG_JSON_ERR_NUMBER; /* leading zero */
        }
    } else {
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
            if (++int_digits > 12) {
                return LG_JSON_ERR_NUMBER;
            }
            whole = whole * 10 + (*ps->p - '0');
            ps->p++;
        }
    }
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') {
            return LG_JSON_ERR_NUMBER;
        }
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
            if (++frac_digits > 3) {
                return LG_JSON_ERR_NUMBER; /* more precision than milli units: refuse, do not round */
            }
            frac = frac * 10 + (*ps->p - '0');
            ps->p++;
        }
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        return LG_JSON_ERR_NUMBER; /* no exponent */
    }
    while (frac_digits < 3) {
        frac *= 10;
        frac_digits++;
    }
    ps->doc->nodes[idx].milli = (negative ? -1 : 1) * (whole * 1000 + frac);
    return LG_JSON_OK;
}

static int match_word(parser_t *ps, const char *word) {
    size_t n = strlen(word);

    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0) {
        return 0;
    }
    ps->p += n;
    return 1;
}

static int key_equal(const lg_json_doc_t *d, int32_t key_off, int32_t other_off) {
    return strcmp(d->strs + key_off, d->strs + other_off) == 0;
}

static int parse_container(parser_t *ps, int depth, int is_object, int32_t *out) {
    char close = is_object ? '}' : ']';
    int32_t idx;
    int32_t last = -1;
    int rc;

    if (depth >= LG_JSON_MAX_DEPTH) {
        return LG_JSON_ERR_DEPTH;
    }
    rc = new_node(ps, is_object ? LG_JSON_OBJECT : LG_JSON_ARRAY, &idx);
    if (rc != LG_JSON_OK) {
        return rc;
    }
    ps->p++; /* the opening bracket */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == close) {
        ps->p++;
        *out = idx;
        return LG_JSON_OK;
    }
    for (;;) {
        int32_t key_off = -1;
        int32_t child;

        skip_ws(ps);
        if (is_object) {
            int32_t m;

            if (ps->p >= ps->end || *ps->p != '"') {
                return LG_JSON_ERR_SYNTAX;
            }
            rc = parse_string(ps, &key_off);
            if (rc != LG_JSON_OK) {
                return rc;
            }
            for (m = ps->doc->nodes[idx].first_child; m != -1; m = ps->doc->nodes[m].next) {
                if (key_equal(ps->doc, ps->doc->nodes[m].key, key_off)) {
                    return LG_JSON_ERR_DUPLICATE;
                }
            }
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                return LG_JSON_ERR_SYNTAX;
            }
            ps->p++;
            skip_ws(ps);
        }
        rc = parse_value(ps, depth + 1, &child);
        if (rc != LG_JSON_OK) {
            return rc;
        }
        ps->doc->nodes[child].key = key_off;
        if (last == -1) {
            ps->doc->nodes[idx].first_child = child;
        } else {
            ps->doc->nodes[last].next = child;
        }
        last = child;
        ps->doc->nodes[idx].count++;

        skip_ws(ps);
        if (ps->p >= ps->end) {
            return LG_JSON_ERR_SYNTAX;
        }
        if (*ps->p == ',') {
            ps->p++;
            continue; /* a value or member must follow: a trailing comma fails in the next round */
        }
        if (*ps->p == close) {
            ps->p++;
            break;
        }
        return LG_JSON_ERR_SYNTAX;
    }
    *out = idx;
    return LG_JSON_OK;
}

static int parse_value(parser_t *ps, int depth, int32_t *out) {
    int32_t idx;
    int rc;

    skip_ws(ps);
    if (ps->p >= ps->end) {
        return LG_JSON_ERR_SYNTAX;
    }
    switch (*ps->p) {
    case '{': return parse_container(ps, depth, 1, out);
    case '[': return parse_container(ps, depth, 0, out);
    case '"':
        rc = new_node(ps, LG_JSON_STRING, &idx);
        if (rc != LG_JSON_OK) {
            return rc;
        }
        rc = parse_string(ps, &ps->doc->nodes[idx].str);
        if (rc != LG_JSON_OK) {
            return rc;
        }
        *out = idx;
        return LG_JSON_OK;
    case 't':
    case 'f':
        rc = new_node(ps, LG_JSON_BOOL, &idx);
        if (rc != LG_JSON_OK) {
            return rc;
        }
        if (match_word(ps, "true")) {
            ps->doc->nodes[idx].boolean = 1;
        } else if (!match_word(ps, "false")) {
            return LG_JSON_ERR_SYNTAX;
        }
        *out = idx;
        return LG_JSON_OK;
    case 'n':
        rc = new_node(ps, LG_JSON_NULL, &idx);
        if (rc != LG_JSON_OK) {
            return rc;
        }
        if (!match_word(ps, "null")) {
            return LG_JSON_ERR_SYNTAX;
        }
        *out = idx;
        return LG_JSON_OK;
    default:
        if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9')) {
            rc = new_node(ps, LG_JSON_NUMBER, &idx);
            if (rc != LG_JSON_OK) {
                return rc;
            }
            rc = parse_number(ps, idx);
            if (rc != LG_JSON_OK) {
                return rc;
            }
            *out = idx;
            return LG_JSON_OK;
        }
        return LG_JSON_ERR_SYNTAX;
    }
}

int lg_json_parse(lg_json_doc_t *doc, const char *text, size_t len) {
    parser_t ps;
    int32_t root;
    int rc;

    if (doc == NULL || (text == NULL && len != 0)) {
        return LG_JSON_ERR_ARGS;
    }
    doc->n_nodes = 0;
    doc->str_len = 0;
    ps.doc = doc;
    ps.p = text;
    ps.end = text + len;

    rc = parse_value(&ps, 0, &root);
    if (rc == LG_JSON_OK) {
        skip_ws(&ps);
        if (ps.p != ps.end) {
            rc = LG_JSON_ERR_TRAILING;
        }
    }
    if (rc != LG_JSON_OK) {
        doc->n_nodes = 0; /* nothing half-parsed is ever visible */
        doc->str_len = 0;
        return rc;
    }
    /* The root is always node 0: it is the first node created. */
    (void)root;
    return LG_JSON_OK;
}

const lg_json_node_t *lg_json_root(const lg_json_doc_t *doc) {
    return doc->n_nodes > 0 ? &doc->nodes[0] : NULL;
}

const lg_json_node_t *lg_json_get(const lg_json_doc_t *doc, const lg_json_node_t *obj,
                                  const char *key) {
    int32_t i;

    if (obj == NULL || obj->type != LG_JSON_OBJECT || key == NULL) {
        return NULL;
    }
    for (i = obj->first_child; i != -1; i = doc->nodes[i].next) {
        if (doc->nodes[i].key >= 0 && strcmp(doc->strs + doc->nodes[i].key, key) == 0) {
            return &doc->nodes[i];
        }
    }
    return NULL;
}

const lg_json_node_t *lg_json_at(const lg_json_doc_t *doc, const lg_json_node_t *arr, uint32_t idx) {
    int32_t i;

    if (arr == NULL || arr->type != LG_JSON_ARRAY || idx >= arr->count) {
        return NULL;
    }
    i = arr->first_child;
    while (idx-- > 0 && i != -1) {
        i = doc->nodes[i].next;
    }
    return i == -1 ? NULL : &doc->nodes[i];
}

int lg_json_get_milli(const lg_json_doc_t *doc, const lg_json_node_t *obj, const char *key,
                      int64_t *out) {
    const lg_json_node_t *n = lg_json_get(doc, obj, key);

    if (n == NULL || n->type != LG_JSON_NUMBER) {
        return -1;
    }
    *out = n->milli;
    return 0;
}

int lg_json_get_str(const lg_json_doc_t *doc, const lg_json_node_t *obj, const char *key,
                    const char **out) {
    const lg_json_node_t *n = lg_json_get(doc, obj, key);

    if (n == NULL || n->type != LG_JSON_STRING) {
        return -1;
    }
    *out = doc->strs + n->str;
    return 0;
}

const char *lg_json_error_name(int err) {
    switch (err) {
    case LG_JSON_OK: return "ok";
    case LG_JSON_ERR_SYNTAX: return "syntax error";
    case LG_JSON_ERR_NODES: return "too many values";
    case LG_JSON_ERR_STRINGS: return "string pool full";
    case LG_JSON_ERR_DEPTH: return "nesting too deep";
    case LG_JSON_ERR_TRAILING: return "text after the value";
    case LG_JSON_ERR_NUMBER: return "invalid number";
    case LG_JSON_ERR_DUPLICATE: return "duplicate key";
    case LG_JSON_ERR_ARGS: return "bad arguments";
    default: return "unknown error";
    }
}
