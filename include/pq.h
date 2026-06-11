#ifndef PQ_H
#define PQ_H

// maybe this can be PQ
typedef struct PQ {
    uint32_t current; // or should this point direclty into array?
    uint64_t* heap;
} PQ;

static const uint64_t INITIAL_CAPACITY = 1024;
static const uint64_t CAPACITY_INDEX = 0;

PQ* pq_init();

void pq_push(PQ* pq, uint64_t event);

uint64_t pq_peek(PQ* pq);

uint64_t pq_pop(PQ* pq);

uint8_t pq_is_empty(PQ* pq);

void pq_free(PQ* pq);

#endif
