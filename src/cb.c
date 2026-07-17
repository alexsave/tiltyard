#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cb.h"
#include "types.h"

// this is just a circular buffer, free sizes
// is it worth creating separate 64 bit vs 32 bit one?

CB* cb_init(u16 type_size) {
    CB* cb = malloc(sizeof(CB));

    cb->start = INITIAL_START_INDEX;
    cb->end = 0;
    cb->capacity = CB_INITIAL_CAPACITY;
    cb->type_size = type_size;

    cb->buffer = malloc(cb->capacity * type_size);

    return cb;
}

void cb_queue(CB* cb, void* value) {
    //printf("queue requested %u\n", value);
    if (cb->start == INITIAL_START_INDEX) {
        // bit of a trick 
        cb->start = 0;
    } else if (cb->start == cb->end) {
        if (cb->capacity << 1 == 0) {
            printf("completely out of circular buffer space, cannot even double\n");
            exit(1);
            return;
        }

        u8* doubled = malloc(2 * cb->capacity * cb->type_size);

        // copy start to capacity
        memcpy(doubled, &(cb->buffer[cb->start]), (cb->capacity - cb->start) * cb->type_size);
        // copy zero to end
        memcpy(&(doubled[cb->capacity - cb->start]), cb->buffer, (cb->end) * cb->type_size);

        free(cb->buffer);
        cb->buffer = doubled;

        cb->start = 0;
        cb->end = cb->capacity;
        cb->capacity <<= 1;
    }

    for (uint8_t i = 0; i < cb->type_size; i++) {
        cb->buffer[(cb->end*cb->type_size)+i] = *(uint8_t*)(value+i);
    } 

    //for (u8 i = 0; i < cb->type_size; i++)
        //cb->buffer[cb->end] = value;
    cb->end = (cb->end + 1) % cb->capacity;
}

void* cb_deque(CB* cb) {
    if (cb->start == INITIAL_START_INDEX)
        return 0;

    void* result = cb->buffer + cb->start * cb->type_size;
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

u32 cb_count(CB* cb) {
    if (cb_is_empty(cb))
        return 0;
    // start == end means completely full, so the -1 +1 dance keeps that from reading as 0
    return ((u32)cb->end + cb->capacity - cb->start - 1) % cb->capacity + 1;
}

void* cb_at(CB* cb, u32 i) {
    return cb->buffer + ((cb->start + i) % cb->capacity) * cb->type_size;
}

void cb_free(CB* cb) {
    free(cb->buffer);

    free(cb);
}

