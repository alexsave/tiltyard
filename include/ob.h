#ifndef OB_H
#define OB_H

#include "types.h"
#include "order.h"
#include "bs.h"
#include "fl.h"
#include "cb.h"

// full, for every price level
typedef struct MBOIndex {
    u16 price;
    // from lowest byte of this whole structure
    // slightly easier parsing and updating if we consider it "offset from end of mbo levels array"
    // yeah lets do that
    // we might be able to save more space in the ob thing by considering that it doesn't have to be byte offset, but like 4xbyte offset or something
    // tough call but we'll leave it as this for now
    u32 byte_offset;
    // i didnt want to do this, but massively helps with figuring out orders
    u32 quantity;
} MBOIndex;

typedef struct MBO {
    u16 hi_bid_index;
    u16 level_count;
    MBOIndex levels[];
    // after this is the actual order data
} MBO;

typedef struct MBOEntry {
    // not used or filled out for now, but it's ready
    u32 order_id;
    u32 quantity;
} MBOEntry;

typedef struct MBOLevel {
    u16 order_count;
    MBOEntry entries[];
} MBOLevel;


// what should we put in here

// idk but it will be used in many internal methods
typedef struct MBORunner {
    //one to the start of the whole mbo
    MBO* mbo;
    //one to the index (mboi)
    MBOIndex* metadata;
    //one to the start of the level (mbol)
    MBOLevel* level;
    //one to the datastart
    void* data_start;
    //also current index
    u16 index;
} MBORunner;


// maybe therse are enough maybe no
typedef struct MBPLevel {
    u16 price;
    u16 quantity;
} MBPLevel;

typedef struct MBP {
    u8 hi_bid_index;
    u8 level_count;
    MBPLevel levels[];
} MBP;

// now returns actual size instead of the new handle. bs is handled by server
u32 ob_execute(u32 order_id, Order* in, void* old_mbo_raw, void* new_mbo_raw, CB* fills);
void mbo_dump(void* mbo_raw);

#endif

