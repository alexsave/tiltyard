#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "types.h"
#include "constants.h"
#include "xpq.h"

// max heap priority queue - pq.c with the comparisons flipped

XPQ* xpq_init() {
    uint64_t* heap = malloc(INITIAL_CAPACITY * sizeof(uint64_t));

    XPQ* xpq = malloc(1 * sizeof(XPQ));

    xpq->current = 1; // one for easier math later
    xpq->heap = heap;
    xpq->heap[0] = INITIAL_CAPACITY;

    return xpq;
}

void xpq_push(XPQ* xpq, uint64_t event) {

    if (xpq->current == MAX_U32) {
        printf("Current has hit max value, upgrade to uint64 now.\n");
        exit(1);
        return;
    }

    if (xpq->current == xpq->heap[CAPACITY_INDEX]) {
        uint64_t current_capacity = xpq->heap[CAPACITY_INDEX];
        uint64_t* doubled = malloc(2 * current_capacity * sizeof(uint64_t));

        memcpy(doubled, xpq->heap, current_capacity * sizeof(uint64_t));

        doubled[CAPACITY_INDEX] *= 2;

        free(xpq->heap);
        xpq->heap = doubled;
    }

    uint32_t run = xpq->current;

    while (1) {
        // parent stays if it's still the bigger one
        if (run == 1 || xpq->heap[run >> 1] >= event) {
            xpq->heap[run] = event;
            break;
        } else {
            xpq->heap[run] = xpq->heap[run >> 1];
            run = run >> 1;
        }
    }

    xpq->current = xpq->current + 1;
}

// undefined behavior if empty, ie xpq->current == 1
uint64_t xpq_peek(XPQ* xpq){
    return xpq->heap[1];
}

uint64_t xpq_pop(XPQ* xpq) {
    if (xpq->current == 1)
        return 0;

    xpq->current = xpq->current - 1;

    uint64_t* heap = xpq->heap;

    uint64_t event = heap[1];

    uint32_t run = 1;

    uint64_t last_copy = heap[xpq->current];

    heap[xpq->current] = 0;
    // just to be safe, ya never know

    while (1) {
        uint32_t left = (run << 1);
        uint32_t right = left + 1;

        if (right >= xpq->current && left >= xpq->current) {
            heap[run] = last_copy;
            break;
        }
        uint64_t left_e = heap[left];

        if (right >= xpq->current) {
            // only right out of bounds

            if (left_e > last_copy) {
                // swap
                heap[run] = left_e;
                heap[left] = last_copy;
            } else {
                heap[run] = last_copy;
            }
            break;
        }

        uint64_t right_e = heap[right];

        if (right_e < last_copy && left_e < last_copy) {
            // exactly where we need to be
            heap[run] = last_copy;
            break;
        } else {
            // swap with the bigger child and follow it down
            if (right_e >= left_e) {
                heap[run] = right_e;
                run = right;
            } else {
                heap[run] = left_e;
                run = left;
            }
        }

    }

    return event;
}

uint8_t xpq_is_empty(XPQ* xpq) {
    return xpq->current == 1;
}

void xpq_free(XPQ* xpq) {
    free(xpq->heap);
    free(xpq);
}
