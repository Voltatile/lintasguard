#include <stdio.h>

#include "lg_config.h"
#include "lg_names.h"
#include "lg_proto.h"
#include "lg_version.h"

int main(void) {
    printf("lg-stub %s proto=%d hdr=%zu event=%zu gate=%s\n", lg_version(), LG_PROTO_VERSION,
           sizeof(lg_hdr_t), sizeof(lg_event_t), LG_DEV_GATE);
    return 0;
}
