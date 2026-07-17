#ifndef CB_H
#define CB_H

#include <stdio.h>
#include <string.h>

#include "types.h"
// fixed sized free list


// 2^16 - 1
static const uint16_t INITIAL_START_INDEX = (uint16_t)65535;

static const u32 CB_INITIAL_CAPACITY = 1024;

// this only supports 16K snapshots, which may be enough
typedef struct CB {
    u16 start;
    u16 end;
    u16 capacity;
    u16 type_size;
    u8* buffer;
} CB;

CB* cb_init(u16 type_size);
void cb_queue(CB* cb, void* value);
void* cb_deque(CB* cb);
// like dequee, but without actually removing
void* cb_last(CB* cb);
u8 cb_is_empty(CB* cb);
// how many entries are queued right now
u32 cb_count(CB* cb);
// the i-th entry from the front without dequeuing (0 = next out). the ring can wrap, so
// callers can't just index the buffer
void* cb_at(CB* cb, u32 i);
void cb_free(CB* cb);

#endif
