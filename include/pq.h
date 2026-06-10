#ifndef PQ_H
#define PQ_H

// maybe this can be PQ
typedef struct PQ {
    //uint32_t capacity;
    uint32_t current; // or should this point direclty into array?
    uint64_t* heap;
} PQ;


PQ* pq_init(uint32_t capacity);

void pq_push(PQ* pq, uint64_t event);

uint64_t pq_peek(PQ* pq);

uint64_t pq_pop(PQ* pq);

uint8_t pq_is_empty(PQ* pq);

void pq_free(PQ* pq);


#endif
