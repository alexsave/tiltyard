#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cb.h"
#include "types.h"

// this is just a circular buffer, hard coded to u32

CB* cb_init() {
    CB* cb = malloc(sizeof(CB));

    cb->start = INITIAL_START_INDEX;
    cb->end = 0;
    cb->capacity = CB_INITIAL_CAPACITY;

    cb->buffer = malloc(cb->capacity * sizeof(u32));

    return cb;
}

void cb_queue(CB* cb, u32 value) {
    printf("queue requested %u\n", value);
    if (cb->start == INITIAL_START_INDEX) {
        // bit of a trick 
        cb->start = 0;
    } else if (cb->start == cb->end) {
        if (cb->capacity << 1 == 0) {
            printf("completely out of circular buffer space, cannot even double\n");
            return;
        }

        // |56.. end start 1234|

        u32* doubled = malloc(2 * cb->capacity * sizeof(u32));

        // copy start to capacity
        memcpy(doubled, &(cb->buffer[cb->start]), (cb->capacity - cb->start) * sizeof(u32));
        // copy zero to end
        memcpy(&(doubled[cb->capacity - cb->start]), cb->buffer, (cb->end) * sizeof(u32));

        free(cb->buffer);
        cb->buffer = doubled;

        cb->start = 0;
        cb->end = cb->capacity;
        cb->capacity <<= 1;
    }

    cb->buffer[cb->end] = value;
    cb->end = (cb->end + 1) % cb->capacity;
}

u32 cb_deque(CB* cb) {
    if (cb->start == INITIAL_START_INDEX)
        return 0;

    u32 result = cb->buffer[cb->start];
    cb->start = (cb->start + 1) % cb->capacity;

    if (cb->start == cb->end) {
        // there's gotta be an easier way than this
        cb->start = INITIAL_START_INDEX;
        cb->end = 0;
    }

    return result;
}

u8 cb_is_empty(CB* cb) {
    return cb->start == INITIAL_START_INDEX;
}

void cb_free(CB* cb) {
    free(cb->buffer);

    free(cb);
}

