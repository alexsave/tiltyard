#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "types.h"
#include "constants.h"
#include "pq.h"

// min heap priority queue

PQ* pq_init() {
    uint64_t* heap = malloc(INITIAL_CAPACITY * sizeof(uint64_t));

    PQ* pq = malloc(1 * sizeof(PQ));

    pq->current = 1; // one for easier math later
    pq->heap = heap;
    pq->heap[0] = INITIAL_CAPACITY;

    return pq;
}

// crucial note: 
// this assumed that priority is either entire event, or top bits of event
// something in here is fucked
void pq_push(PQ* pq, uint64_t event) {

    // we are restricted by limits of current 
    if (pq->current == MAX_U32) {
        printf("Current has hit max value, upgrade to uint64 now.\n");
        exit(1);
        return;
    }

    // actually more like ==, i don't know how you could get >
    if (pq->current == pq->heap[CAPACITY_INDEX]) {
        //if(debug) printf("doublign");
        uint64_t current_capacity = pq->heap[CAPACITY_INDEX];
        uint64_t* doubled = malloc(2 * current_capacity * sizeof(uint64_t));
    
        memcpy(doubled, pq->heap, current_capacity * sizeof(uint64_t));

        doubled[CAPACITY_INDEX] *= 2;

        free(pq->heap);
        pq->heap = doubled;
    } else if (pq->current == pq->heap[CAPACITY_INDEX]) {
        printf("Current has exceeded capacity. it shouldn't\n");
    }
    
    uint32_t run = pq->current;

    // 4-ary now: parent of j is ((j-2)>>2)+1, so the tree is half as deep as the binary one
    // was and a sift touches half the cache lines. pop order is unchanged - keys are unique
    // (every event carries a distinct id in its low bits), so min-extraction order is fully
    // determined by key order no matter the heap shape
    while (run > 1) {
        uint32_t parent = ((run - 2) >> 2) + 1;
        //equals very very unlikely you'll see why
        if (pq->heap[parent] <= event)
            break;
        pq->heap[run] = pq->heap[parent];
        run = parent;
    }
    pq->heap[run] = event;

    pq->current = pq->current + 1;
}

// ez one
// undefined behavior if empty, ie pq->current == 1
uint64_t pq_peek(PQ* pq){
    // should we return zero?
    // genuinely don't know what the fastest option is
    // maybe pq should handle it, maybe the callers should (keepign this function lightweight)
    // or we could put 0 in the 1 index when we're empty
    // but that could also be confused with a event of type 0 "client toggle", params 0, priority 0
    // calling is empty is the best move
    return pq->heap[1];
}

// well this is bugged, explaining why we had multipel boot events
uint64_t pq_pop(PQ* pq) {
    if (pq->current == 1)
        return 0;

    pq->current = pq->current - 1;

    uint64_t* heap = pq->heap;

    uint64_t event = heap[1];  
    //u8 debug = 0;
    //if (event == 17210121269605826586){
        //printf("you want to explain to me how exaclty this comes out twice??????\n");
        //printf("pq current %u\n", pq->current);
        //printf("%llu %llu %llu %llu\n", heap[0] , heap[1] , heap[2] , heap[3] );
        //debug = 1;

    //}

    //u8 debug2 = 0;
    //if (event == 17208732656582262808){
        //printf("DEBUG2 locked in\n");
        //debug2 = 1;
    //}
    uint32_t run = 1;

    uint64_t last_copy = heap[pq->current];

    heap[pq->current] = 0;
    // just to be safe, ya never know

    // 4-ary sift-down: children of i sit at 4i-2 .. 4i+1, adjacent, so the four loads land
    // in one or two cache lines instead of a fresh line per level like the old binary walk
    while (1) {
        uint32_t first = (run << 2) - 2;
        if (first >= pq->current) {
            // no children, gottem
            heap[run] = last_copy;
            break;
        }
        uint32_t end = first + 4;
        if (end > pq->current)
            end = pq->current;

        uint32_t best = first;
        uint64_t best_e = heap[first];
        for (uint32_t k = first + 1; k < end; k++) {
            if (heap[k] < best_e) {
                best = k;
                best_e = heap[k];
            }
        }

        if (best_e >= last_copy) {
            // smallest child doesn't beat it, this is home
            heap[run] = last_copy;
            break;
        }

        // very likely case: pull the smallest child up and keep walking down
        heap[run] = best_e;
        run = best;
    }

    return event;
}

uint8_t pq_is_empty(PQ* pq) {
    return pq->current == 1;
}

//ez
void pq_free(PQ* pq) {
    free(pq->heap);
    free(pq);
}

