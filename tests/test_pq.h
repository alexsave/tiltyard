#ifndef TEST_PQ_H
#define TEST_PQ_H

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "rand.h"
#include "pq.h"
#include "types.h"
#include "constants.h"

void test_bug(){

//5798218432512 scheduled in slow
//2899115507713 scheduled in slow
// kill event v
//3019915657215 scheduled in slow
    PQ* slow_bucket = pq_init();

    pq_push(slow_bucket, 5798218432512);
    pq_push(slow_bucket, 2899115507713);
    pq_push(slow_bucket, 3019915657215);

    // pq bucket is reversed

    printf("%llu %llu %llu %llu\n", slow_bucket->heap[0] & E_MASK, slow_bucket->heap[1] & E_MASK, slow_bucket->heap[2] & E_MASK, slow_bucket->heap[3] & E_MASK);
    u64 get = pq_pop(slow_bucket);
    printf("%llu\n", get);
    printf("%llu %llu %llu %llu\n", slow_bucket->heap[0] & E_MASK, slow_bucket->heap[1] & E_MASK, slow_bucket->heap[2] & E_MASK, slow_bucket->heap[3] & E_MASK);

    get = pq_pop(slow_bucket);
    printf("%llu\n", get);
    printf("%llu %llu %llu %llu\n", slow_bucket->heap[0] & E_MASK, slow_bucket->heap[1] & E_MASK, slow_bucket->heap[2] & E_MASK, slow_bucket->heap[3] & E_MASK);

    get = pq_pop(slow_bucket);
    printf("%llu\n", get);
    printf("%llu %llu %llu %llu\n", slow_bucket->heap[0] & E_MASK, slow_bucket->heap[1] & E_MASK, slow_bucket->heap[2] & E_MASK, slow_bucket->heap[3] & E_MASK);

    get = pq_pop(slow_bucket);
    printf("%llu\n", get);
    printf("%llu %llu %llu %llu\n", slow_bucket->heap[0] & E_MASK, slow_bucket->heap[1] & E_MASK, slow_bucket->heap[2] & E_MASK, slow_bucket->heap[3] & E_MASK);
    
    
}

void test_pq() {

    test_bug();

    // how do we use this again?
    // TDD: test driven development
    //hmmm
    //initialize it + clear it at the end

    PQ* pq = pq_init();

    assert(pq_is_empty(pq) == 1);

    printf("\nheap: ");
    for(int i = 0; i < 6; i++) printf("%llu ", pq->heap[i]);
    printf("\ncurrent: %u \n", pq->current);

    // 1, 3, 4, 2
    pq_push(pq, (uint64_t)1);

    assert(pq_is_empty(pq) == 0);

    uint64_t get = pq_peek(pq);
    assert(get == 1);


    pq_push(pq, (uint64_t)3);

    pq_push(pq, (uint64_t)4);

    pq_push(pq, (uint64_t)2);

    get = pq_peek(pq);
    assert(get == 1); 

    get = pq_pop(pq);
    assert(get == 1);

    get = pq_pop(pq);
    assert(get == 2);

    get = pq_pop(pq);
    assert(get == 3);

    get = pq_pop(pq);
    assert(get == 4);


    get = pq_pop(pq);
    assert(get == 0);

    pq_push(pq, (uint64_t)12);

    get = pq_pop(pq);
    assert(get == 12);

    // that



    // and a whole many more edge cases i'm sure
    // but this is a good start

    pq_free(pq);

}

#endif
