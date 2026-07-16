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

// how many bytes the mbp derived from this mbo needs (for the bs reservation)
u32 mbp_derive_size(void* mbo_raw);
// squash an mbo into its price-aggregated mbp
void mbp_derive(void* mbp_raw, void* mbo_raw);


#endif

