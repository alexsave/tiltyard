#ifndef MBP_H
#define MBP_H

#include "types.h"
#include "server.h"

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

typedef struct MBP10 {
    // first 4 bits are bid count, last 4 are ask count
    // do not bother checking the index if 
    // 0-9 are bids, 10-19 are asks
    //u8 bid_ask_counts;
    // nah fuck that just set price zero and call it a day
    MBPIndex levels[20];
} MBP10;

// EZ
typedef struct MBP1 {
    MBPIndex hi_bid;
    MBPIndex lo_ask;
} MBP1;

// how many bytes the mbp derived from this mbo needs (for the bs reservation)
u32 mbp_derive_size(void* mbo_raw);
// squash an mbo into its price-aggregated mbp
// this should probably return a bit mask of which of MBP10, MBP1 were updated
// or no, we dont' even need to do that
void mbp_derive(ServerContext* cs);

// debug dumps, mirroring mbo_dump
void mbp_dump(void* mbp_raw);
void mbp10_dump(void* mbp10_raw);
void mbp1_dump(void* mbp1_raw);


#endif

