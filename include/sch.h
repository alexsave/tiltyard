#ifndef SCH_H
#define SCH_H

#include <stdint.h>
#include <stdio.h>
#include "types.h"

#include "pq.h"
#include "constants.h"

typedef struct SCH {
    uint64_t now; // in nanoseconds

    uint64_t current_bucket; // how many buckets we've gone through, not "bucket index"
    PQ ** buckets;

    PQ * slow_bucket;
    u64 * rand;
} SCH;

SCH* sch_init(u64* rand);

// maybe take one uint64_t as a param?
void sch_schedule(SCH* sch, uint64_t event, uint64_t delta_ns);

uint64_t sch_pop(SCH* sch);

uint64_t sch_now_ns(SCH* sch);

void sch_free(SCH* sch);

#endif
