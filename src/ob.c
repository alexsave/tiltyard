#include <string.h>

#include "ob.h"

#include "types.h"
#include "bs.h"
#include "cb.h"
#include "order.h"
#include "fl.h"


// parsing stuff

// oldob and newob are pointers into the blobstore, ready to be written to
// its a bit tricky because we wont know how much to write until hlway through
void ob_market(u8 direction, u16 quantity, Order* in, void* old_mbo_raw, void* old_mbp, BS* mbo_bs, BS* mbp_bs) {

    if (direction == 1) {
        // buy, change this to constant val later

        // step one is to analyze the MBP to figure out how much to allocate

        MBP* mbp = (MBP*)old_mbp;
        u8 hi_bid_index = mbp->hi_bid_index;
        u8 low_ask_index = hi_bid_index + 1;

        u8 level_count = mbp->level_count;

        // this will go down as we wipe levels
        u8 new_level_count = level_count;

        // try to fill this level by level
        u16 remaining_quantity = quantity;

        u8 i = low_ask_index;
        for (; i < level_count; i++){
            // this copies the struct into levels, change to pointer if its slow
            MBPLevel level = mbp->levels[i];
            
            // less than, wipe out entire level
            // equal , wipe out entire level
            // greater, pause
            if (level.quantity > remaining_quantity) {
                // this is the new lowest ask
                break;
            } else {
                // wipe out entire level
                new_level_count--;
                remaining_quantity -= level.quantity;
            }
        }

        u8 new_low_ask_index = i;

        // the new lowest ask is the level at i
        // we now need to throughly go through MBO and try to match
        if (remaining_quantity == 0){
            // special case where we can skip going through MBO to figure out some allocation sizes
        }
    
        MBO* mbo = (MBO*)old_mbo_raw;
        MBOIndex new_low = mbo->levels[new_low_ask_index];
        MBOLevel* new_low_level = (MBOLevel*)(old_mbo_raw+/* some bullshit headers*/new_low.byte_offset);


        //CB* filled_orders = cb_init();

        u8 remaining_orders_on_level = new_low_level->order_count;
        u16 remaining_quantity_on_level = mbp->levels[new_low_ask_index].quantity;

        if (remaining_quantity == 0){
        } else {
            for(u8 j = 0; j < new_low_level->order_count; j++) {
                u32 order_id = new_low_level->order_ids[j];

                //Order* in = (Order*)fl_get(orders, order_id);

                u16 order_quantity = in->quantity;

                if (order_quantity > remaining_quantity) {
                    // modify order directly, maybe send partial fill notification
                    in->quantity -= remaining_quantity;
                    remaining_quantity_on_level -= remaining_quantity;
                    break;
                } else {
                    // fill order entirely
                    remaining_orders_on_level--;
                    remaining_quantity -= order_quantity;
                    remaining_quantity_on_level -= order_quantity;
                }
            }
        }
        

        // at this point, here is what we have
        // level count goes down by (new_low_ask_index - low_ask_index)

        u8 updated_level_count = (level_count - (new_low_ask_index - low_ask_index));
        
        // this does not count the array at all
        u32 mbp_size = sizeof(MBP) + sizeof(MBPLevel) * updated_level_count;
    
        void* new_mbp;
        u32 handle = bs_reserve(mbp_bs, mbp_size, 1, &new_mbp);


        MBP* mbp_p = (MBP*)new_mbp;
        mbp_p->hi_bid_index = hi_bid_index;
        mbp_p->level_count = updated_level_count;

        // MBO levels zero through hi_bid_index are unchanged
        memcpy(&(mbp_p->levels[0]), &(mbp->levels[0]), (hi_bid_index + 1)* sizeof(MBPLevel));

        // funny business on this guy
        mbp_p->levels[low_ask_index].price = mbp_p->levels[new_low_ask_index].price;
        mbp_p->levels[low_ask_index].quantity = remaining_quantity_on_level;

        memcpy(&(mbp_p->levels[low_ask_index+1]), &(mbp->levels[new_low_ask_index+1]), 
            (level_count - (new_low_ask_index + 1))* sizeof(MBPLevel));


        // at this point we have now copied MBP successfully


        // move on to MBO

        MBO* old_mbo = (MBO*)old_mbo_raw;

        u32 size = calculate_mbo_size(old_mbo, new_low_ask_index, remaining_quantity_on_level);

        void* new_mbo_raw;
        
        u32 mbo_handle = bs_reserve(mbo_bs, size, 1, &new_mbo_raw);

        MBO* new_mbo = (MBO*)new_mbo_raw;

        

        new_mbo->hi_bid_index = hi_bid_index;
        new_mbo->level_count = updated_level_count;

        memcpy(&(new_mbo->levels[0]), &(old_mbo->levels[0]), (hi_bid_index + 1)  * sizeof(MBOIndex));

        // the price levels will be correct, but not the byte offsets
        memcpy(&(new_mbo->levels[low_ask_index]), &(old_mbo->levels[new_low_ask_index]), (level_count - (new_low_ask_index + 1)) * sizeof(MBOIndex));
        
        void* old_after_levels = old_mbo_raw + (sizeof(MBO) + (level_count * sizeof(MBOIndex)));
        void* after_levels = new_mbo + (sizeof(MBO) + (hi_bid_index + level_count - new_low_ask_index * sizeof(MBOIndex)));

        // now we start writing the orders

        u16 size_of_unmodified = old_mbo->levels[low_ask_index].byte_offset - old_mbo->levels[0].byte_offset;

        // copy unmodified buys
        memcpy(after_levels, old_after_levels, size_of_unmodified);

        u16 run_index = 0;

        u16 orders_on_level = remaining_orders_on_level;

        // new low ask
        MBOLevel* level = (MBOLevel*)(after_levels+size_of_unmodified);
        level->order_count = orders_on_level;
        for (u8 i = 0; i < orders_on_level; i++) {
            level->order_ids[i] = new_low_level->order_ids[new_low_level->order_count - orders_on_level + i];
        }

        void* next_level = ((void*)level) + 
                (sizeof(MBOLevel) + orders_on_level * sizeof(u32));

        // the metadata for the low ask index will point to this much (ie after the unmodified buys)
        new_mbo->levels[low_ask_index + run_index].byte_offset = size_of_unmodified;

        for(; run_index < level_count - new_low_ask_index; run_index++){

            // get the next level from old mbo
            // everything after new low ask will be copied

            u16 old_level_offset = old_mbo->levels[new_low_ask_index + run_index].byte_offset;

            MBOLevel* old_level = (MBOLevel*)(old_after_levels + old_level_offset);

            u32 level_size = sizeof(MBOLevel) + (old_level->order_count * sizeof(u32));

            memcpy(next_level, old_level, level_size);



            // i hope this is a num
            new_mbo->levels[run_index+low_ask_index].byte_offset = (next_level - after_levels);

            next_level = next_level + level_size;
        }






    } else if (direction == 0) {
        // sell

    }





}

u32 calculate_mbo_size(MBO* old_mbo, u32 new_low_ask_index, u16 remaining_order_new_low_ask) {
    u8 hi_bid_index = old_mbo->hi_bid_index;
    u8 low_ask_index = hi_bid_index + 1;
    u8 level_count = old_mbo->level_count;

    // specifically for market buy

    u32 size = 0;

    // mbo metadata
    size += sizeof(MBO);

    // the "L2" array
    size += sizeof(MBOIndex) * (hi_bid_index + level_count - new_low_ask_index);

    u16 size_of_unmodified = old_mbo->levels[low_ask_index].byte_offset - old_mbo->levels[0].byte_offset;

    // unmodified buys
    size += size_of_unmodified;

    // new lowest ask
    size += sizeof(MBOLevel) + remaining_order_new_low_ask * sizeof(u32);

    // unmodified asks

    u16 old_after_levels = sizeof(MBO) + (old_mbo->level_count * sizeof(MBOIndex));
    
    u16 old_first_unmod_ask_offset = old_mbo->levels[new_low_ask_index + 1].byte_offset;
    u16 old_last_unmod_ask_offset = old_mbo->levels[old_mbo->level_count - 1].byte_offset;
    u16 last_level_orders = ((MBOLevel*)(old_mbo + old_after_levels + old_last_unmod_ask_offset))->order_count;
    u16 last_level_size = sizeof(MBOLevel) + last_level_orders * sizeof(u32);

    size += (old_last_unmod_ask_offset + last_level_size) - (old_first_unmod_ask_offset);

    return size;
}

void ob_limit() {
}

