#include <string.h>
#include <stdio.h>

#include "ob.h"

#include "types.h"
#include "constants.h"
#include "bs.h"
#include "cb.h"
#include "order.h"
#include "fill.h"

// "modified level behavior"
static const u8 NEW = 0;
static const u8 APPEND = 1;
static const u8 REST_REMAINDER = 2;
static const u8 FILL_SOME = 3;
static const u8 CAN_REP = 4;
static const u8 EXACT = 5;
static const u8 CAN_NO_WIPE = 6;
static const u8 CAN_WIPE = 7;
static const u8 SHRINK = 8; // can rep, but keep in place
static const u8 OP_TYPE_INVALID = MAX_U8;

static const u8 BUY = 1;

void* mbo_data_start(void* mbo_raw){
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


    void* data_start = mbo_data_start(mbo_raw);
    printf("level data\n");

    for (u16 i = 0; i < mbo->level_count; i++) {
        MBOIndex mboi = mbo->levels[i];
        u32 byte_offset = mboi.byte_offset;
        //printf("new mbol %p\n", (void*)(data_);
        MBOLevel* mbol = (MBOLevel*)(data_start + byte_offset);
        for(u8 j = 0; j < 8; j++){
            //printf("%u\n", *(u8*)(data_start+byte_offset+j));
        }

        printf("%uc\t with %u order count\t", mboi.price, mbol->order_count);

        // we crash right after this i think

        //printf("start of mbol %p\n", (data_start+byte_offset));
        //printf("start of order_ids %p\n", &(mbol->order_ids));

        for (u16 j = 0; j < mbol->order_count; j++) {
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


// just append
void mbo_app_level(MBORunner* new, u32 order_id, u32 quantity) {
    // jump past to write the proper entry
    MBOEntry* entry = (MBOEntry*)(((void*)(new->level)) + _mbo_level_size(new->level->order_count));
    entry->order_id = order_id;
    entry->quantity = quantity;
    new->level->order_count++;

    new->metadata->quantity += quantity;
    if(new->metadata->quantity == 0) {
        printf("quantity ended up 0\n");
        mbo_dump(new->mbo);
        exit(1);
    }
}

// shrink specific order to that quantity
void mbo_shrink_level(MBORunner* new, u32 order_id, u32 quantity, u32 new_id) {
    for (u16 i = 0; i < new->level->order_count; i++) {
        MBOEntry * order = new->level->entries + i;
        if (order->order_id == order_id){
            new->metadata->quantity -= order->quantity - quantity;
            order->quantity = quantity;
            order->order_id = new_id;
            break;
        }
    }
}

// copy append
void mbo_copapp_level(MBORunner* old, MBORunner* new, u32 order_id, u32 quantity) {
    mbo_copy_level(old, new);
    mbo_app_level(new, order_id, quantity);
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

void mbo_fill_remove(MBORunner* old, MBORunner* new, u16 price, u32 remaining_quantity, CB* fills, u32 cancel_id) {
    // it warns we dont use price. shoudl we?
    new->metadata->price = old->metadata->price;
    new->metadata->quantity = old->metadata->quantity - remaining_quantity;
    new->metadata->byte_offset = ((void*)(new->level)) - new->data_start;

    MBOLevel* mod_level = old->level;

    u32 remaining_orders_on_level = mod_level->order_count;

    u8 partial_fill = 0;

    u16 i = 0;
    for (; i < mod_level->order_count; i++) {

        // that's what we USED to do. but really we just modify the order book here
        // and somehow notify that THIS MUCH of THIS ORDER was filled, passing it up to serer.c to modify the order themselves
        // which also solves the whole partial thing

        MBOEntry * prev_order = mod_level->entries + i;
        u32 prev_order_id = prev_order->order_id;

        if (prev_order_id == cancel_id)
            continue;

        u32 order_quantity = prev_order->quantity;

        if (order_quantity > remaining_quantity) {
            //prev_order->quantity -= remaining_quantity;
            Fill f = {
                .order_id = prev_order_id, 
                .quantity_filled = remaining_quantity, 
                .partial = 1};
            cb_queue(fills, &f);
            //printf("a filling %u\n", prev_order_id);
            partial_fill = 1;
            break;
        } else {
            // fill resting order entirely
            remaining_orders_on_level--;
            remaining_quantity -= order_quantity;
            Fill f = {
                .order_id = prev_order_id, 
                .quantity_filled = order_quantity};
            cb_queue(fills, &f);
            //printf("b filling %u\n", prev_order_id);
        }
    }

    MBOLevel * init = new->level;

    u16 j = i;

    if (partial_fill) {
        // I feel like we were previously forgetting to write in the case of partial fill, not anymore
        // first need to append that last one in, but NOT modify the one on server
        MBOEntry * prev_order = mod_level->entries + i;
        //printf("about to fill in data for partial fill %u", (prev_order->quantity - remaining_quantity));
        init->entries[0].order_id = prev_order->order_id;
        init->entries[0].quantity = prev_order->quantity - remaining_quantity;

        j++;
    }

    init->order_count = remaining_orders_on_level;
    for (; j < mod_level->order_count; j++){

        MBOEntry * prev_order = mod_level->entries + j;
        if (prev_order->order_id == cancel_id){
            // skip it, but also increment i to make sure we write to the correct spot
            i++;
            continue;
        }

        init->entries[j-i].order_id = prev_order->order_id;
        init->entries[j-i].quantity = prev_order->quantity;
    }
}

// used for cancellations
void mbo_splice_level(MBORunner* old, MBORunner* new, u32 cancel_id) {
    new->metadata->price = old->metadata->price;
    new->metadata->quantity = old->metadata->quantity; // for now
    new->metadata->byte_offset = ((void*)(new->level)) - new->data_start;

    MBOLevel* mod_level = old->level;

    // copy in order entry by order entry except our guy
    u16 j = 0;
    for (u16 i = 0; i < mod_level->order_count; i++) {
        MBOEntry * prev_order = mod_level->entries + i;
        if (prev_order->order_id == cancel_id){
            new->metadata->quantity -= prev_order->quantity;
            continue;
        }
        new->level->entries[j].order_id = prev_order->order_id;
        new->level->entries[j].quantity = prev_order->quantity;

        j++;
    }
    new->level->order_count = mod_level->order_count - 1;
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

// oh this is a bit confusing
// but this first parameter order id is like "the cancel order"
// the "order that will be cancelled" is in->other_id
// in fact I dont even think we need 
u32 ob_cancel(Order* to_cancel, u32 cancel_id, void* old_mbo_raw, void* new_mbo_raw) {
    // ok this is much easier than anything

    // the only level check we need to do is find the level of the cancel and see if it wipes
    // easy index-only check : if quantity of order == quantity of level - wipe level, possibly updating hi_bid_index
    // else modify
    // copy all other levels
    MBO* old_mbo = (MBO*)old_mbo_raw;
    u16 cancel_price = to_cancel->price;

    // it's on level i, level_wipe indicated
    MBO* new_mbo = (MBO*)new_mbo_raw;
    new_mbo->level_count = old_mbo->level_count;
    new_mbo->hi_bid_index = old_mbo->hi_bid_index;

    u8 level_wipe = 0;
    u16 i = 0;
    for (; i < old_mbo->level_count; i++){
        if (old_mbo->levels[i].price == cancel_price) {
            if (old_mbo->levels[i].quantity == to_cancel->quantity){
                level_wipe = 1;
                new_mbo->level_count--;

                // one last check
                if (old_mbo->hi_bid_index != MAX_U16 && i <= old_mbo->hi_bid_index) 
                    new_mbo->hi_bid_index--;
            }
            break;
        }
    }

    // at this point level_count is set
    // ah wait now we need orders again in ob

    MBORunner * old_runner = mbor_init(old_mbo);
    MBORunner * new_runner = mbor_init(new_mbo);

    for (u16 j = 0; j < old_mbo->level_count; j++) {
        if (j == i) {
            if (level_wipe) {
                // just skip old
            } else {
                mbo_splice_level(old_runner, new_runner, cancel_id);
                mbo_jump(new_runner);
            }
        } else {
            mbo_copy_level(old_runner, new_runner);
            mbo_jump(new_runner);
        }
        mbo_jump(old_runner);
    }

    // that fuckin easy

    return ((void*)(new_runner->level)) - new_mbo_raw;
}

// figure out which old-mbo levels a single side (bid OR ask) touches, and how
// lui/hui are the untouched indicies bracketing the affected range, op_type says what to do
// remaining_out gets the leftover quantity after any fills, modified_level the level FILL_SOME bit into
void ob_affected_range(MBO* old_mbo, Order* rep, Order* can,
                       u8 is_can_rep, u8 is_cancel, u32 cancel_id,
                       u16 cancel_index, u8 cancel_was_sole,
                       u16* lui, u16* hui, u8* op_type,
                       u16* modified_level, u16* remaining_out, CB* fills) {

    u16 price = rep->price;
    u16 remaining_quantity = rep->quantity;

    // sentinel: the "put it at the bottom" fallback below checks for this, so it
    // must not start as a real op (0 == NEW). callers pass a zero-initialized field.
    *op_type = OP_TYPE_INVALID;


    // somewhat special case
    if (!is_can_rep && is_cancel) {
        if (cancel_was_sole)
            *op_type = CAN_WIPE;
        else
            *op_type = CAN_NO_WIPE;
        *lui = cancel_index;
        *hui = cancel_index;
    } else {

        if (is_can_rep && rep->price == can->price && ((rep->status >> BUY_DIRECTION_BIT) & 1) == ((can->status >> BUY_DIRECTION_BIT & 1))) {
            *lui = cancel_index;
            *hui = cancel_index;
            if (rep->quantity > can->quantity)
                *op_type = CAN_REP;
            else
                *op_type = SHRINK;

            // assuming rep quanity not zero
        } else {
            u8 is_market = (rep->status >> IS_MARKET_BIT) & 1;

            // check if marketable
            u8 direction = (rep->status >> BUY_DIRECTION_BIT) & 1;

            i16 multiplier = 0;
            u16 hi_bid_index = old_mbo->hi_bid_index;
            u16 lo_ask_index = hi_bid_index + 1;

            u16 start_search;
            u16 bottom;
            u16 top;

            u8 has_opponents = 1;

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

            u16 untouched_below = start_search;
            u16 untouched_above = top;


            // can do ++ or -- as we go
            MBOIndex * old_level = old_mbo->levels + start_search;
            //printf("has opps %u old lvl price %u price %u\n", has_opponents, old_level->price, price);
            if (is_market || (has_opponents && (multiplier)*(old_level->price) <= multiplier*price)) {
                untouched_below = start_search;
                untouched_above = top;
                // this will also take the lowest ask -> replace with bid case

                for(u16 current_level = start_search; ; current_level += multiplier) {
                    old_level = old_mbo->levels + current_level;

                    if (!is_market && (remaining_quantity > 0 && multiplier*price < multiplier*old_level->price)) {
                        // for later limits:
                        // GTC - rest remainder (current behavior)
                        // IOC - should become EXACT
                        // we dont want to touch the next level, rest the remaining quantity
                        if (is_gtc) {
                            *op_type = REST_REMAINDER;
                        } else if (is_ioc) {
                            // TODO, if its exact but there is quantity remaining, is that fine?
                            *op_type = EXACT;
                        }
                        untouched_above = current_level - multiplier;
                        break;
                    }

                    // could be zero, the same as eating the entire level
                    u32 effective_level_quantity = old_level->quantity;
                    // TODO: pairs allow us to do order book operations even if nothing is cancelled
                    // Verify that this is not the case if the cancel order is not found
                    // (cancel order must not exist or its price reset to zero or is_can_rep set to false)
                    if (is_can_rep && old_level->price == can->price)
                        effective_level_quantity -= can->quantity;

                    if (effective_level_quantity >= remaining_quantity) {
                        untouched_above = current_level;
                    }
                    //printf("current level %u effective level %u remaining q %u\n", current_level, effective_level_quantity, remaining_quantity);

                    if (effective_level_quantity <= remaining_quantity) {
                        // fill
                        // this looks like a good use case for runners
                        void* old_run = (mbo_data_start((void*)old_mbo) + old_level->byte_offset);

                        MBOLevel* mod_level = (MBOLevel*)old_run;
                        for (u16 i = 0; i < mod_level->order_count; i++) {
                            MBOEntry * entry = mod_level->entries + i;
                            if (is_can_rep && entry->order_id == cancel_id)
                                continue;
                            Fill f = { 
                                .order_id = entry->order_id,
                                .quantity_filled = entry->quantity};
                            cb_queue(fills, &f);
                            //printf("f filling %u\n", entry->order_id);
                        }  
                    }

                    if (effective_level_quantity > remaining_quantity){
                        //printf("effective level quantity more than remaining\n");
                        *op_type = FILL_SOME;
                        *modified_level = current_level;
                        break;
                    } else if (effective_level_quantity == remaining_quantity) {
                        remaining_quantity -= effective_level_quantity;
                        *op_type = EXACT;
                        break;
                    } else {
                        remaining_quantity -= effective_level_quantity;
                    }

                    if (current_level == top) {
                        if (remaining_quantity > 0) {
                            // limit GTC - rest remainder
                            // limit IOC - scrap remainder - make it EXACT
                            // limit FOK - shouldnt't happen
                            // market GTC - impossible
                            // market IOC - EXACT too
                            // market FOK - shouldnt happen
                            if (is_gtc)
                                *op_type = REST_REMAINDER;
                            else if (is_ioc)
                                *op_type = EXACT;
                            untouched_above = top;
                        }
                        break;
                    }

                }

            } else {
                // nonmarketable limit order, will rest

                if (direction == 1 && hi_bid_index != MAX_U16 && old_mbo->levels[hi_bid_index].price < price) {
                    *op_type = NEW;
                    untouched_below = hi_bid_index + 1;
                    untouched_above = hi_bid_index;
                } else {
                    for(; start_search != bottom; start_search -= multiplier) {
                        u16 level_price = old_mbo->levels[(u16)(start_search-multiplier)].price;

                        if (price*multiplier == level_price*multiplier) {
                            // does this math work out? or is this bytes?
                            *op_type = APPEND;
                            untouched_below = start_search-multiplier;
                            untouched_above = start_search-multiplier;

                            break;
                        }       
                        if (price*multiplier > level_price*multiplier) {
                            // this will include all levels currently in old_mbo
                            *op_type = NEW; 
                            untouched_below = start_search;
                            untouched_above = start_search-multiplier;
                            break;
                        } 
                    }


                    // did not find, it needs to be put at bottom
                    if (*op_type == OP_TYPE_INVALID) {
                        *op_type = NEW;
                        untouched_below = bottom;
                        untouched_above = bottom - multiplier; // iffy on this
                    }
                }

            }
            *lui = direction == BUY ? untouched_below : untouched_above;
            *hui = direction == BUY ? untouched_above : untouched_below;
        }
    }

    *remaining_out = remaining_quantity;
}

// one side (bid or ask) of a book mutation: the request + where it lands
typedef struct OBSide {
    Order* rep;          // the incoming/replacing order (price, quantity, direction)
    u32 order_id;        // id the new resting entry should carry

    u8 is_can_rep;
    u8 is_cancel;
    u32 cancel_id;       // rep->other_id, the order being cancelled
    u16 cancel_price;    // its price level in old_mbo
    u16 cancel_index;    // its index in old_mbo
    u8 cancel_was_sole;  // was it the only order on that level

    // filled in by ob_affected_range
    u16 lui, hui;
    u8 op_type;
    u16 modified_level;
    u16 remaining;       // leftover quantity after any fills
} OBSide;

// find which old level holds the cancel, and whether it's the only order there
void ob_locate_cancel(MBO* old_mbo, OBSide* s, Order* can) {
    s->cancel_price = can->price;
    for (u16 i = 0; i < old_mbo->level_count; i++) {
        MBOIndex * level = old_mbo->levels + i;
        if (level->price == can->price){
            s->cancel_index = i;
            if (level->quantity == can->quantity)
                s->cancel_was_sole = 1;
            break;
        }
    }
}

// how many levels this op adds. everything leaves a fresh level behind except
// EXACT (consumed a whole level exactly) and CAN_WIPE (cancel emptied its level)
u16 ob_op_level_delta(u8 op_type) {
    return op_type != EXACT && op_type != CAN_WIPE;
}

// copy old levels [from, to) into new, splicing out any watched cancel that shows up
// (up to two, so the pair can watch both the bid and ask cancels in one pass)
void ob_copy_range(MBORunner* old, MBORunner* new, u16 from, u16 to, OBSide** sides, u8 n_sides) {
    mbo_to_index(old, from);
    for (u16 i = from; i < to; i++) {
        u8 spliced = 0;
        for (u8 s = 0; s < n_sides; s++) {
            OBSide* sd = sides[s];
            if ((sd->is_can_rep | sd->is_cancel) && old->metadata->price == sd->cancel_price) {
                if (sd->cancel_was_sole){
                    //skip level
                } else {
                    mbo_splice_level(old, new, sd->cancel_id);
                    mbo_jump(new);
                }
                spliced = 1;
                break;
            }
        }
        if (!spliced) {
            mbo_copy_level(old, new);
            mbo_jump(new);
        }
        mbo_jump(old);
    }
}

// serialize one side's op into new_runner. old_runner must be sitting at s->lui.
// returns the hi_bid_index this side implies for the new book.
u16 ob_apply_op(OBSide* s, MBO* old_mbo, MBORunner* old_runner, MBORunner* new_runner, CB* fills) {
    u8 op_type = s->op_type;
    u8 direction = (s->rep->status >> BUY_DIRECTION_BIT) & 1;
    u16 price = s->rep->price;
    u16 quantity = s->rep->quantity;
    u32 order_id = s->order_id;
    u32 cancel_id = s->cancel_id;
    u8 is_can_rep = s->is_can_rep;
    u16 cancel_index = s->cancel_index;
    u8 cancel_was_sole = s->cancel_was_sole;

    u16 hbi;

    if (op_type == NEW){
        hbi = old_mbo->hi_bid_index;

        if (hbi != MAX_U16 && is_can_rep && cancel_was_sole && cancel_index <= hbi)
            hbi--;

        if (direction == BUY)
            hbi++;

        // old not relevant
        mbo_insert_level(new_runner, order_id, price, quantity);
    } else if (op_type == APPEND) {
        if (hbi != MAX_U16 && is_can_rep && cancel_was_sole && cancel_index <= old_mbo->hi_bid_index)
            hbi = old_mbo->hi_bid_index - 1;
        else
            hbi = old_mbo->hi_bid_index;

        // old already in correct position
        mbo_copy_level(old_runner, new_runner);
        mbo_app_level(new_runner, order_id, quantity);
    } else if (op_type == REST_REMAINDER) {
        if (direction == BUY)
            hbi = new_runner->index;
        else
            hbi = new_runner->index - 1;
        // old not relevant
        mbo_insert_level(new_runner, order_id, price, s->remaining);
    } else if (op_type == FILL_SOME) {
        if (direction == BUY)
            hbi = new_runner->index - 1;
        else
            hbi = new_runner->index;

        mbo_to_index(old_runner, s->modified_level);
        // fill, but dont count the cancel_id
        mbo_fill_remove(old_runner, new_runner, price, s->remaining, fills, is_can_rep ? cancel_id: MAX_U32);
    } else if (op_type == CAN_REP) {
        // we can assume is_can_rep
        hbi = old_mbo->hi_bid_index;
        // old already in correct position
        mbo_splice_level(old_runner, new_runner, cancel_id);
        mbo_app_level(new_runner, order_id, quantity);
    } else if (op_type == EXACT) {
        hbi = new_runner->index - 1;
        // skip
    } else if (op_type == CAN_WIPE) {
        // similar to exact
        hbi = old_mbo->hi_bid_index;
        if (hbi != MAX_U16 && cancel_index <= hbi)
            hbi--;
    } else if (op_type == CAN_NO_WIPE) {
        hbi = old_mbo->hi_bid_index;
        mbo_splice_level(old_runner, new_runner, cancel_id);
    } else if (op_type == SHRINK) {
        // copy then shrink
        hbi = old_mbo->hi_bid_index;
        mbo_copy_level(old_runner, new_runner);
        mbo_shrink_level(new_runner, cancel_id, quantity, order_id);
    }

    if (op_type != EXACT && op_type != CAN_WIPE)
        mbo_jump(new_runner);

    return hbi;
}

// build a side descriptor from an order id, resolving its cancel against old_mbo
void ob_side_init(OBSide* s, FL* orders, MBO* old_mbo, u32 order_id) {
    Order* rep = (Order*)fl_get(orders, order_id);
    s->rep = rep;
    s->order_id = order_id;
    s->is_can_rep = (rep->status >> CAN_REP_BIT) & 1;
    s->is_cancel = (rep->status >> CANCEL_BIT) & 1;
    s->cancel_id = rep->other_id;
    if (s->is_can_rep | s->is_cancel)
        ob_locate_cancel(old_mbo, s, (Order*)fl_get(orders, s->cancel_id));
}

// run a side through ob_affected_range, filling in its lui/hui/op_type/etc.
void ob_side_range(OBSide* s, FL* orders, MBO* old_mbo, CB* fills) {
    Order* can = (s->is_can_rep | s->is_cancel) ? (Order*)fl_get(orders, s->cancel_id) : 0;
    ob_affected_range(old_mbo, s->rep, can,
                      s->is_can_rep, s->is_cancel, s->cancel_id,
                      s->cancel_index, s->cancel_was_sole,
                      &s->lui, &s->hui, &s->op_type,
                      &s->modified_level, &s->remaining, fills);
}

// atomic bid + ask replace/cancel. bid is bid_order_id, ask is ask_order_id.
// requires a non-crossing pair (bid price < ask price, neither leg marketable),
// so the two affected ranges stay disjoint with the bid region below the ask region.
u32 ob_pair(FL* orders, u32 bid_order_id, u32 ask_order_id, void* old_mbo_raw, void* new_mbo_raw, CB* fills) {
    MBO* old_mbo = (MBO*)old_mbo_raw;
    MBO* new_mbo = (MBO*)new_mbo_raw;

    OBSide bid = {0};
    OBSide ask = {0};
    ob_side_init(&bid, orders, old_mbo, bid_order_id);
    ob_side_init(&ask, orders, old_mbo, ask_order_id);

    ob_side_range(&bid, orders, old_mbo, fills);
    ob_side_range(&ask, orders, old_mbo, fills);

    // serialization form: [0, bid_lui) bid_op (bid_hui, ask_lui) ask_op (ask_hui, end]
    u16 level_count = bid.lui
                    + ob_op_level_delta(bid.op_type)
                    + (ask.lui - bid.hui - 1)
                    + ob_op_level_delta(ask.op_type)
                    + (old_mbo->level_count - ask.hui - 1);

    // a sole cancel sitting in a copy region gets dropped, so it's one fewer level
    if ((bid.is_can_rep | bid.is_cancel) && bid.cancel_was_sole
            && (bid.cancel_index < bid.lui || bid.cancel_index > bid.hui))
        level_count--;
    if ((ask.is_can_rep | ask.is_cancel) && ask.cancel_was_sole
            && (ask.cancel_index < ask.lui || ask.cancel_index > ask.hui))
        level_count--;

    new_mbo->level_count = level_count;

    MBORunner* new_runner = mbor_init(new_mbo);
    MBORunner* old_runner = mbor_init(old_mbo);

    // either cancel can surface in any copy region, so watch both throughout
    OBSide* sides[2] = { &bid, &ask };

    ob_copy_range(old_runner, new_runner, 0, bid.lui, sides, 2);
    ob_apply_op(&bid, old_mbo, old_runner, new_runner, fills);
    ob_copy_range(old_runner, new_runner, bid.hui + 1, ask.lui, sides, 2);
    ob_apply_op(&ask, old_mbo, old_runner, new_runner, fills);
    ob_copy_range(old_runner, new_runner, ask.hui + 1, old_mbo->level_count, sides, 2);

    // hi_bid_index = (bid levels in the new book) - 1, counted region by region:
    //   lower untouched [0, bid.lui)      -> all bids                        : bid.lui
    //   bid op                            -> rests a bid unless it fully filled
    //   middle untouched (bid.hui, ask.lui) -> bids up to the old boundary
    //   ask op                            -> only FILL_SOME leaves a (bitten) bid
    //   upper untouched (ask.hui, end]    -> all asks                        : 0
    u16 old_hbi = old_mbo->hi_bid_index;
    u16 bids = bid.lui
             + (bid.op_type != FILL_SOME && bid.op_type != EXACT)
             + (ask.op_type == FILL_SOME);
    if (old_hbi != MAX_U16) {
        int cap = (int)ask.lui - 1;            // last middle index
        if ((int)old_hbi < cap) cap = old_hbi; // ...but only bids count
        int mid_bids = cap - (int)bid.hui;
        if (mid_bids > 0) bids += mid_bids;
    }
    new_mbo->hi_bid_index = bids ? bids - 1 : MAX_U16;

    return ((void*)(new_runner->level)) - new_mbo_raw;
}


u32 ob_canrep(FL* orders, u32 order_id, void* old_mbo_raw, void* new_mbo_raw, CB* fills) {

    MBO* old_mbo = (MBO*)old_mbo_raw;
    MBO* new_mbo = (MBO*)new_mbo_raw;

    // general serialization form:
    //[0 - lowest_untouched)something(highest_untouched - end]
    //+ cancel index


    // one side: resolve the cancel, then find its affected range
    OBSide side = {0};
    ob_side_init(&side, orders, old_mbo, order_id);
    ob_side_range(&side, orders, old_mbo, fills);

    // handy aliases for the level_count bookkeeping below
    u16 lui = side.lui;
    u16 hui = side.hui;
    u16 cancel_index = side.cancel_index;
    u8 cancel_was_sole = side.cancel_was_sole;
    u8 is_can_rep = side.is_can_rep;
    u8 is_cancel = side.is_cancel;
    u8 op_type = side.op_type;


    // ==== new section 

    // ok at this point we have lui and hui and cancel_index and various other types
    // first lets figure out level count

    // what are untouched
    // the rest depend on "op_type"
    u16 level_count = (lui - 0) + (old_mbo->level_count - hui - 1);

    //printf("level count %u lui %u hui %u op_type %u\n", level_count, lui, hui, op_type);

    // every op leaves a fresh level behind except EXACT / CAN_WIPE (see helper)
    level_count += ob_op_level_delta(op_type);

    if (is_can_rep | is_cancel) {
        if (hui == MAX_U16){
            // we really mean 0 and above is hui
            if ((cancel_index < lui || cancel_index >= 0) && (cancel_was_sole))
                level_count--;
        } else if (lui == MAX_U16) {
            // probably not possible, we can indicate "nothing" by doing zero
        } else {
            if (cancel_was_sole && (cancel_index < lui || cancel_index >= hui+1)) 
                level_count--;
        }
    }

    new_mbo->level_count = level_count;

    // ==== new section

    // ok now we can really have fun
    // copy in lower block
    // do op_type crazy
    // copy in higher block

    MBORunner* new_runner = mbor_init(new_mbo);
    MBORunner* old_runner = mbor_init(old_mbo);

    // copy in the lower untouched block, splicing the cancel if it lives down here
    OBSide* sides[1] = { &side };
    ob_copy_range(old_runner, new_runner, 0, lui, sides, 1);
    // do the op in the middle
    u16 hbi = ob_apply_op(&side, old_mbo, old_runner, new_runner, fills);

    // copy in the higher untouched block, splicing the cancel if it lives up here
    ob_copy_range(old_runner, new_runner, hui + 1, old_mbo->level_count, sides, 1);

    new_mbo->hi_bid_index = hbi;
    if (new_mbo->level_count == 0)
        new_mbo->hi_bid_index = MAX_U16;
    // then do some calculation like
    return ((void*)(new_runner->level)) - new_mbo_raw;
}

// helper method for market makers and such
// price is passed in as a shortcut, otherwise we'd have to scan all the levels carefully
// this is a client helper, and the client will not be able to pass in FL* orders
// because that is "live" and every client is fundamentally delayed
// but they should remember the price at least

u32 ob_queue_position(u16 price, u32 order_id, void* mbo_raw) {
    // how much quantity ahead of this order
    MBO* mbo = (MBO*)mbo_raw;

    MBOIndex* mboi = mbo->levels;
    for (u16 i = 0; i < mbo->level_count; i++) {
        if (mboi->price == price) {
            MBOLevel * mbol = (MBOLevel*)(mbo_data_start(mbo_raw) + mboi->byte_offset);

            MBOEntry * mboe = mbol->entries;
            u32 quantity_before = 0;
            for (u16 j = 0; j < mbol->order_count; j++) {
                if (mboe->order_id == order_id){
                    return quantity_before;
                }
                quantity_before += mboe->quantity;
                mboe++;
            }

            return MAX_U32;
        }
        mboi++;
    }
    // was not found, it is not in the book
    return MAX_U32;
}

