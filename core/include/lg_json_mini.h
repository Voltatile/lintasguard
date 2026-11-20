#ifndef LG_JSON_MINI_H
#define LG_JSON_MINI_H

/*
 * Minimal strict JSON parser for the config and scenario files. Portable, no allocation, no
 * floating point: the caller owns the lg_json_doc_t (about 50 KB, keep it static).
 *
 * What it accepts: objects, arrays, strings, numbers, true, false, null. Anything else is an
 * error and nothing is guessed:
 *  - numbers are integers or decimals with at most 3 fractional digits (stored as milli units,
 *    so 12.5 is 12500); no exponent, no leading plus, no leading zeros, at most 12 integer digits;
 *  - strings may use the escapes \" \\ \/ \b \f \n \r \t; \u escapes and raw control characters
 *    are rejected;
 *  - duplicate keys in one object, trailing commas, trailing text and nesting deeper than
 *    LG_JSON_MAX_DEPTH are rejected.
 */

#include <stddef.h>
#include <stdint.h>

#define LG_JSON_MAX_NODES 1024
#define LG_JSON_MAX_STR 8192
#define LG_JSON_MAX_DEPTH 16

#define LG_JSON_OK 0
#define LG_JSON_ERR_SYNTAX (-1)
#define LG_JSON_ERR_NODES (-2)   /* more than LG_JSON_MAX_NODES values */
#define LG_JSON_ERR_STRINGS (-3) /* string pool full */
#define LG_JSON_ERR_DEPTH (-4)
#define LG_JSON_ERR_TRAILING (-5)
#define LG_JSON_ERR_NUMBER (-6)
#define LG_JSON_ERR_DUPLICATE (-7)
#define LG_JSON_ERR_ARGS (-8)

typedef enum {
    LG_JSON_NULL = 0,
    LG_JSON_BOOL = 1,
    LG_JSON_NUMBER = 2,
    LG_JSON_STRING = 3,
    LG_JSON_ARRAY = 4,
    LG_JSON_OBJECT = 5
} lg_json_type_t;

typedef struct {
    uint8_t type;    /* lg_json_type_t */
    uint8_t boolean; /* for LG_JSON_BOOL */
    uint8_t reserved[2];
    int32_t first_child; /* index of the first child, -1 if none */
    int32_t next;        /* index of the next sibling, -1 if none */
    int32_t key;         /* offset of the member key in the string pool, -1 for non-members */
    int32_t str;         /* offset of the string value in the string pool */
    uint32_t count;      /* children of an array or object */
    int64_t milli;       /* number value times 1000 */
} lg_json_node_t;

typedef struct {
    lg_json_node_t nodes[LG_JSON_MAX_NODES];
    uint32_t n_nodes;
    char strs[LG_JSON_MAX_STR];
    uint32_t str_len;
} lg_json_doc_t;

/* Parses text[0..len) (no NUL terminator needed). Returns LG_JSON_OK or a negative error. */
int lg_json_parse(lg_json_doc_t *doc, const char *text, size_t len);

/* The root value, or NULL when nothing was parsed. */
const lg_json_node_t *lg_json_root(const lg_json_doc_t *doc);

/* Member of an object by key, or NULL (also NULL when obj is not an object). */
const lg_json_node_t *lg_json_get(const lg_json_doc_t *doc, const lg_json_node_t *obj,
                                  const char *key);

/* Element of an array by index, or NULL (also NULL when arr is not an array or idx is out of range). */
const lg_json_node_t *lg_json_at(const lg_json_doc_t *doc, const lg_json_node_t *arr, uint32_t idx);

/* Typed access to a member. 0 on success, -1 when the member is missing or has another type. */
int lg_json_get_milli(const lg_json_doc_t *doc, const lg_json_node_t *obj, const char *key,
                      int64_t *out);
int lg_json_get_str(const lg_json_doc_t *doc, const lg_json_node_t *obj, const char *key,
                    const char **out);

const char *lg_json_error_name(int err);

#endif
