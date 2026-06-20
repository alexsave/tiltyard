#include <string.h>
#include <stdio.h>

#include "ob.h"

#include "types.h"
#include "constants.h"
#include "bs.h"
#include "cb.h"
#include "order.h"
#include "fl.h"

u16 _data_start_offset(u16 level_count) {
    return sizeof(MBO) + level_count * sizeof(MBOIndex);
}

void* data_start(void* mbo_raw){
    return mbo_raw + sizeof(MBO) + ((MBO*)(mbo_raw))->level_count * sizeof(MBOIndex);
}

void mbo_dump(void* mbo_raw) {
    MBO* mbo = (MBO*)mbo_raw;

    printf("===MBO DUMP===\n");
    printf("MBO with %u levels and hi bid index %u\n", mbo->level_count, mbo->hi_bid_index);
    for (u8 i = 0; i < mbo->level_count; i++) {
        MBOIndex mboi = mbo->levels[i];
        if (i == mbo->hi_bid_index)
            printf("[");
        else 
            printf(" ");
        printf("level\t%uc with quantity\t%u at offset\t%u", mboi.price, mboi.quantity, mboi.byte_offset);
        if (i == mbo->hi_bid_index)
            printf("] - highest bid\n");
        else 
            printf("\n");
    }


    void* data_start = mbo_raw + _data_start_offset(mbo->level_count);
    printf("level data\n");

    for (u8 i = 0; i < mbo->level_count; i++) {
        MBOIndex mboi = mbo->levels[i];
        u8 byte_offset = mboi.byte_offset;
        //printf("new mbol %p\n", (void*)(data_);
        MBOLevel mbol = *(MBOLevel*)(data_start + byte_offset);
        printf("%uc\t with %u orders\t", mboi.price, mbol.order_count);
        for (u8 j = 0; j < mbol.order_count; j++) {
            printf("#%u\t", mbol.order_ids[j]);
        }    
        printf("\n");
    }
    printf("===MBO DUMP END===\n");

}



// were doing a lot just to figure out the size
// it might be better to guess the next size we need, then only "lock it in" once we have the size
// will need a change to bs
// but not that hard, just need to update bs->metadata[(bs->md_end-1)%bs->md_capacity].size = size;

void ob_limit(Order* in, u32 order_id, FL* orders, u32 mbo_handle, BS* mbo_bs) {
    u8 direction = (in->flags >> BUY_DIRECTION_BIT) & 1;
    u16 price = in->price;
    u16 quantity = in->quantity;

    // step one - calculate max possible new mbo size
    u32 old_size = mbo_bs->metadata[mbo_handle].size;
    u32 max_new_size = old_size + sizeof(MBOIndex) + sizeof(MBOLevel) + sizeof(u32);

    u32 actual_size = max_new_size;

    void* new_mbo_raw;
    u32 unused = bs_reserve(mbo_bs, max_new_size, 10, &new_mbo_raw);

    MBO* new_mbo = (MBO*)new_mbo_raw;


    void* old_mbo_raw = bs_get(mbo_bs, mbo_handle);
    MBO* old_mbo = (MBO*)old_mbo_raw;

    // step one - calculate new mbo size

    u32 next_mbo_size = 0;
    if (old_mbo->level_count == 0) {
        // very special case - this just goes into the ob no questions asked
        next_mbo_size = sizeof(MBO) + sizeof(MBOIndex) + sizeof(MBOLevel) + sizeof(u32);

        printf("old has zero levels\n");

        new_mbo->level_count = 1;

        if (direction == 1) {
            printf("first bid in!\n");
            // we now have one bid in, at 0
            new_mbo->hi_bid_index = 0;
        } else {
            // we now have one ask in
            printf("first ask in!\n");
            new_mbo->hi_bid_index = MAX_U8;
        }

        new_mbo->levels[0].price = price;
        new_mbo->levels[0].quantity = quantity;
        printf("data start offset for 1 is %u\n", _data_start_offset(1));
        new_mbo->levels[0].byte_offset =  0;

        MBOLevel* mbol = (MBOLevel*)(new_mbo_raw + _data_start_offset(1));
        printf("new mbol %p\n", (void*)mbol);
        mbol->order_count = 1;
        mbol->order_ids[0] = order_id;

        actual_size = next_mbo_size;
    //} else if (old_mbo->hi_bid_index == old_mbo->level_count-1) {
        // only bids
    //} else if (old_mbo->hi_bid_index == MAX_U8) {
        // only asks
    } else {
        i8 hi_bid_index = old_mbo->hi_bid_index;
        i8 lo_ask_index = hi_bid_index + 1;

        u16 hi_bid = old_mbo->levels[hi_bid_index].price;
        u16 lo_ask = old_mbo->levels[lo_ask_index].price;

        u8 start_search;
        i8 multiplier = 0;
        u8 bottom;
        u8 top;

        // buying
        if (direction == 1) {
            start_search = lo_ask_index;
            multiplier = 1;
            //more deep
            bottom = 0;
            top = old_mbo->level_count - 1;
        } else {
            start_search = hi_bid_index;
            multiplier = -1;
            bottom = old_mbo->level_count-1;
            top = 0;
        }

        // marketable limit order
        if ((multiplier)*(old_mbo->levels[start_search].price) <= multiplier*price) {
            printf("we have a marketable limit order\n");
            // yes it is marketable, at least part of it can be matched immediately
            
            u16 remaining_quantity = quantity;

            // EXCLUSIVE NOW, everything outside of these is untouched
            u8 untouched_below = start_search;
            u8 untouched_above = top;

            u8 partial_fill = 0;

            //bid for buy, ask for sell
            u8 our_side = 0;
            u8 their_side = 0;

            u8 level_count = old_mbo->level_count;

            u8 exact_level_wipe = 0;

            u8 modified_level = 0;
            u8 modified_level_index = 0;
            

            u8 current_level;

            // first figure out what rows we need to get rid of
            for(current_level = start_search; ; current_level += multiplier) {
                MBOIndex mboi = old_mbo->levels[current_level];
                MBOIndex next = old_mbo->levels[current_level+multiplier];
                if (remaining_quantity > 0 && (multiplier)*mboi.price > (multiplier)*price) {
                    printf("not enough to fill limit, leaving as open bid or ask\n");
                    level_count++;

                    // stop due to limit price - possibly partial fill
                    untouched_above = current_level - 1;// test this out in scenario with limit between ask and bid gap
                    partial_fill = 1;
                    our_side = price;
                    their_side = next.price;
                    break;
                }
                
                u16 level_quantity = mboi.quantity;

                if (level_quantity > remaining_quantity) {
                    modified_level = 1;
                    modified_level_index = current_level;
                    untouched_above = current_level;
                    their_side = mboi.price;

                    // this fill will be entirely filled on this level, making this lowest ask
                    //next_lowest_ask = mboi.price;
                    break;
                } else if(level_quantity == remaining_quantity) {
                    printf("limit filled exactly at this level\n");
                    remaining_quantity = 0;
                    exact_level_wipe = 1;
                    untouched_above = current_level;
                    // this fill will be entirely filled on this level, mking NEXT highest lowest ask
                    //next_lowest_ask = 
                    their_side = next.price;
                    level_count--;
                    break;
                } else {
                    level_count--;
                }
                
    

                if (start_search == top) {
                    // do something maybe?
                    
                    break;
                }
                
            }

            void* old_data_start = data_start(old_mbo_raw);
            void* new_data_start = data_start(new_mbo_raw);

            void* new_run = new_data_start;
            void* old_run = old_data_start;

           
            //printf("ods %p nds %p nr %p or %p\n", old_data_start, new_data_start, new_run, old_run);
            
            // CHANGING TO EXCLUSIVE
            u8 lowest_untouched_index = direction == 1 ? untouched_below : untouched_above;

            u8 highest_untouched_index = direction == 1 ?  untouched_above : untouched_below;
            printf("lowest untouched index %u highest_untouched index %u\n", lowest_untouched_index, highest_untouched_index);

            //if (lowest_untouched_level 

            current_level = 0;
            for (; current_level < lowest_untouched_index; current_level++) {
                MBOIndex mboi = old_mbo->levels[current_level];

                new_mbo->levels[current_level].price = mboi.price;
                new_mbo->levels[current_level].quantity = mboi.quantity;
                new_mbo->levels[current_level].byte_offset = (new_run - new_data_start);

                old_run = (old_data_start + mboi.byte_offset);

                u8 old_size = sizeof(MBOLevel) + (((MBOLevel*)old_run)->order_count) * sizeof(u32);
                memcpy(new_run, old_run, old_size);
                u8 new_size = old_size;
                new_run += new_size;
            }

            u8 new_level = current_level;

            //u8 highest_untouched_index = direction == 1 ?  untouched_above : untouched_below;

            current_level = highest_untouched_index + 1;
            if (exact_level_wipe) {
                //current_level = highest_untouched_index + 1;
                
            } else if (partial_fill) {
                // insert an ask or a bid, doesn' tmatter the hi_bid_index update will determine
                new_mbo->levels[current_level].byte_offset = (new_run - new_data_start);
                // this is where we need to insert the new limit order
                new_mbo->levels[current_level].price = price;
                new_mbo->levels[current_level].quantity = remaining_quantity;

                MBOLevel* init = ((MBOLevel*)new_run);
                init->order_count = 1;
                init->order_ids[0] = order_id;
                new_run += sizeof(MBOLevel) + init->order_count * sizeof(u32);

                //current_level = highest_untouched_index;
                if (direction == 1) {
                    //buy, we just updated highest bid
                    new_mbo->hi_bid_index = current_level;
                } else {
                    //sell, we just updated lowest ask
                    new_mbo->hi_bid_index = current_level - 1;
                }
                    
            } else if (modified_level) {
                // a whole bunch of bullshit

                // the scenario where we partially fill a level
                // even worse, maybe partially fill an order
                MBOIndex* mod_index = &(old_mbo->levels[modified_level_index]);

                new_mbo->levels[current_level].byte_offset = (new_run - new_data_start);
                // this is where we need to insert the new limit order
                new_mbo->levels[current_level].price = price;
                new_mbo->levels[current_level].quantity = mod_index->quantity - remaining_quantity;

                old_run = (old_data_start + mod_index->byte_offset);
                
                // now go through it and see which orders we fill
                MBOLevel* mod_level = (MBOLevel*)old_run;

                u32 remaining_orders_on_level = mod_level->order_count;

                u8 i = 0;
                for (; i < mod_level->order_count; i++) {
                    u32 order_id = mod_level->order_ids[i];
                    Order* prev_order = (Order*)fl_get(orders, order_id);
            
                    u16 order_quantity = prev_order->quantity;
                    if (order_quantity > remaining_quantity) {
                        // modify order directly, maybe send partial fill notification
                        in->quantity -= remaining_quantity;
                        break;
                    } else {
                        // fill order entirely
                        remaining_orders_on_level--;
                        remaining_quantity -= order_quantity;
                    }
                }

                MBOLevel* init = ((MBOLevel*)new_run);
                init->order_count = remaining_orders_on_level;
                for (u8 j = i; j < mod_level->order_count; i++) {
                    init->order_ids[j-i] = mod_level->order_ids[j];
                }

                new_run += sizeof(MBOLevel) + (mod_level->order_count - i) * sizeof(u32);
                
                    
                if (direction == 1) {
                    new_mbo->hi_bid_index = current_level - 1;
                } else {

                    new_mbo->hi_bid_index = current_level;
                }

                //current_level = highest_untouched_index;
            } 



            for (; current_level < old_mbo->level_count; current_level++) {
                MBOIndex mboi = old_mbo->levels[new_level];

                new_mbo->levels[current_level].price = mboi.price;
                new_mbo->levels[current_level].quantity = mboi.quantity;
                new_mbo->levels[current_level].byte_offset = (new_run - new_data_start);

                old_run = (old_data_start + mboi.byte_offset);

                u8 old_size = sizeof(MBOLevel) + (((MBOLevel*)old_run)->order_count) * sizeof(u32);
                memcpy(new_run, old_run, old_size);
                u8 new_size = old_size;
                new_run += new_size;

                new_level++;
            }


                if (new_mbo->level_count == 0) 
                    new_mbo->hi_bid_index = MAX_U8;





            // buy 
            // 0------highest bid remains the same, then rows wiped or highest bid updated, then remanining asks remain the same
            // sell
            //  lowest ask ---- levelcount remain the same

        } else {
            // it is not marketable, easier case

            //u8 insert_before = start_search;
            u8 price_level_exists = 0;
            u8 match_level = 0;

            for(; start_search != bottom; start_search -= multiplier) {
                if (old_mbo->levels[start_search-multiplier].price == price) {
                    match_level = start_search-multiplier;
                    price_level_exists = 1;
                    break;
                }
            }

            // now we know the size of MBOIndex at least

            new_mbo->hi_bid_index = old_mbo->hi_bid_index;
            if (direction == 1 && !price_level_exists)
                new_mbo->hi_bid_index++;

                    new_mbo->level_count = old_mbo->level_count;
            if (!price_level_exists)
                new_mbo->level_count++;

            void* old_data_start = new_mbo_raw + sizeof(MBO) + new_mbo->level_count * sizeof(MBOIndex);
            void* new_data_start = new_mbo_raw + sizeof(MBO) + new_mbo->level_count * sizeof(MBOIndex);

            void* new_run = new_data_start;
            void* old_run = old_data_start;

            if (price_level_exists) {
                for (u8 current_level = 0; current_level < new_mbo->level_count; current_level++) {
                    MBOIndex mboi = old_mbo->levels[current_level];

                    new_mbo->levels[current_level].price = mboi.price;
                    new_mbo->levels[current_level].quantity = mboi.quantity;
                    new_mbo->levels[current_level].byte_offset = (new_run - new_data_start);

                    old_run = (old_data_start + mboi.byte_offset);

                    u8 old_size = sizeof(MBOLevel) + (((MBOLevel*)old_run)->order_count) * sizeof(u32);
                    memcpy(new_run, old_run, old_size);
                    u8 new_size = old_size;

                    if (current_level == match_level) {
                        new_mbo->levels[current_level].quantity += quantity;

                        ((MBOLevel*)new_run)->order_count++;
                        u32* ptr = (u32*)(new_run + old_size);
                        *ptr = order_id;
                        new_size += sizeof(u32);
                    }
                    new_run += new_size;
                }
            } else {
                // with insert
                // 0 until we find, then one to keep track of where we are
                u8 found_delta = 0;
                for (u8 current_level = 0; current_level < new_mbo->level_count; current_level++) {
                    MBOIndex mboi = old_mbo->levels[current_level - found_delta];

                    u8 new_size;
                    new_mbo->levels[current_level].byte_offset = (new_run - new_data_start);
                    if (found_delta == 0 && mboi.price > price) {
                        // this is where we need to insert the new limit order
                        new_mbo->levels[current_level].price = price;
                        new_mbo->levels[current_level].quantity = quantity;

                        MBOLevel* init = ((MBOLevel*)new_run);
                        init->order_count = 1;
                        init->order_ids[0] = order_id;
                        new_size = sizeof(MBOLevel) + init->order_count * sizeof(u32);


                        found_delta = 1;
                    } else {
                        new_mbo->levels[current_level].price = mboi.price;
                        new_mbo->levels[current_level].quantity = mboi.quantity;

                        old_run = (old_data_start + mboi.byte_offset);

                        u8 old_size = sizeof(MBOLevel) + (((MBOLevel*)old_run)->order_count) * sizeof(u32);
                        memcpy(new_run, old_run, old_size);
                        new_size = old_size;
                    }
                    new_run += new_size;
                }
            }
        }



    }



    //actual_size = 1000;

// much much later
    bs_resize(mbo_bs, actual_size);
}

