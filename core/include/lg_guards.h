#ifndef LG_GUARDS_H
#define LG_GUARDS_H

/*
 * Guard allowlist (README 3.1, Step 8). Portable. The list of guard ids that may acknowledge level 4
 * comes from the "guards" array of the crossing config. This is NOT authentication: the PIN hash in
 * LG_MSG_GUARD_ACK is not checked yet and any local process can send the message (Step 13).
 * A config without "guards" gives an empty list: nobody can acknowledge.
 */

#include <stddef.h>
#include <stdint.h>

#include "lg_json_mini.h"
#include "lg_proto.h"

#define LG_GUARDS_MAX 8

typedef struct {
    uint8_t count;
    char id[LG_GUARDS_MAX][LG_GUARD_ID_LEN]; /* NUL terminated, 1 to LG_GUARD_ID_LEN - 1 characters */
} lg_guards_t;

/*
 * Reads the optional "guards" array of the crossing object (strings of 1 to 7 characters from
 * A-Z a-z 0-9 _ -, at most LG_GUARDS_MAX, no duplicates). 0 on success; -1 with a reason in err
 * when the member has another type or an entry is invalid.
 */
int lg_guards_from_json(const lg_json_doc_t *doc, lg_guards_t *out, char *err, size_t errlen);

/*
 * True when `id` (a field of `field_len` bytes taken from a message) holds a NUL-terminated string
 * that is in the list. A field without a NUL inside field_len bytes never matches.
 */
int lg_guards_allowed(const lg_guards_t *g, const char *id, size_t field_len);

#endif
