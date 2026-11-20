#include <stdio.h>
#include <string.h>

#include "lg_version.h"

int main(void) {
    if (strlen(lg_version()) == 0) {
        puts("FAIL: test_smoke (empty version)");
        return 1;
    }
    puts("PASS: test_smoke");
    return 0;
}
