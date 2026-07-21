#ifndef CB_H
#define CB_H

#include <stdio.h>
#include <string.h>

#include "types.h"
// fixed sized free list


// empty sentinel: must be an out-of-range index. capacity can now grow past 64K,
// so 65535 would collide with a real slot — use the u32 max instead.
static const u32 INITIAL_START_INDEX = (u32)0xFFFFFFFF;

static const u32 CB_INITIAL_CAPACITY = 1024;

// start/end/capacity are u32: the trade tape is append-only (never dequeued) and a
// busy 40-day run needs ~180k entries, well past what u16 (max 65535) could address.
typedef struct CB {
    u32 start;
    u32 end;
    u32 capacity;
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
// empty the buffer in O(1) by resetting the indices; keeps the allocation for reuse
void cb_clear(CB* cb);
// the i-th entry from the front without dequeuing (0 = next out). the ring can wrap, so
// callers can't just index the buffer
void* cb_at(CB* cb, u32 i);
void cb_free(CB* cb);

#endif
