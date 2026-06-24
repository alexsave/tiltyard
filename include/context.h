#ifndef CONTEXT_H
#define CONTEXT_H

#include "order.h"
#include "types.h"

typedef struct Context {
    void* mbo_snapshot;
    // same thing here, both needed
    u32 order_id; // the order this response is for. MAX_U32 for broadcasts?
    Order* response_order_ptr;

    Order* next_order_ptr;
    u32 random;
    u8 status; // status of ^THIS order
} Context;

#endif

