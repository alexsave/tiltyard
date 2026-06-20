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

void* _data_start(void* mbo_raw){
    return mbo_raw + sizeof(MBO) + ((MBO*)(mbo_raw))->level_count * sizeof(MBOIndex);
}

u16 _mbo_level_size(u16 order_count) {
    return sizeof(MBOLevel) + order_count * sizeof(u32);
}

void mbo_dump(void* mbo_raw) {
    MBO* mbo = (MBO*)mbo_raw;


    printf("===raw DUMP===\n");
    for (u8 i = 0; i < 32;i++){
        //printf("%u\n", *(u8*)(mbo_raw+i));
    }

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


    void* data_start = _data_start(mbo_raw);
    printf("level data\n");

    for (u8 i = 0; i < mbo->level_count; i++) {
        MBOIndex mboi = mbo->levels[i];
        u8 byte_offset = mboi.byte_offset;
        //printf("new mbol %p\n", (void*)(data_);
        MBOLevel* mbol = (MBOLevel*)(data_start + byte_offset);
        for(u8 j = 0; j < 8; j++){
            //printf("%u\n", *(u8*)(data_start+byte_offset+j));
        }
        printf("%uc\t with %u orders\t", mboi.price, mbol->order_count);
        //printf("start of mbol %p\n", (data_start+byte_offset));
        //printf("start of order_ids %p\n", &(mbol->order_ids));

        for (u8 j = 0; j < mbol->order_count; j++) {
            printf("#%u\t", mbol->order_ids[j]);
        }    
        printf("\n");
    }
    printf("===MBO DUMP END===\n");

}



// were doing a lot just to figure out the size
// it might be better to guess the next size we need, then only "lock it in" once we have the size
// will need a change to bs
// but not that hard, just need to update bs->metadata[(bs->md_end-1)%bs->md_capacity].size = size;

u32 ob_limit(u32 order_id, FL* orders, u32 mbo_handle, BS* mbo_bs) {
    Order* in = (Order*)fl_get(orders, order_id);
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

    u8 has_opponents = 1;

    // buying
    if (direction == 1) {
        start_search = lo_ask_index;
        multiplier = 1;
        //more deep
        bottom = 0;
        top = old_mbo->level_count - 1;

        has_opponents = lo_ask_index < old_mbo->level_count;
    } else {
        start_search = hi_bid_index;
        multiplier = -1;
        bottom = old_mbo->level_count-1;
        top = 0;
        has_opponents = hi_bid_index != MAX_U8;
    }

    if (has_opponents && (multiplier)*(old_mbo->levels[start_search].price) <= multiplier*price) {
        printf("we have a marketable limit order\n");
        printf("starting search from %u and going %u\n", start_search, multiplier);
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

        u8 final_level_count = 0;

        // first figure out what rows we need to get rid of
        for(current_level = start_search; ; current_level += multiplier) {

            printf("current level %u\n", current_level);
            MBOIndex mboi = old_mbo->levels[current_level];
            MBOIndex next = old_mbo->levels[current_level+multiplier];
            //if(mboi.price == 2 || next.price == 2){
            //printf("this is where price 2 comes from")
            //}
            if (remaining_quantity > 0 && (multiplier)*mboi.price > (multiplier)*price) {
                printf("not enough to fill limit, leaving as open bid or ask\n");
                level_count++;

                // stop due to limit price - possibly partial fill
                untouched_above = current_level - multiplier;// test this out in scenario with limit between ask and bid gap
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
                printf("found exact remaining quantity at level %u %u\n", current_level, mboi.price);
                untouched_above = current_level;
                // this fill will be entirely filled on this level, mking NEXT highest lowest ask
                //next_lowest_ask = 
                their_side = next.price;
                level_count--;
                break;
            } else {
                remaining_quantity -= level_quantity;
                level_count--;
            }



            if (start_search == top) {
                // do something maybe?

                break;
            }

        }

        // let's figure out how many we need

        u8 lowest_untouched_index = direction == 1 ? untouched_below : untouched_above;

        u8 highest_untouched_index = direction == 1 ?  untouched_above : untouched_below;
        printf("lowest untouched index %u highest_untouched index %u\n", lowest_untouched_index, highest_untouched_index);

        new_mbo->level_count = (lowest_untouched_index - 0) + (old_mbo->level_count - highest_untouched_index - 1) + (partial_fill | modified_level);


        void* old_data_start = _data_start(old_mbo_raw);
        void* new_data_start = _data_start(new_mbo_raw);

        if((*((u8*)(new_mbo_raw +2))) == 2){
            printf("initial first price level set to 2 suddently\n");
        }

        void* new_run = new_data_start;
        void* old_run = old_data_start;


        printf("ods %p nds %p nr %p or %p\n", old_data_start, new_data_start, new_run, old_run);

        // CHANGING TO EXCLUSIVE

        //if (lowest_untouched_level 

        current_level = 0;
        for (; current_level < lowest_untouched_index; current_level++) {
            MBOIndex mboi = old_mbo->levels[current_level];
            printf("going through untouched low %u %u\n", mboi.price, mboi.quantity);
            //mbo_dump(new_mbo);

            new_mbo->levels[current_level].price = mboi.price;
            new_mbo->levels[current_level].quantity = mboi.quantity;
            new_mbo->levels[current_level].byte_offset = (new_run - new_data_start);

            old_run = (old_data_start + mboi.byte_offset);

            u8 old_size = sizeof(MBOLevel) + (((MBOLevel*)old_run)->order_count) * sizeof(u32);

            printf("copying from %p to %p\n", old_run, new_run);

            if((*((u8*)(new_mbo_raw +2))) == 2){
                printf("first price level set to 2 before memcopy\n");
            }
            memcpy(new_run, old_run, old_size);
            if((*((u8*)(new_mbo_raw +2))) == 2){
                printf("first price level set to 2 after memcpy\n");
            }


            u8 new_size = old_size;
            new_run += new_size;
            final_level_count++;
        }

        u8 new_current_level = current_level;
        u8 old_current_level = current_level;


        u8 new_level = current_level;

        //u8 highest_untouched_index = direction == 1 ?  untouched_above : untouched_below;

        current_level = highest_untouched_index + 1;
        if (exact_level_wipe) {
            //current_level = highest_untouched_index + 1;
            new_mbo->hi_bid_index = new_current_level - 1;

        } else if (partial_fill) {
            printf("we got a partial fill\n");
            // insert an ask or a bid, doesn' tmatter the hi_bid_index update will determine
            new_mbo->levels[new_current_level].byte_offset = (new_run - new_data_start);
            // this is where we need to insert the new limit order
            new_mbo->levels[new_current_level].price = price;
            new_mbo->levels[new_current_level].quantity = remaining_quantity;

            MBOLevel* init = ((MBOLevel*)new_run);
            init->order_count = 1;
            init->order_ids[0] = order_id;
            new_run += sizeof(MBOLevel) + init->order_count * sizeof(u32);

            //current_level = highest_untouched_index;
            if (direction == 1) {
                //buy, we just updated highest bid
                new_mbo->hi_bid_index = new_current_level;
            } else {
                //sell, we just updated lowest ask
                new_mbo->hi_bid_index = new_current_level - 1;
            }
            new_current_level++;
            final_level_count++;

        } else if (modified_level) {
            // a whole bunch of bullshit

            // the scenario where we partially fill a level
            // even worse, maybe partially fill an order
            MBOIndex* mod_index = &(old_mbo->levels[modified_level_index]);

            new_mbo->levels[new_current_level].byte_offset = (new_run - new_data_start);
            // this is where we need to insert the new limit order
            new_mbo->levels[new_current_level].price = mod_index->price;
            new_mbo->levels[new_current_level].quantity = mod_index->quantity - remaining_quantity;

            old_run = (old_data_start + mod_index->byte_offset);

            // now go through it and see which orders we fill
            MBOLevel* mod_level = (MBOLevel*)old_run;

            u32 remaining_orders_on_level = mod_level->order_count;

            u8 i = 0;
            for (; i < mod_level->order_count; i++) {
                printf("going throuh orders %u\n", i);
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
            for (u8 j = i; j < mod_level->order_count; j++) {
                //printf("going throuh orders %u\n", j);
                init->order_ids[j-i] = mod_level->order_ids[j];
            }

            new_run += sizeof(MBOLevel) + (mod_level->order_count - i) * sizeof(u32);


            if (direction == 1) 
                new_mbo->hi_bid_index = new_current_level - 1;
            else 
                new_mbo->hi_bid_index = new_current_level;

            //current_level = highest_untouched_index;
            new_current_level++;
            level_count++;

            final_level_count++;
        } 



        for (old_current_level = highest_untouched_index+1; old_current_level < old_mbo->level_count; old_current_level++) {
            printf("going through untouched above\n");
            MBOIndex mboi = old_mbo->levels[old_current_level];

            new_mbo->levels[new_current_level].price = mboi.price;
            new_mbo->levels[new_current_level].quantity = mboi.quantity;
            new_mbo->levels[new_current_level].byte_offset = (new_run - new_data_start);

            old_run = (old_data_start + mboi.byte_offset);

            u8 old_size = sizeof(MBOLevel) + (((MBOLevel*)old_run)->order_count) * sizeof(u32);
            memcpy(new_run, old_run, old_size);
            u8 new_size = old_size;
            new_run += new_size;

            new_level++;
            new_current_level++;
            final_level_count++;
        }

        new_mbo->level_count = final_level_count;

        if (new_mbo->level_count == 0) {
            new_mbo->hi_bid_index = MAX_U8;
        }



        // buy 
        // 0------highest bid remains the same, then rows wiped or highest bid updated, then remanining asks remain the same
        // sell
        //  lowest ask ---- levelcount remain the same

    } else {
        // it is not marketable, easier case

        //u8 insert_before = start_search;
        u8 price_level_exists = 0;
        u8 match_level = 0;

        if (direction == 1 && hi_bid_index != MAX_U8 && old_mbo->levels[hi_bid_index].price < price) {
            //special case - nothing will match, this between hi bid and low ask
        } else {


            for(; start_search != bottom; start_search -= multiplier) {
                printf("checking  %u, %u against %u\n", 
                        old_mbo->levels[start_search-multiplier].price,(start_search-multiplier), price);

                if (old_mbo->levels[(u8)(start_search-multiplier)].price == price) {
                    match_level = start_search-multiplier;
                    price_level_exists = 1;
                    break;
                }
            }
        }


        // now we know the size of MBOIndex at least

        new_mbo->hi_bid_index = old_mbo->hi_bid_index;
        if (direction == 1 && !price_level_exists)
            new_mbo->hi_bid_index++;

        new_mbo->level_count = old_mbo->level_count;
        if (!price_level_exists)
            new_mbo->level_count++;

        void* old_data_start = _data_start(old_mbo_raw);
        void* new_data_start = _data_start(new_mbo_raw);

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
            printf("nonmarketable limit order with insert\n");


            u8 current_new_level = 0;

            // 0 until we find, then one to keep track of where we are
            u8 found = 0;

            for (u8 current_level = 0; current_level < old_mbo->level_count; current_level++) {
                MBOIndex mboi = old_mbo->levels[current_level];
                u8 new_size;
                new_mbo->levels[current_new_level].byte_offset = (new_run - new_data_start);

                if (found == 0 && mboi.price > price) {
                    printf("inserting new level\n");
                    // this is where we need to insert the new limit order
                    new_mbo->levels[current_new_level].price = price;
                    new_mbo->levels[current_new_level].quantity = quantity;
                    new_mbo->levels[current_new_level].byte_offset = (new_run - new_data_start);
                    MBOLevel* mbol = (MBOLevel*)new_run;
                    mbol->order_count = 1;
                    mbol->order_ids[0] = order_id;
                    new_size = _mbo_level_size(1);

                    found = 1;
                    new_run += new_size;
                    current_new_level++;
                }

                printf("coyping existing level\n");

                new_mbo->levels[current_new_level].price = mboi.price;
                new_mbo->levels[current_new_level].quantity = mboi.quantity;
                new_mbo->levels[current_new_level].byte_offset = (new_run - new_data_start);

                old_run = (old_data_start + mboi.byte_offset);
                u8 old_size = _mbo_level_size(((MBOLevel*)old_run)->order_count);

                memcpy(new_run, old_run, old_size);
                new_size = old_size;
                new_run += new_size;
                current_new_level++;
            }




            if(found == 0){
                printf("inserting new level\n");
                // this is where we need to insert the new limit order
                new_mbo->levels[current_new_level].price = price;
                new_mbo->levels[current_new_level].quantity = quantity;
                new_mbo->levels[current_new_level].byte_offset = (new_run - new_data_start);
                MBOLevel* mbol = (MBOLevel*)new_run;
                mbol->order_count = 1;
                mbol->order_ids[0] = order_id;

                found = 1;
                current_new_level++;
                new_run += _mbo_level_size(1);
            }



        }
    }

}

    return unused;


//actual_size = 1000;

// much much later
bs_resize(mbo_bs, actual_size);
}

