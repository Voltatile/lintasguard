#ifndef LG_TEST_H
#define LG_TEST_H

/* Minimal test helpers: count failures, print one PASS/FAIL line per test binary. */

#include <stdio.h>

static int lg_test_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                               \
            lg_test_failures++;                                                                    \
        }                                                                                          \
    } while (0)

#define LG_TEST_DONE(name)                                                                         \
    do {                                                                                           \
        if (lg_test_failures == 0) {                                                               \
            puts("PASS: " name);                                                                   \
            return 0;                                                                              \
        }                                                                                          \
        printf("FAIL: %s (%d check(s) failed)\n", name, lg_test_failures);                         \
        return 1;                                                                                  \
    } while (0)

#endif
