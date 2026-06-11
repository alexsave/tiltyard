#ifndef TEST_SCH_H
#define TEST_SCH_H

#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#include "rand.h"
#include "pq.h"
#include "sch.h"

void log_full(uint64_t raw) {
    printf("raw: %llu, priority: %llu, type: %llu, params: %llu\n", 
        raw, 
        raw >> E_BITS, 
        (raw >> (E_BITS-T_BITS)) & T_MASK, 
        raw & PARAM_MASK);
}

void test_sch() {

    SCH* sch = sch_init();

    uint64_t event = 7 << (E_BITS - T_BITS) | 1000;

    sch_schedule(sch, event, 100);

    uint64_t next = sch_pop(sch);
    
    log_full(next);

    next = sch_pop(sch);
    log_full(next);

    //magic
    sch_free(sch);
}

#endif
