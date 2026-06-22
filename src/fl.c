#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "fl.h"
#include "types.h"
#include "constants.h"

// fixed sized free list


// with some compression, these could be a single char
// reserved_id - lowest N value can never be assigned
// anything >= than id_limit cannot be reserved
FL* fl_init(uint8_t type_size, u32 id_limit) {
    FL* fl = malloc(1*sizeof(FL));

    uint8_t* data = malloc(FL_INITIAL_CAPACITY * type_size);
    uint32_t* stack = malloc(FL_INITIAL_CAPACITY * sizeof(uint32_t));

    for (uint32_t i = 0; i < FL_INITIAL_CAPACITY; i++) {
        stack[i] = i;
    }

    fl->data = data;
    fl->stack = stack;
    fl->sp = 0;
    fl->capacity = FL_INITIAL_CAPACITY;
    fl->type_size = type_size;
    fl->id_limit = id_limit;

    return fl;
}

// you can just put whatever in here
uint32_t fl_insert(FL* fl, void* data) {
    if (fl->sp == fl->id_limit) {
        // really we won't be checking max_u32, we will be checking for this log
        // if it appears we will fix things to avoid it
        printf("reached id limit, rearchitect freelist\n");
        exit(1);
        return MAX_U32;
    }

    // we're about to get id from fl->sp + 1
    if (fl->sp + 1 >= fl->capacity) {
        printf("doubling fl capacity\n");
        // quick check (does not fit in 32 bits anymore)
        if ((fl->capacity << 1) == 0) {
            printf("completely out of capacity. rearchitect freelist\n");
            exit(1);
            // return some bogus
            return ((fl->capacity - 1) << 1) + 1;
        }

        u32* doubled_stack = malloc(2 * fl->capacity * sizeof(u32));

        // we can leave the bottom half as uninitialized as it will fill back down
        // as we release
        // but the top half must be filled like so
        
        for(u32 i = fl->capacity; i < 2 * fl->capacity; i++){
            doubled_stack[i] = i;
        }

        // 
        free(fl->stack);
        fl->stack = doubled_stack;

        uint8_t* doubled_data = malloc(2 * fl->capacity * fl->type_size);
        memcpy(doubled_data, fl->data, fl->capacity * fl->type_size);
        free(fl->data);
        fl->data = doubled_data;

        fl->sp = fl->capacity;
        
        fl->capacity <<= 1;
    }

    fl->sp = fl->sp + 1;

    uint32_t id = fl->stack[fl->sp];

    // this is ugly but - find a better way maybe with fixed width
    // maybe we have shared helper methods, but specific types of freelists use fixed width
    for (uint8_t i = 0; i < fl->type_size; i++) {
        if(id>197855180){
            printf("sp %u capacity %u\n", fl->sp, fl->capacity);
            printf("id %u i %u destination %u\n", id, i, ((id*fl->type_size)+i));
            printf("getting bytes %p\n", data);
            exit(1);
        }
        fl->data[(id*fl->type_size)+i] = *(uint8_t*)(data+i);
    }

    return id;
}

void* fl_release(FL* fl, uint32_t id) {
    // shouldn't happen but prevents extra releases
    if (fl->sp == 0 || id >= fl->id_limit) {
        printf("This shouldn't happen, investigate why %u was released again.\n", id);
        // null ptr is fine
        return 0;
    }

    // pop the id back into the stack of available ids
    fl->stack[fl->sp] = id;
    fl->sp = fl->sp - 1;

    //maybe?
    return fl->data + id*fl->type_size;
}

void* fl_get(FL* fl, uint32_t id) {
    return fl->data + id*(fl->type_size);
}

void fl_free(FL* fl) {
    free(fl->data);
    free(fl->stack);
    free(fl);
}
