
// root test file
// will call other tests

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "rand.h"
#include "pq.h"

void test_random() {
    // call into random
    // now we're at the same problem 
    // in that idk how to incldue stuff

    uint64_t seed = 603;

    uint64_t* rand = rand_init(seed);
    printf("%llu\n", *rand);
    rand_next(rand);
    printf("%llu\n", *rand);
    
    
    printf("testing random\n");

    rand_free(rand);
}

void test_pq() {
    // how do we use this again?
    // TDD: test driven development
    //hmmm
    //initialize it + clear it at the end

    PQ* pq = pq_init(1024);

    printf("\nheap: ");
    for(int i = 0; i < 6; i++) printf("%llu ", pq->heap[i]);
    printf("\ncurrent: %lu \n", pq->current);

    // 1, 3, 4, 2
    pq_push(pq, (uint64_t)1);

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


    // in between...

    // schedule at time "1"
    // schedule at time "3"
    // peek -> return 1
    // schedule at time "2"
    // peek -> return 1
    // pop -> return 1
    // peek -> return 2
    // schedule 1
    // peek -> return 1

    // and a whole many more edge cases i'm sure
    // but this is a good start

    pq_free(pq);
    
}

int main(int argc, char* argv[]){
    // Random testing section
    //test_random();

    test_pq();

    return 0;
}
