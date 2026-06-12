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

    printf("TEST SCHEDULE 1\n");

    sch_schedule(sch, event, 100);

    printf("TEST POP 1\n");

    uint64_t next = sch_pop(sch);

    
    log_full(next);

    uint64_t now_ns = sch_now_ns(sch);
    printf("Now (last popped event): %llu or ~%llus\n", now_ns, now_ns/1000000000);

    printf("TEST POP 2\n");

    next = sch_pop(sch);
    log_full(next);

    now_ns = sch_now_ns(sch);
    printf("Now (last popped event): %llu or ~%llus\n", now_ns, now_ns/1000000000);

    uint64_t event2 = 6 << (E_BITS - T_BITS) | 1234;

    printf("TEST SCHEDULE FAR 1\n");

    // wait instead of this weird number let's schedule like a day out?
    // I think that's out of range...
    sch_schedule(sch, event2, (uint64_t)24*60*60*1000000000);

    //24 hours
    for(int i = 0; i < 25; i++){
        printf("\n");
        next = sch_pop(sch);
        log_full(next);
        now_ns = sch_now_ns(sch);
        printf("Now (last popped event): %llu or ~%llus\n", now_ns, now_ns/1000000000);
    }

    sch_free(sch);
}

#endif
