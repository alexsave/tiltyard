#include <string.h>
#include <stdio.h>

#include "ob.h"

#include "types.h"
#include "constants.h"
#include "bs.h"
#include "cb.h"
#include "order.h"
#include "fl.h"

void* _data_start(void* mbo_raw){
    return mbo_raw + sizeof(MBO) + ((MBO*)(mbo_raw))->level_count * sizeof(MBOIndex);
}

void mbo_dump(void* mbo_raw) {
    //if(1) return;
    MBO* mbo = (MBO*)mbo_raw;
    printf("mbo at %p\n", mbo_raw);

    printf("===raw DUMP===\n");
    for (u8 i = 0; i < 32;i++){
        //printf("%u\n", *(u8*)(mbo_raw+i));
    }

    printf("===MBO DUMP===\n");
    printf("MBO with %u levels and hi bid index %u\n", mbo->level_count, mbo->hi_bid_index);
    u32 last_byte_offset = MAX_U16;
    for (u16 i = 0; i < mbo->level_count; i++) {
        MBOIndex mboi = mbo->levels[i];

        if (i == mbo->hi_bid_index)
            printf("[");
        else 
            printf(" ");
        printf("level\t%uc with quantity\t%u at offset\t%u", mboi.price, mboi.quantity, mboi.byte_offset);
        if (mboi.byte_offset == last_byte_offset){
            printf("consecutive level byte offsets were equal, impossible\n");

            //exit(1);
        }
        last_byte_offset = mboi.byte_offset;
        if (i == mbo->hi_bid_index)
            printf("] - highest bid\n");
        else 
            printf("\n");
    }


    void* data_start = _data_start(mbo_raw);
    printf("level data\n");

    for (u16 i = 0; i < mbo->level_count; i++) {
        MBOIndex mboi = mbo->levels[i];
        u32 byte_offset = mboi.byte_offset;
        //printf("new mbol %p\n", (void*)(data_);
        MBOLevel* mbol = (MBOLevel*)(data_start + byte_offset);
        for(u8 j = 0; j < 8; j++){
            //printf("%u\n", *(u8*)(data_start+byte_offset+j));
        }

        printf("%uc\t with %u orders\t", mboi.price, mbol->order_count);

        // we crash right after this i think

        //printf("start of mbol %p\n", (data_start+byte_offset));
        //printf("start of order_ids %p\n", &(mbol->order_ids));

        for (u16 j = 0; j < mbol->order_count; j++) {
            u32 oid = mbol->entries[j].order_id;
            //if (oid > 43440){
            //printf("some ridiculously large id value already\n");
            //exit(1);
            //}
            printf("%ush #%u\t", mbol->entries[j].quantity, mbol->entries[j].order_id);
        }    
        printf("\n");
    }
    printf("===MBO DUMP END===\n");

}

u32 _data_start_offset(u16 level_count) {
    return sizeof(MBO) + level_count * sizeof(MBOIndex);
}


u32 _mbo_level_size(u16 order_count) {
    return sizeof(MBOLevel) + order_count * sizeof(MBOEntry);
}

// hope that works
MBORunner * mbor_init(MBO* mbo) {
    MBORunner * mbor = malloc(sizeof(MBORunner));
    
    mbor->mbo = mbo;
    mbor->metadata = mbo->levels;
    // for this one, it's critical that level_count is initialized properly
    mbor->data_start = (((void*)mbo) + _data_start_offset(mbo->level_count));
    mbor->level = mbor->data_start;
    mbor->index = 0;
    return mbor;
}

void mbo_copy_level(MBORunner* old, MBORunner* new){
    MBOIndex* mboi = old->metadata;

    new->metadata->price = mboi->price;
    new->metadata->quantity = mboi->quantity;
    new->metadata->byte_offset = ((void*)(new->level)) - new->data_start;

    MBOLevel* old_run = old->level;

    // techincally &(oldrun) == &(oldrun->order_Count)
    // but maybe not something to rely on
    u16 old_size = _mbo_level_size(old_run->order_count);
    memcpy(new->level, old_run, old_size);
}

// copy append
void mbo_copapp_level(MBORunner* old, MBORunner* new, u32 order_id, u32 quantity) {
    mbo_copy_level(old, new);

    // AND THEN

    // jump past to write the proper entry
    MBOEntry* entry = (MBOEntry*)(((void*)(new->level)) + _mbo_level_size(new->level->order_count));
    entry->order_id = order_id;
    entry->quantity = quantity;
    new->level->order_count++;

    new->metadata->quantity += quantity;
    if(new->metadata->quantity == 0) {
        printf("quantity ended up 0\n");
        mbo_dump(old->mbo);
        exit(1);
    }
}

void mbo_insert_level(MBORunner* new, u32 order_id, u16 price, u32 quantity) {
    new->metadata->price = price;
    new->metadata->quantity = quantity;
    new->metadata->byte_offset = ((void*)(new->level)) - new->data_start;
    
    MBOLevel* level = new->level;
    level->order_count = 1;
    level->entries[0].order_id = order_id;
    level->entries[0].quantity = quantity;
}

void mbo_to_index(MBORunner* run, u16 index) {
    run->index = index;
    run->metadata = run->mbo->levels + index; //levels[index]
    run->level = ((void*)(run->data_start)) + run->metadata->byte_offset;
}

void mbo_partial_fill_insert(MBORunner* old, MBORunner* new, u16 price, u32 remaining_quantity) {
    new->metadata->price = old->metadata->price;
    new->metadata->quantity = old->metadata->quantity - remaining_quantity;
    new->metadata->byte_offset = ((void*)(new->level)) - new->data_start;

    MBOLevel* mod_level = old->level;

    u32 remaining_orders_on_level = mod_level->order_count;

    u8 partial_fill = 0;

    u16 i = 0;
    for (; i < mod_level->order_count; i++) {
        u32 prev_order_id = mod_level->entries[i].order_id;
        //Order* prev_order = (Order*)fl_get(orders, prev_order_id);
        // that's what we USED to do. but really we just modify the order book here
        // and somehow notify that THIS MUCH of THIS ORDER was filled, passing it up to serer.c to modify the orders themselves
        // which also solves the whole partial thing
        
        MBOEntry * prev_order = mod_level->entries + i;
    
        u32 order_quantity = prev_order->quantity;
        if (order_quantity > remaining_quantity) {
            //prev_order->quantity -= remaining_quantity;
            // CRTICALLY, SOMEHOW LOG THIS BACK TO SERVER.C
            printf("PARTIAL FILL %u\n", remaining_quantity);
            partial_fill = 1;
            break;
        } else {
            printf("FILL %u\n", order_quantity);
            // fill order entirely
            remaining_orders_on_level--;
            remaining_quantity -= order_quantity;
            // ALSO GET THIS BACK TO SERVER.C
            //cb_queue(fills, prev_order_id);
        }
    }

    MBOLevel * init = new->level;

    u16 j = i;

    if (partial_fill) {
        // I feel like we were previously forgetting to write in the case of partial fill, not anymore
        // first need to append that last one in, but NOT modify the one on server
        MBOEntry * prev_order = mod_level->entries + i;
        init->entries[0].order_id = prev_order->order_id;
        init->entries[0].quantity = prev_order->quantity - remaining_quantity;

        j++;
    }

    init->order_count = remaining_orders_on_level;
    for (; j < mod_level->order_count; j++){
        init->entries[j-i].order_id = mod_level->entries[j].order_id;
        init->entries[j-i].quantity = mod_level->entries[j].quantity;
    }
}

// this COULD return somethign based on if we have more or not
void mbo_jump(MBORunner* run) {
    // the order_count MUST be written at this point
    run->level = ((void*)run->level) + _mbo_level_size(run->level->order_count);
    // if this is of type MBOIndex it will work right?
    run->metadata++;
    //naming is hard
    run->index++;
}

// were doing a lot just to figure out the size
// it might be better to guess the next size we need, then only "lock it in" once we have the size
// will need a change to bs
// but not that hard, just need to update bs->metadata[(bs->md_end-1)%bs->md_capacity].size = size;

// fill holder for the trades we make
// it's not up to this OB modifier to handle fills just to log them really
u32 ob_limit(u32 order_id, FL* orders, u32 mbo_handle, BS* mbo_bs, u16 ref_count, CB* fills, u32* partial_fill_id, u32* partial_fill_q) {
    Order* in = (Order*)fl_get(orders, order_id);
    u8 direction = (in->flags >> BUY_DIRECTION_BIT) & 1;
    u16 price = in->price;
    u32 quantity = in->quantity;

    // if we have cancelreplace trade, all this gets fucked
    // the replace part of it can just be thought of as a regular order
    // but now on top of all this we need to be constantly vigilant of the canclled order
    // which could possibly wipe a level
    // we have a shortcut in that we can check the order price
    // if the cancel order price is not in teh index, it doesnt exist
    // also if the cancel order client id does not match the requester client id, it's probably already filled or an invalid request
    // they should've had a chance to see that their order was rejected or filled or whatever
    // and we can check index offset to see if their cancel is in the level it says its in
    // if the id is not there, we can skip the cancel part

    // now lets say it is there
    // wtf do we do
    // 2 options
    // 1 - decrease the index level q by order q, decrement level order count by 1, update offsets accordingly
    // 2 - wipe out the level, update hi_bid_index accordingly, update level size, update offset accordingly

    // actually there's a third
    // if they want to cancel and replace AT THE SAME PRICE, that's a modify
    // we can allow them to shrink size but nothing else
    // this one is actually much easier and we just swap the order id in the OB without changing size at all, but we do need to update level quantity
    // let me sleep on this

    // step one - calculate max possible new mbo size
    u32 old_size = mbo_bs->metadata[mbo_handle].size;
    u32 max_new_size = old_size + sizeof(MBOIndex) + sizeof(MBOLevel) + sizeof(MBOEntry);

    u32 actual_size = max_new_size;

    void* new_mbo_raw;
    u32 unused = bs_reserve(mbo_bs, max_new_size, ref_count, &new_mbo_raw);

    MBO* new_mbo = (MBO*)new_mbo_raw;

    //printf("start of bs store at %p\n", mbo_bs->store);
    //printf("new mbo at %p\n", new_mbo_raw);

    void* old_mbo_raw = bs_get_no_ref(mbo_bs, mbo_handle);
    MBO* old_mbo = (MBO*)old_mbo_raw;

    u16 hi_bid_index = old_mbo->hi_bid_index;
    u16 lo_ask_index = hi_bid_index + 1;

    u16 start_search;
    i16 multiplier = 0;
    u16 bottom;
    u16 top;

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
        has_opponents = hi_bid_index != MAX_U16;
    }

    if (has_opponents && (multiplier)*(old_mbo->levels[start_search].price) <= multiplier*price) {
        //printf("we have a marketable limit order\n");
        //printf("starting search from %u and going %u\n", start_search, multiplier);
        // yes it is marketable, at least part of it can be matched immediately

        u32 remaining_quantity = quantity;

        // EXCLUSIVE NOW, everything outside of these is untouched
        u16 untouched_below = start_search;
        u16 untouched_above = top;

        u8 partial_fill = 0;

        //bid for buy, ask for sell

        u8 exact_level_wipe = 0;

        u8 modified_level = 0;
        u16 modified_level_index = 0;

        u16 current_level;

        // first figure out what rows we need to get rid of
        for(current_level = start_search; ; current_level += multiplier) {

            //printf("current level %u\n", current_level);
            MBOIndex mboi = old_mbo->levels[current_level];

            if (remaining_quantity > 0 && (multiplier)*mboi.price > (multiplier)*price) {
                //printf("not enough to fill limit, leaving as open bid or ask\n");

                // stop due to limit price - possibly partial fill
                untouched_above = current_level - multiplier;
                partial_fill = 1;
                break;
            }

            u32 level_quantity = mboi.quantity;

            if (level_quantity >= remaining_quantity){
                untouched_above = current_level;
            }

            if (level_quantity <= remaining_quantity) {
                // better yet we just do it here
                void* old_run = (_data_start((void*)old_mbo) + mboi.byte_offset);

                MBOLevel* mod_level = (MBOLevel*)old_run;
                u32 remaining_orders_on_level = mod_level->order_count;
                for (u16 i = 0; i < mod_level->order_count; i++) {
                    u32 prev_order_id = mod_level->entries[i].order_id;
                    cb_queue(fills, prev_order_id);
                }

            }

            if (level_quantity > remaining_quantity) {
                // this order will be entirely filled on this level, making this lowest ask/highets bid

                modified_level = 1;
                modified_level_index = current_level;
                //printf("order will partially eat into level %u with remaining quantity %u vs level quantity %u\n", mboi.price, remaining_quantity, level_quantity);

                break;
            } else if(level_quantity == remaining_quantity) {
                //printf("limit filled exactly at this level\n");
                remaining_quantity -= level_quantity;
                exact_level_wipe = 1;
                //printf("found exact remaining quantity at level %u %u\n", current_level, mboi.price);
                // this fill will be entirely filled on this level, mking NEXT highest lowest ask

                break;
            } else {
                //printf("takinga  bit of quantity\n");
                remaining_quantity -= level_quantity;

            }

            if (current_level == top) {
                if (remaining_quantity > 0) {
                    partial_fill = 1;
                    untouched_above = top;
                    // we need to just add a new one with remaining quantity
                }

                break;
            }
        }

        // let's figure out how many we need

        u16 lowest_untouched_index = direction == 1 ? untouched_below : untouched_above;
        u16 highest_untouched_index = direction == 1 ? untouched_above : untouched_below;

        new_mbo->level_count = (lowest_untouched_index - 0) + (old_mbo->level_count - highest_untouched_index - 1) + (partial_fill | modified_level);

        void* new_run = _data_start(new_mbo_raw);
        //printf("got new run\n");

        // CHANGING TO EXCLUSIVE

        MBORunner* old_runner = mbor_init(old_mbo);
        MBORunner* new_runner = mbor_init(new_mbo);
        u16 i = 0;
        for ( ; i < lowest_untouched_index; i++) {
            mbo_copy_level(old_runner, new_runner);
            mbo_jump(old_runner);
            mbo_jump(new_runner);
        }

        u16 new_current_level = i;
        u16 old_current_level = i;

        if (exact_level_wipe) {
            new_mbo->hi_bid_index = new_current_level - 1;
            in->quantity = 0;

        } else if (partial_fill) {
            // the INCOMING order is partially filled, thus left as a standing order

            if (direction == 1) 
                //buy, we just updated highest bid
                new_mbo->hi_bid_index = new_current_level;
             else 
                //sell, we just updated lowest ask
                new_mbo->hi_bid_index = new_current_level - 1;

            // this is where we need to insert the new limit order
            mbo_insert_level(new_runner, order_id, price, remaining_quantity);
            mbo_jump(new_runner);

            in->quantity = remaining_quantity;
        } else if (modified_level) {
            in->quantity = 0;
            if (direction == 1) 
                new_mbo->hi_bid_index = new_current_level - 1;
            else 
                new_mbo->hi_bid_index = new_current_level;
            
            // first lets get the oldrunner in the right mindset
            mbo_to_index(old_runner, modified_level_index);

            mbo_partial_fill_insert(old_runner, new_runner, price, remaining_quantity);
            
            mbo_jump(new_runner);
            


            //printf("partial fill and insert\n");
        } 


        for (old_current_level = highest_untouched_index+1; old_current_level < old_mbo->level_count; old_current_level++) {
            mbo_copy_level(old_runner, new_runner);
            mbo_jump(old_runner);
            mbo_jump(new_runner);
        }

        // we can skip all this silliness above
        if (new_mbo->level_count == 0) 
            new_mbo->hi_bid_index = MAX_U16;

    } else {
        // it is not marketable, easier case
        u8 price_level_exists = 0;
        u16 match_level = 0;

        if (direction == 1 && hi_bid_index != MAX_U16 && old_mbo->levels[hi_bid_index].price < price) {
            //special case - nothing will match, this between hi bid and low ask
        } else {
            for(; start_search != bottom; start_search -= multiplier) {
                //printf("checking  %u, %u against %u\n", 
                //old_mbo->levels[start_search-multiplier].price,(start_search-multiplier), price);

                if (old_mbo->levels[(u16)(start_search-multiplier)].price == price) {
                    match_level = start_search-multiplier;
                    price_level_exists = 1;
                    break;
                }
            }
        }

        // now we know the size of MBOIndex at least

        new_mbo->hi_bid_index = old_mbo->hi_bid_index;
        if (direction == 1 && !price_level_exists) {
            new_mbo->hi_bid_index++;
        }

        new_mbo->level_count = old_mbo->level_count;
        if (!price_level_exists)
            new_mbo->level_count++;

        void* new_run = _data_start(new_mbo_raw);

        MBORunner * old_runner = mbor_init(old_mbo);
        MBORunner * new_runner = mbor_init(new_mbo);

        if (price_level_exists) {
            for (u16 i = 0; i < new_mbo->level_count; i++) {
                if (i == match_level) 
                    mbo_copapp_level(old_runner, new_runner, order_id, quantity);
                else 
                    mbo_copy_level(old_runner, new_runner);
                mbo_jump(old_runner);
                mbo_jump(new_runner);
            }

        } else {
            u8 found = 0;
            for (u16 i = 0; i < old_mbo->level_count; i++) {
                if (!found && old_runner->metadata->price > price){
                    mbo_insert_level(new_runner, order_id, price, quantity);
                    mbo_jump(new_runner);
                    found = 1;
                }
                mbo_copy_level(old_runner, new_runner);
                mbo_jump(old_runner);
                mbo_jump(new_runner);
            }
            if (found == 0){
                mbo_insert_level(new_runner, order_id, price, quantity);
                mbo_jump(new_runner);
            }

        }
    }

    // even better, we have everythign we need to calculate actual_size
    // as new_runner is left past the end due to jumps...
    //actual_size = ((void*)(new_runner->level)) - new_mbo_raw;
    // ah not yet

    void* new_data_start = _data_start(new_mbo_raw);
    if (new_mbo->level_count == 0) {
        actual_size = new_data_start - new_mbo_raw;
    } else {
        void* mbol = new_mbo->levels[new_mbo->level_count-1].byte_offset + new_data_start;
        actual_size = (mbol + _mbo_level_size(((MBOLevel*)mbol)->order_count)) - new_mbo_raw;
    }


    // much much later
    u8 resize_status = bs_resize(mbo_bs, actual_size);
    if (resize_status){
        u32 max_new_size = old_size + sizeof(MBOIndex) + sizeof(MBOLevel) + sizeof(MBOEntry);
        printf("predicted size was %u, then requested to resize to %U\n", max_new_size, actual_size);
        printf("old mbo\n");
        mbo_dump(old_mbo);
        printf("new mbo\n");
        mbo_dump(new_mbo);
        exit(1);
    }
    return unused;
}

