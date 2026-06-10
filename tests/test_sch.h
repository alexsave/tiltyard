#ifndef TEST_SCH_H
#define TEST_SCH_H

#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#include "rand.h"
#include "pq.h"
#include "sch.h"

void test_sch() {

    SCH* sch = sch_init();

    uint64_t event = 7 << (E_BITS - T_BITS) | 1000;

    sch_schedule_fast(sch, event, 100);

    uint64_t scheduled = sch_pop(sch);
    printf("%llu\n", scheduled);
    //magic
    sch_free(sch);
}

#endif
