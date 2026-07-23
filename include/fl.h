#ifndef FL_H
#define FL_H

#include <stdint.h>
#include "types.h"

// fixed sized free list

// TODO: no occupancy tracking. the stack cannot tell you whether an id is already free, so a
// double release pushes it twice and fl_insert later mints it to two live owners at once, who
// then silently share a slot. the check in fl_release only catches sp==0/out of range, and it
// prints without returning. wants a u8 live[] per slot (set on insert, cleared on release,
// refuse if already clear). until then every release site has to prove it can only run once
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

// the id the next fl_insert will return, without taking it. valid only until the next insert
// or release on this fl
u32 fl_next_id(FL* fl);

void* fl_release(FL* fl, u32 id);

// get without releasing
void* fl_get(FL* fl, u32 id);

void fl_free(FL* fl);

#endif

