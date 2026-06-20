#ifndef CONTEXT_H
#define CONTEXT_H

#include "order.h"
#include "types.h"

typedef struct Context {
    void* mbo_snapshot;
    Order* order_ptr;
    u32 random;
} Context;

#endif

