#ifndef FL_H
#define FL_H

#include <stdint.h>
#include "types.h"

// fixed sized free list

typedef struct FL {
    u32 sp;
    u32* stack;
    // technically liek "any" or void* data, but easier this way
    u8* data;
    u32 capacity;
    // combine these???
    u32 type_size;
    u32 id_limit;
} FL;

static const u32 FL_INITIAL_CAPACITY = 8192;

FL* fl_init(u8 type_size, u32 id_limit);

u32 fl_insert(FL* fl, void* data);

void* fl_release(FL* fl, u32 id);

// get without releasing
void* fl_get(FL* fl, u32 id);

void fl_free(FL* fl);

#endif

