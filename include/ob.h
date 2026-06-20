#ifndef OB_H
#define OB_H

#include "types.h"
#include "order.h"
#include "bs.h"
#include "fl.h"

static const u8 REJECT = 1;

// Lets start with MBP market by price
// full, for every price level
typedef struct MBOIndex {
    u16 price;
    // from lowest byte of this whole structure
    // slightly easier parsing and updating if we consider it "offset from end of mbo levels array"
    // yeah lets do that
    u16 byte_offset;
    // i didnt want to do this, but massively helps with figuring out orders
    u16 quantity;
} MBOIndex;

typedef struct MBO {
    u8 hi_bid_index;
    u8 level_count;
    MBOIndex levels[];
    // after this is the actual order data
} MBO;


typedef struct MBOLevel {
    u16 order_count;
    u32 order_ids[];
} MBOLevel;

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

/*
    MBO:
    level count, hi bid index,
    [price/byteoffset]
    [[orderids]]
*/


typedef struct OrderBookMetadata {
    u16 lowest_ask;
    u16 highest_bid;
    u16 row_count;
} OrderBookMetadata;

typedef struct OrderBookRowMetadata {
    u16 price;
    u16 order_count;
} OrderBookRowMetadata;

typedef struct OrderInBook {
    // consider folding quantity into this?
    u32 order_id;
} OrderInBook;

/*
blueprint for writing to order book
 OrderBookMetadata* obm = (*OrderBookMetadata)bs_address;
    // $101, $99
    obm->lowest_ask = 10100;
    obm->highest_bid = 9900;
    obm->row_count = 2;

    bs_address += sizeof(OrderBookMetadata);

    for (u8 row = 0; row < obm->row_count; row++) {
        //make it explicit
    
        OrderBookRowMetadata* obrm = (*OrderBookRowMetadata)bs_address;
        if (row == 0)
            obrm->price = 9900;
        else 
            obrm->price = 10100;
            
        obrm->order_count = 1;

        // interestingly my notes say to limit it to 10 quantity bits, combined with 22bit order id yeilds 32 bits
        // yeah probably a good idea considering this will be a massive data thing

        bs_address += sizeof(OrderBookRowMetadata);

        for (u8 oi = 0; oi < obrm->order_count; oi++) {
            // the fact that it's here tells us its limit
            // adn the side tell us buy/sell
            // and the price
            // we just need quantity?
            // which is stored in the order anyways...?
            // well it's goig to be 32 bits anyways
        
            OrderInBook* oib = (*OrderInBook)bs_address;
            // ah this is so dumb
            if (row == 0)
                oib = order_id99;
            else 
                oib = order_id101;

            bs_address += sizeof(OrderInBook);
        }
    }

*/

void ob_limit(u32 order_id, FL* orders, u32 mbo_handle, BS* mbo_bs);
void mbo_dump(void* mbo_raw);

#endif

