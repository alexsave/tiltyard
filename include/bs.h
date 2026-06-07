#ifndef BS_H
#define BS_H

#include <stdint.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
// fixed sized free list

typedef struct Order {
    uint32_t quantity;
    uint32_t client_id;
    uint32_t price;
    uint32_t type;
} Order;

typedef struct BSM {
    uint32_t refs;
    uint32_t offset;
    uint32_t size;
} BSM;


// 2^16 - 1
static const uint16_t INITIAL_METADATA_INDEX = (uint16_t)65536;

// this only supports 16K snapshots, which may be enough
typedef struct BS {
    uint16_t md_start;
    uint16_t md_end;
    uint16_t md_capacity;
    BSM* metadata;

    uint32_t store_capacity;
    void* store;
} BS;

BS* bs_init();
void bs_free(BS* bs);
uint32_t bs_reserve(BS* bs, uint32_t size, uint32_t refs, void ** address_holder);

void* bs_get(BS* bs, uint32_t bs_number);

#endif
