#ifndef SCH_H
#define SCH_H

#include <stdint.h>
#include <stdio.h>
#include "types.h"

#include "pq.h"

typedef struct SCH {
    uint64_t now; // in nanoseconds

    uint64_t current_bucket; // how many buckets we've gone through, not "bucket index"
    PQ ** buckets;

    PQ * slow_bucket;
    u64 * rand;
} SCH;

// take the more interesting case first
static const uint64_t P_BITS = 39;
static const uint64_t P_SPAN = (uint64_t)1 << P_BITS;
static const uint64_t P_MASK = P_SPAN - 1;

static const uint64_t FULL_SIZE_BITS = sizeof(uint64_t) * 8;

//"event"
static const uint64_t E_BITS = FULL_SIZE_BITS - P_BITS;

// 0000.1111
// or like inverse P mask?
static const uint64_t E_MASK = ((uint64_t)1 << E_BITS) - 1;

// number of fast buckets
static const uint8_t BUCKET_BITS = 3;
static const uint8_t SCH_BUCKETS = 1 << BUCKET_BITS;
static const uint8_t BUCKET_MASK = SCH_BUCKETS - 1;


static const uint64_t S_TO_NS = 1000000000;

static const uint8_t SLOW_CHECK_TYPE = 7;

//type bits
static const uint8_t T_BITS = 3;
static const uint8_t T_MASK = (1 << T_BITS) - 1;

static const uint32_t PARAM_BITS = FULL_SIZE_BITS - P_BITS - T_BITS;
static const uint32_t PARAM_MASK = (1 << PARAM_BITS) - 1;

SCH* sch_init(u64* rand);

// maybe take one uint64_t as a param?
void sch_schedule(SCH* sch, uint64_t event, uint64_t delta_ns);

uint64_t sch_pop(SCH* sch);

uint64_t sch_now_ns(SCH* sch);

void sch_free(SCH* sch);

#endif
