#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

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
void pq_push(PQ* pq, uint64_t event) {
    // we are restricted by limits of current 
    uint32_t max_32 = ((((uint32_t)1 << 31) - 1) << 1) + 1;
    if (pq->current == max_32) {
        printf("Current has hit max value, upgrade to uint64 now.\n");
        return;
    }

    // actually more like ==, i don't know how you could get >
    if (pq->current == pq->heap[CAPACITY_INDEX]) {
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

    while (1) {
        //parent
        //equals very very unlikely you'll see why 
        if (run == 1 || pq->heap[run >> 1] <= event) { 
            pq->heap[run] = event;
            break;
        } else {
            pq->heap[run] = pq->heap[run >> 1];
            run = run >> 1;
        }
    }

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
    uint32_t run = 1;

    uint64_t last_copy = heap[pq->current];

    heap[pq->current] = 0;
    // just to be safe, ya never know

    while(1){
        uint32_t left = (run << 1);
        uint32_t right = left + 1;

        // printf("right %llu pq current %llu left  %llu\n", right, pq->current, left);
        if (right >= pq->current && left >= pq->current) {
            // gottem
            heap[run] = last_copy;
            break;
        }
        uint64_t left_e = heap[left];

        if (right >= pq->current) {
            // only right out of bounds

            if (left_e < last_copy) {
                // swap
                heap[run] = left_e;
                heap[left] = last_copy;
            } else {
                heap[run] = last_copy;
            }
            // else we're still done anyways, as we have one child and its >= last_copy
            break;
        }

        // e for event btw
        uint64_t right_e = heap[right];

        // normal case
        // it has to be the case that both the value at left & right is 

        // printf("right e %llu last copy %llu left e %llu\n", right_e, last_copy, left_e);
        // I think it's really just two, but I ned to test properly yes
        if (right_e > last_copy && left_e > last_copy) {
            // printf("where we need to be\n");
            // exactly where we need to be
            break;
        } else {
            // very likely case
            // swap with lower one and update "run" properly
            if (right_e <= left_e) {
                // move right
                heap[run] = right_e;
                run = right;
            } else {
                // move left
                heap[run] = left_e;
                run = left;
            } // what if they're equal? unlikely, but we'll go right
        }

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

