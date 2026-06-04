#include <stdint.h>
#include <stdlib.h>
#include "pq.h"

// min heap priority queue

PQ* pq_init(uint32_t capacity) {
    uint64_t* heap = malloc(capacity * sizeof(uint64_t));

    PQ* pq = malloc(1 * sizeof(PQ));

    pq->current = 1; // possibly one for easier math later
    pq->heap = heap;
    pq->heap[0] = capacity;

    return pq;
}

void pq_push(PQ* pq, uint64_t event) {
    uint32_t run = pq->current;

    while (1) {
        //parent
        uint32_t next = run >> 1; 
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
uint64_t pq_peek(PQ* pq){
    return pq->heap[1];
}

uint64_t pq_pop(PQ* pq) {
    if (pq->current == 1)
        return 0;

    pq->current = pq->current - 1;

    uint64_t* heap = pq->heap;

    uint64_t event = heap[1];  
    uint32_t run = 1;

    uint64_t last_copy = heap[pq->current];

    heap[pq->current] = 0;

    while(1){
        uint32_t left = (run << 1);
        uint32_t right = left + 1;

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
            }
            // else we're still done anyways, as we have one child and its >= last_copy
            break;
        }

        // e for event btw
        uint64_t right_e = heap[right];

        // normal case
        // it has to be the case that both the value at left & right is 

        // I think it's really just two, but I ned to test properly yes
        if (right_e > last_copy && left_e > last_copy) {
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

//ez
void pq_free(PQ* pq) {
    free(pq->heap);
    free(pq);
}

