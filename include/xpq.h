#ifndef XPQ_H
#define XPQ_H

#include "pq.h" // shares INITIAL_CAPACITY / CAPACITY_INDEX, same layout

// max heap twin of pq - same memory, opposite pop order. sell stops want the highest
// trigger price first, and a min heap can't fake that without mangling the whole key
typedef struct XPQ {
    uint32_t current;
    uint64_t* heap;
} XPQ;

XPQ* xpq_init();

void xpq_push(XPQ* xpq, uint64_t event);

uint64_t xpq_peek(XPQ* xpq);

uint64_t xpq_pop(XPQ* xpq);

uint8_t xpq_is_empty(XPQ* xpq);

void xpq_free(XPQ* xpq);

#endif
