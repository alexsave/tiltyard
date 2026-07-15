#ifndef MBP_H
#define MBP_H

#include "types.h"

typedef struct MBPIndex {
    u16 price;
    u32 quantity;
} MBPIndex;

typedef struct MBP {
    u16 hi_bid_index;
    u16 level_count;
    MBPIndex levels[];
    // after this is the actual order data
} MBP;


#endif

