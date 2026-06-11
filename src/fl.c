#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "fl.h"

// fixed sized free list


FL* fl_init(uint8_t type_size) {
    FL* fl = malloc(1*sizeof(FL));

    uint8_t * data = malloc(FL_INITIAL_CAPACITY * type_size);
    uint32_t* stack = malloc(FL_INITIAL_CAPACITY * sizeof(uint32_t));

    for (uint32_t i = 0; i < FL_INITIAL_CAPACITY; i++) {
        stack[i] = i;
    }

    fl->data = data;
    fl->stack = stack;
    fl->sp = FL_INITIAL_CAPACITY;
    fl->capacity = FL_INITIAL_CAPACITY;
    fl->type_size = type_size;

    return fl;
}

// you can just put whatever in here
uint32_t fl_insert(FL* fl, void* data) {

    if (fl->sp == 0) {
        // quick check
        if ((fl->capacity << 1) == 0) {
            printf("completely out of capacity. rearchitect freelist\n");
            // return some bogus
            return ((fl->capacity - 1) << 1) + 1;
        }

        uint32_t* doubled_stack = malloc(2 * fl->capacity * sizeof(uint32_t));
        for (uint32_t i = 0; i < fl->capacity; i++) {
            // can't do mem copy because this genuinely new data, new indicies
            doubled_stack[i] = i + fl->capacity;
        }
        free(fl->stack);
        fl->stack = doubled_stack;

        uint8_t* doubled_data = malloc(2 * fl->capacity * fl->type_size);
        memcpy(doubled_data, fl->data, fl->capacity * fl->type_size);
        free(fl->data);
        fl->data = doubled_data;

        fl->sp = fl->capacity;
        
        fl->capacity <<= 1;
    }

    fl->sp = fl->sp - 1;

    uint32_t id = fl->stack[fl->sp];

    // this is ugly but - find a better way maybe with fixed width
    // maybe we have shared helper methods, but specific types of freelists use fixed width
    for (uint8_t i = 0; i < fl->type_size; i++) {
        fl->data[(id*fl->type_size)+i] = *(uint8_t*)(data+i);
    }

    return id;
}

void fl_release(FL* fl, uint32_t id) {
    // shouldn't happen but prevents extra releases
    if (fl->sp == fl->capacity)
        return;

    fl->stack[fl->sp] = id;
    fl->sp = fl->sp + 1;
}

void fl_free(FL* fl) {
    free(fl->data);
    free(fl->stack);
    free(fl);
}
