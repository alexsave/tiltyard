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

void _write_level(MBO* new_mbo, u8 new_current_level, u16 price, u16 quantity, void* new_run){
    void* new_data_start = _data_start(new_mbo);

    new_mbo->levels[new_current_level].price = price;
    new_mbo->levels[new_current_level].quantity = quantity;
    new_mbo->levels[new_current_level].byte_offset = new_run - new_data_start;
}

void _copy_level_and_jump(MBO* old_mbo, u8 old_current_level, MBO* new_mbo, u8* new_current_level, void** new_run){
    MBOIndex mboi = old_mbo->levels[old_current_level];

    // assuming level count is set correctly
    _write_level(new_mbo, *new_current_level, mboi.price, mboi.quantity, *new_run);

    void* old_run = (_data_start((void*)old_mbo) + mboi.byte_offset);

    u8 old_size = _mbo_level_size((((MBOLevel*)old_run)->order_count));
    memcpy(*new_run, old_run, old_size);
    *new_run = (*new_run) + old_size;

    *new_current_level = *new_current_level + 1;
}

void _append_to_level_and_jump(MBO* old_mbo, u8 old_current_level, MBO* new_mbo, u8* new_current_level, void** new_run, u32 order_id, u16 quantity){
    // first copy the level
    // then go back and do this, then finally jump properly

    // save our spot
    MBOLevel* mbol = (MBOLevel*)(*new_run);
    _copy_level_and_jump(old_mbo, old_current_level, new_mbo, new_current_level, new_run);

    // jump to where we need to write the new order id
    u32* ptr = (u32*)(((void*)mbol) + _mbo_level_size(mbol->order_count));
    *ptr = order_id;

    mbol->order_count++;

    // now jump to where we need to be for the next order level
    *new_run = ((void*)mbol) + _mbo_level_size(mbol->order_count);

    new_mbo->levels[old_current_level].quantity += quantity;
}

// not reliant on old at all, this is a new row
void _insert_level_and_jump(MBO* new_mbo, u8* new_current_level, void** new_run, u16 price, u16 quantity, u32 order_id){

    // this is where we need to insert the new limit order
    printf("setting level data\n");
    _write_level(new_mbo, *new_current_level, price, quantity, *new_run);

    MBOLevel* mbol = (MBOLevel*)(*new_run);// seprate issue but probably need this too
    mbol->order_count = 1;
    mbol->order_ids[0] = order_id;

    printf("updating pointers %p\n", *new_run);
    *new_run = (*new_run) + _mbo_level_size(1);
    *new_current_level = *new_current_level + 1;
    printf("done updating pointers %p\n", *new_run);
    //found = 1;
}

// does a lot but it's kinda coherent tho
void _partial_fill_and_insert_and_jump(MBO* old_mbo, u8 modified_level_index, MBO* new_mbo, u8* new_current_level, void** new_run, FL* orders, u16* remaining_quantity) {
    // a whole bunch of bullshit
     

    // the scenario where we partially fill a level
    // even worse, maybe partially fill an order
    MBOIndex* mboi = &(old_mbo->levels[modified_level_index]);

    _write_level(new_mbo, *new_current_level, mboi->price, mboi->quantity - (*remaining_quantity), *new_run);

    //old_run = (old_data_start + mod_index->byte_offset);
    void* old_run = (_data_start((void*)old_mbo) + mboi->byte_offset);

    // now go through it and see which orders we fill
    MBOLevel* mod_level = (MBOLevel*)old_run;

    u32 remaining_orders_on_level = mod_level->order_count;

    u8 i = 0;
    for (; i < mod_level->order_count; i++) {
        //printf("going throuh orders %u\n", i);
        u32 prev_order_id = mod_level->order_ids[i];
        Order* prev_order = (Order*)fl_get(orders, prev_order_id);

        u16 order_quantity = prev_order->quantity;
        if (order_quantity > (*remaining_quantity)) {
            // modify order directly, maybe send partial fill notification
            prev_order->quantity -= *remaining_quantity;
            break;
        } else {
            // fill order entirely
            remaining_orders_on_level--;
            (*remaining_quantity) = (*remaining_quantity) - order_quantity;
        }
    }

    MBOLevel* init = ((MBOLevel*)(*new_run));
    init->order_count = remaining_orders_on_level;
    for (u8 j = i; j < mod_level->order_count; j++) {
        ////printf("going throuh orders %u\n", j);
        init->order_ids[j-i] = mod_level->order_ids[j];
    }

    *new_run  = (*new_run) + sizeof(MBOLevel) + (mod_level->order_count - i) * sizeof(u32);

    (*new_current_level) = (*new_current_level) + 1;
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

    i8 hi_bid_index = old_mbo->hi_bid_index;
    i8 lo_ask_index = hi_bid_index + 1;

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

        u8 exact_level_wipe = 0;

        u8 modified_level = 0;
        u8 modified_level_index = 0;

        u8 current_level;

        // first figure out what rows we need to get rid of
        for(current_level = start_search; ; current_level += multiplier) {

            //printf("current level %u\n", current_level);
            MBOIndex mboi = old_mbo->levels[current_level];

            if (remaining_quantity > 0 && (multiplier)*mboi.price > (multiplier)*price) {
                printf("not enough to fill limit, leaving as open bid or ask\n");

                // stop due to limit price - possibly partial fill
                untouched_above = current_level - multiplier;
                partial_fill = 1;
                break;
            }

            u16 level_quantity = mboi.quantity;

            if (level_quantity >= remaining_quantity){
                untouched_above = current_level;
            }

            if (level_quantity > remaining_quantity) {
                // this order will be entirely filled on this level, making this lowest ask

                modified_level = 1;
                modified_level_index = current_level;

                break;
            } else if(level_quantity == remaining_quantity) {
                printf("TRADE: %u %u %u\n", direction, mboi.price, level_quantity);

                //printf("limit filled exactly at this level\n");
                remaining_quantity -= level_quantity;
                exact_level_wipe = 1;
                //printf("found exact remaining quantity at level %u %u\n", current_level, mboi.price);
                // this fill will be entirely filled on this level, mking NEXT highest lowest ask

                break;
            } else {
                printf("TRADE: %u %u %u\n", direction, mboi.price, level_quantity);

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

        u8 lowest_untouched_index = direction == 1 ? untouched_below : untouched_above;
        u8 highest_untouched_index = direction == 1 ?  untouched_above : untouched_below;

        new_mbo->level_count = (lowest_untouched_index - 0) + (old_mbo->level_count - highest_untouched_index - 1) + (partial_fill | modified_level);

        void* new_run = _data_start(new_mbo_raw);

        // CHANGING TO EXCLUSIVE

        u8 old_current_level = 0;
        for (; old_current_level < lowest_untouched_index;)
            _copy_level_and_jump(old_mbo, old_current_level, new_mbo, &old_current_level, &new_run);

        u8 new_current_level = old_current_level;

        if (exact_level_wipe) {
            new_mbo->hi_bid_index = new_current_level - 1;

        } else if (partial_fill) {
            // this is where we need to insert the new limit order
            _insert_level_and_jump(new_mbo, &new_current_level, &new_run, price, remaining_quantity, order_id);

            if (direction == 1) {
                //buy, we just updated highest bid
                new_mbo->hi_bid_index = new_current_level;
            } else {
                //sell, we just updated lowest ask
                new_mbo->hi_bid_index = new_current_level - 1;
            }

            Order* current_order = (Order*)fl_get(orders, order_id);
            current_order->quantity = remaining_quantity;
        } else if (modified_level) {
            if (direction == 1) 
                new_mbo->hi_bid_index = new_current_level - 1;
            else 
                new_mbo->hi_bid_index = new_current_level;

            _partial_fill_and_insert_and_jump(old_mbo, modified_level_index, new_mbo, &new_current_level, &new_run, orders,  &remaining_quantity); 
        } 


        for (old_current_level = highest_untouched_index+1; old_current_level < old_mbo->level_count; old_current_level++) 
            _copy_level_and_jump(old_mbo, old_current_level, new_mbo, &new_current_level, &new_run);

        // we can skip all this silliness above
        if (new_mbo->level_count == 0) 
            new_mbo->hi_bid_index = MAX_U8;

    } else {
        // it is not marketable, easier case
        u8 price_level_exists = 0;
        u8 match_level = 0;

        if (direction == 1 && hi_bid_index != MAX_U8 && old_mbo->levels[hi_bid_index].price < price) {
            //special case - nothing will match, this between hi bid and low ask
        } else {
            for(; start_search != bottom; start_search -= multiplier) {
                //printf("checking  %u, %u against %u\n", 
                //old_mbo->levels[start_search-multiplier].price,(start_search-multiplier), price);

                if (old_mbo->levels[(u8)(start_search-multiplier)].price == price) {
                    match_level = start_search-multiplier;
                    price_level_exists = 1;
                    break;
                }
            }
        }

        // now we know the size of MBOIndex at least

        new_mbo->hi_bid_index = old_mbo->hi_bid_index;
        if (direction == 1 && !price_level_exists){
            printf("buy with no existing price level, bumping hi bid index to %u\n", new_mbo->hi_bid_index + 1);
            printf("old was %u\n", old_mbo->hi_bid_index);

            new_mbo->hi_bid_index++;
        }

        new_mbo->level_count = old_mbo->level_count;
        if (!price_level_exists)
            new_mbo->level_count++;

        void* new_run = _data_start(new_mbo_raw);


        if (price_level_exists) {
            for (u8 old_current_level = 0; old_current_level < new_mbo->level_count;) {
                if (old_current_level == match_level) 
                    _append_to_level_and_jump(old_mbo, old_current_level, new_mbo, &old_current_level, &new_run, order_id, quantity);
                else 
                    _copy_level_and_jump(old_mbo, old_current_level, new_mbo, &old_current_level, &new_run);
            }
        } else {
            u8 new_current_level = 0;

            u8 found = 0;

            for (u8 old_current_level = 0; old_current_level < old_mbo->level_count; old_current_level++) {
                MBOIndex mboi = old_mbo->levels[old_current_level];

                if (found == 0 && mboi.price > price) {
                    _insert_level_and_jump(new_mbo, &new_current_level, &new_run, price, quantity, order_id);
                    found = 1;
                }
                _copy_level_and_jump(old_mbo, old_current_level, new_mbo, &new_current_level, &new_run);
            }

            if(found == 0)
                _insert_level_and_jump(new_mbo, &new_current_level, &new_run, price, quantity, order_id);
        }
    }

    // much much later
    bs_resize(mbo_bs, actual_size);
    return unused;
}

