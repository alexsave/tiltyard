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
            u32 oid = mbol->order_ids[j];
            //if (oid > 43440){
                //printf("some ridiculously large id value already\n");
                //exit(1);
            //}
            printf("#%u\t", mbol->order_ids[j]);
        }    
        printf("\n");
    }
    printf("===MBO DUMP END===\n");

}

u32 _data_start_offset(u16 level_count) {
    return sizeof(MBO) + level_count * sizeof(MBOIndex);
}


u32 _mbo_level_size(u16 order_count) {
    return sizeof(MBOLevel) + order_count * sizeof(u32);
}

void _write_level(MBO* new_mbo, u16 new_current_level, u16 price, u32 quantity, void* new_run){
    void* new_data_start = _data_start(new_mbo);

    new_mbo->levels[new_current_level].price = price;
    new_mbo->levels[new_current_level].quantity = quantity;
    // we should never write quantity zero, it's a smell for sure
    new_mbo->levels[new_current_level].byte_offset = new_run - new_data_start;
}

void _copy_level_and_jump(MBO* old_mbo, u16 old_current_level, MBO* new_mbo, u16* new_current_level, void** new_run){
    MBOIndex mboi = old_mbo->levels[old_current_level];

    // assuming level count is set correctly
    _write_level(new_mbo, *new_current_level, mboi.price, mboi.quantity, *new_run);

    void* old_run = (_data_start((void*)old_mbo) + mboi.byte_offset);

    u16 old_size = _mbo_level_size((((MBOLevel*)old_run)->order_count));
    memcpy(*new_run, old_run, old_size);
    *new_run = (*new_run) + old_size;

    *new_current_level = *new_current_level + 1;
}

void _append_to_level_and_jump(MBO* old_mbo, u16 old_current_level, MBO* new_mbo, u16* new_current_level, void** new_run, u32 order_id, u32 quantity){
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
    if (new_mbo->levels[old_current_level].quantity == 0){
        mbo_dump(old_mbo);
        exit(1);
    }
}

// not reliant on old at all, this is a new row
void _insert_level_and_jump(MBO* new_mbo, u16* new_current_level, void** new_run, u16 price, u32 quantity, u32 order_id){

    // this is where we need to insert the new limit order
    //printf("setting level data\n");
    _write_level(new_mbo, *new_current_level, price, quantity, *new_run);

    MBOLevel* mbol = (MBOLevel*)(*new_run);// seprate issue but probably need this too
    mbol->order_count = 1;
    mbol->order_ids[0] = order_id;

    //printf("updating pointers %p\n", *new_run);
    *new_run = (*new_run) + _mbo_level_size(1);
    *new_current_level = *new_current_level + 1;
    //printf("done updating pointers %p\n", *new_run);
}

// does a lot but it's kinda coherent tho
void _partial_fill_and_insert_and_jump(MBO* old_mbo, u16 modified_level_index, MBO* new_mbo, u16* new_current_level, void** new_run, FL* orders, u32* remaining_quantity, CB* fills, u32* partial_fill_id, u32* partial_fill_q) {
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

    u16 i = 0;
    for (; i < mod_level->order_count; i++) {
        //printf("going throuh orders %u\n", i);
        u32 prev_order_id = mod_level->order_ids[i];
        Order* prev_order = (Order*)fl_get(orders, prev_order_id);

        u32 order_quantity = prev_order->quantity;
        if (order_quantity > (*remaining_quantity)) {
            // modify order directly, maybe send partial fill notification
            prev_order->quantity -= *remaining_quantity;
            // mark partial fill
            *partial_fill_id = prev_order_id;
            *partial_fill_q = *remaining_quantity;
            break;
        } else {
            // fill order entirely
            remaining_orders_on_level--;
            (*remaining_quantity) = (*remaining_quantity) - order_quantity;
            // complete fill
            cb_queue(fills, prev_order_id);
        }
    }

    MBOLevel* init = ((MBOLevel*)(*new_run));
    init->order_count = remaining_orders_on_level;
    for (u16 j = i; j < mod_level->order_count; j++) {
        ////printf("going throuh orders %u\n", j);
        init->order_ids[j-i] = mod_level->order_ids[j];
    }

    *new_run  = (*new_run) + sizeof(MBOLevel) + (mod_level->order_count - i) * sizeof(u32);

    (*new_current_level) = (*new_current_level) + 1;
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

    // step one - calculate max possible new mbo size
    u32 old_size = mbo_bs->metadata[mbo_handle].size;
    u32 max_new_size = old_size + sizeof(MBOIndex) + sizeof(MBOLevel) + sizeof(u32);

    u32 actual_size = max_new_size;

    void* new_mbo_raw;
    u32 unused = bs_reserve(mbo_bs, max_new_size, ref_count, &new_mbo_raw);

    MBO* new_mbo = (MBO*)new_mbo_raw;

    //printf("start of bs store at %p\n", mbo_bs->store);
    //printf("new mbo at %p\n", new_mbo_raw);

    void* old_mbo_raw = bs_get_no_ref(mbo_bs, mbo_handle);
    MBO* old_mbo = (MBO*)old_mbo_raw;

    i16 hi_bid_index = old_mbo->hi_bid_index;
    i16 lo_ask_index = hi_bid_index + 1;

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
                    u32 prev_order_id = mod_level->order_ids[i];
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
                //printf("TRADE: %u %u %u\n", direction, mboi.price, level_quantity);

                //printf("limit filled exactly at this level\n");
                remaining_quantity -= level_quantity;
                exact_level_wipe = 1;
                //printf("found exact remaining quantity at level %u %u\n", current_level, mboi.price);
                // this fill will be entirely filled on this level, mking NEXT highest lowest ask

                break;
            } else {
                //printf("TRADE: %u %u %u\n", direction, mboi.price, level_quantity);

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
        u16 highest_untouched_index = direction == 1 ?  untouched_above : untouched_below;

        new_mbo->level_count = (lowest_untouched_index - 0) + (old_mbo->level_count - highest_untouched_index - 1) + (partial_fill | modified_level);

        void* new_run = _data_start(new_mbo_raw);
        //printf("got new run\n");

        // CHANGING TO EXCLUSIVE

        u16 old_current_level = 0;
        for (; old_current_level < lowest_untouched_index;)
            _copy_level_and_jump(old_mbo, old_current_level, new_mbo, &old_current_level, &new_run);

        u16 new_current_level = old_current_level;

        // before we do this, for all three cases we need to completely copy all orders from wiped levels into "fills"
        // exact level wipe does this, but does not do the partial thing
        // partial fill does this for all levels below where we add the new bid
        // modified fill does this for for all levele below, adn aslo the partial thing

        // I believe the exact formula is...
    
        // lets split it up for now

        // fuck it i think it's the same for all three scenarios
        // anything between lowest_untouched and highest_untouched will be wiped out, thus filled
        // we insert no new rows, thus all rows from lowest_untouched to highest_untouched must've been wiped
        // outside of those, they remain the same
        for (u16 wipe_level = lowest_untouched_index; wipe_level <= highest_untouched_index; wipe_level++){
            // do the partial fill thing
        }


        if (exact_level_wipe) {
            new_mbo->hi_bid_index = new_current_level - 1;

        } else if (partial_fill) {
            // the INCOMING order is partially filled, thus left as a standing order

            if (direction == 1) {
                //buy, we just updated highest bid
                new_mbo->hi_bid_index = new_current_level;
            } else {
                //sell, we just updated lowest ask
                new_mbo->hi_bid_index = new_current_level - 1;
            }
            // this is where we need to insert the new limit order
            _insert_level_and_jump(new_mbo, &new_current_level, &new_run, price, remaining_quantity, order_id);

            Order* current_order = (Order*)fl_get(orders, order_id);
            current_order->quantity = remaining_quantity;
        } else if (modified_level) {
            if (direction == 1) 
                new_mbo->hi_bid_index = new_current_level - 1;
            else 
                new_mbo->hi_bid_index = new_current_level;

            //printf("partial fill and insert\n");
            _partial_fill_and_insert_and_jump(old_mbo, modified_level_index, new_mbo, &new_current_level, &new_run, orders,  &remaining_quantity, fills, partial_fill_id, partial_fill_q); 
        } 

        //printf("going through above levels\n");

        for (old_current_level = highest_untouched_index+1; old_current_level < old_mbo->level_count; old_current_level++) 
            _copy_level_and_jump(old_mbo, old_current_level, new_mbo, &new_current_level, &new_run);

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
        if (direction == 1 && !price_level_exists){
            //printf("buy with no existing price level, bumping hi bid index to %u\n", new_mbo->hi_bid_index + 1);
            //printf("old was %u\n", old_mbo->hi_bid_index);

            new_mbo->hi_bid_index++;
        }

        new_mbo->level_count = old_mbo->level_count;
        if (!price_level_exists)
            new_mbo->level_count++;

        void* new_run = _data_start(new_mbo_raw);

        if (price_level_exists) {
            for (u16 old_current_level = 0; old_current_level < new_mbo->level_count;) {
                if (old_current_level == match_level) 
                    _append_to_level_and_jump(old_mbo, old_current_level, new_mbo, &old_current_level, &new_run, order_id, quantity);
                else 
                    _copy_level_and_jump(old_mbo, old_current_level, new_mbo, &old_current_level, &new_run);
            }
        } else {
            u16 new_current_level = 0;

            u8 found = 0;

            for (u16 old_current_level = 0; old_current_level < old_mbo->level_count; old_current_level++) {

                MBOIndex mboi = old_mbo->levels[old_current_level];

                if (found == 0 && mboi.price > price) {
                    _insert_level_and_jump(new_mbo, &new_current_level, &new_run, price, quantity, order_id);
                    found = 1;
                }
                _copy_level_and_jump(old_mbo, old_current_level, new_mbo, &new_current_level, &new_run);
            }

            if(found == 0){
                _insert_level_and_jump(new_mbo, &new_current_level, &new_run, price, quantity, order_id);
            }
        }
    }

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
        u32 max_new_size = old_size + sizeof(MBOIndex) + sizeof(MBOLevel) + sizeof(u32);
        printf("predicted size was %u, then requested to resize to %U\n", max_new_size, actual_size);
        printf("old mbo\n");
        mbo_dump(old_mbo);
        printf("new mbo\n");
        mbo_dump(new_mbo);
        exit(1);
    }
    return unused;
}

