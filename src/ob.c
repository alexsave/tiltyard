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
static const u8 OP_TYPE_INVALID = MAX_U8;

static const u8 BUY = 1;

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
            printf("a filling %u\n", prev_order_id);
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
            printf("b filling %u\n", prev_order_id);
        }
    }

    MBOLevel * init = new->level;

    u16 j = i;

    if (partial_fill) {
        // I feel like we were previously forgetting to write in the case of partial fill, not anymore
        // first need to append that last one in, but NOT modify the one on server
        MBOEntry * prev_order = mod_level->entries + i;
        printf("about to fill in data for partial fill %u", (prev_order->quantity - remaining_quantity));
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

void mbo_partial_fill_insert(MBORunner* old, MBORunner* new, u16 price, u32 remaining_quantity, CB* fills) {
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
        u32 order_quantity = prev_order->quantity;

        if (order_quantity > remaining_quantity) {
            //prev_order->quantity -= remaining_quantity;
            Fill f = {
                .order_id = prev_order_id, 
                .quantity_filled = remaining_quantity, 
                .partial = 1};
            partial_fill = 1;
            cb_queue(fills, &f);
            printf("c filling %u\n", prev_order_id);
            break;
        } else {
            // fill resting order entirely
            remaining_orders_on_level--;
            remaining_quantity -= order_quantity;
            Fill f = {
                .order_id = prev_order_id, 
                .quantity_filled = order_quantity};
            cb_queue(fills, &f);
            printf("d filling %u\n", prev_order_id);
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

u32 ob_canrep(FL* orders, u32 order_id, void* old_mbo_raw, void* new_mbo_raw, CB* fills) {

    MBO* old_mbo = (MBO*)old_mbo_raw;
    MBO* new_mbo = (MBO*)new_mbo_raw;

    // general serialization form:
    //[0 - lowest_untouched)something(highest_untouched - end]
    //+ cancel index


    // these specifically refer to indicies in the old mbo
    u16 lui;
    u16 hui;
    u16 cancel_index;
    u8 op_type = OP_TYPE_INVALID;

    // secondary specs
    u16 append_level;
    u16 modified_level;

    u8 cancel_was_sole = 0;

    // fito_rst figure out level count
    Order* rep = (Order*)fl_get(orders, order_id);
    u16 price = rep->price;
    u16 quantity = rep->quantity;
    u16 remaining_quantity = quantity;
    u8 direction = (rep->status >> BUY_DIRECTION_BIT) & 1;
    u16 cancel_id = rep->other_id;
    Order* can;

    printf("canrep %u\n", rep->status >> CAN_REP_BIT);

    u8 is_can_rep = (rep->status >> CAN_REP_BIT) & 1;
    // if not, we're going to assume it's a normal "add"
    // we'll get to plain cancel in a bit, that's much simpler 

    // but we need to wire in this assumption

    if (is_can_rep){
        printf("requetsed to cancel %u\n", cancel_id);
        can = (Order*)fl_get(orders, cancel_id);
        for (u8 i = 0; i < old_mbo->level_count; i++) {
            MBOIndex * level = old_mbo->levels + i;
            if (level->price == can->price){
                cancel_index = i;
                if (level->quantity == can->quantity)
                    cancel_was_sole = 1;
                break;
            }
        }
    }

    if (is_can_rep && rep->price == can->price && ((rep->status >> BUY_DIRECTION_BIT) & 1) == ((can->status >> BUY_DIRECTION_BIT & 1))) {
        lui = cancel_index;
        hui = cancel_index;
        op_type = CAN_REP;

        // assuming rep quanity not zero
        new_mbo->level_count = old_mbo->level_count;
    } else {
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
        printf("has opps %u old lvl price %u price %u\n", has_opponents, old_level->price, price);
        if (has_opponents && (multiplier)*(old_level->price) <= multiplier*price) {
            untouched_below = start_search;
            untouched_above = top;
            // this will also take the lowest ask -> replace with bid case

            for(u16 current_level = start_search; ; current_level += multiplier) {
                old_level = old_mbo->levels + current_level;

                if (remaining_quantity > 0 && multiplier*price < multiplier*old_level->price) {
                    op_type = REST_REMAINDER;
                    untouched_above = current_level - multiplier;
                    break;
                }

                // could be zero, the same as eating the entire level
                u32 effective_level_quantity = old_level->quantity;
                if (is_can_rep && old_level->price == can->price)
                    effective_level_quantity -= can->quantity;

                if (effective_level_quantity >= remaining_quantity) {
                    untouched_above = current_level;
                }
                printf("current level %u effective level %u remaining q %u\n", current_level, effective_level_quantity, remaining_quantity);

                if (effective_level_quantity <= remaining_quantity) {
                    // fill
                    // this looks like a good use case for runners
                    void* old_run = (_data_start((void*)old_mbo) + old_level->byte_offset);

                    MBOLevel* mod_level = (MBOLevel*)old_run;
                    for (u16 i = 0; i < mod_level->order_count; i++) {
                        Fill f = { 
                            .order_id = mod_level->entries[i].order_id,
                            .quantity_filled = mod_level->entries[i].quantity};
                        cb_queue(fills, &f);
                        printf("f filling %u\n", mod_level->entries[i].order_id);
                    }  
                }

                if (effective_level_quantity > remaining_quantity){
                    printf("effective level quantity more than remaining\n");
                    op_type = FILL_SOME;
                    modified_level = current_level;
                    break;
                } else if (effective_level_quantity == remaining_quantity) {
                    remaining_quantity -= effective_level_quantity;
                    op_type = EXACT;
                    break;
                } else {
                    remaining_quantity -= effective_level_quantity;
                }

                if (current_level == top) {
                    if (remaining_quantity > 0) {
                        op_type = REST_REMAINDER;
                        untouched_above = top;
                    }
                    break;
                }

            }

        } else {

            if (direction == 1 && hi_bid_index != MAX_U16 && old_mbo->levels[hi_bid_index].price < price) {
                op_type = NEW;
                untouched_below = hi_bid_index + 1;
                untouched_above = hi_bid_index;
            } else {
                for(; start_search != bottom; start_search -= multiplier) {
                    u16 level_price = old_mbo->levels[(u16)(start_search-multiplier)].price;

                    if (price*multiplier == level_price*multiplier) {
                        // does this math work out? or is this bytes?
                        op_type = APPEND;
                        untouched_below = start_search-multiplier;
                        untouched_above = start_search-multiplier;

                        append_level = start_search-multiplier;

                        break;
                    }       
                    if (price*multiplier > level_price*multiplier) {
                        // this will include all levels currently in old_mbo
                        op_type = NEW; 
                        untouched_below = start_search;
                        untouched_above = start_search-multiplier;
                        break;
                    } 
                }
                

                // did not find, it needs to be put at bottom
                if (op_type == OP_TYPE_INVALID) {
                    op_type = NEW;
                    untouched_below = bottom;
                    untouched_above = bottom - multiplier; // iffy on this
                }
            }

        }
        lui = direction == BUY ? untouched_below : untouched_above;
        hui = direction == BUY ? untouched_above : untouched_below;
    }

    // ok at this point we have lui and hui and cancel_index and various other types
    // first lets figure out level count

    // what are untouched
    // the rest depend on "op_type"
    u16 level_count = (lui - 0) + (old_mbo->level_count - hui - 1);

    printf("level count %u lui %u hui %u op_type %u\n", level_count, lui, hui, op_type);

    // can be simplified to +1, -1 only for exact but this explanation is good
    if (op_type == NEW){
        // price level did not exist, CANNOT be cancel id level, must be in lui/hui
        level_count++;
    } else if (op_type == APPEND) {
        // append means that we went from limit to antoher limit
        // it CANNOT be the same price same side, toehrwise it would be a CAN_REP_LEVEL
        // and CANNOT be same price opposide side other wise it would have been marketed
        // there fore MUST be in hui/lui
        level_count++;
    } else if (op_type == REST_REMAINDER) {
        // this one is toughest to think about
        // but the edge case is low ask -> hi bid
        // if it were the case that the cancel id was somewhere in between lui/hui
        // then it MUST have been picked up by walking the book
        // if it was lower, it wouldn't have been hit
        // if it was higher, it wouldn't have been hit either
        // so if it's not in lui/hui, it MUST have been consumed so dw about it
        level_count++;
    } else if (op_type == FILL_SOME) {
        // took a bite out of a level via fill
        // the cancelled order CANNOT be the last remaining order of this level
        // otherwise level it would've been wiped 
        // there fore even if we remove it, there will be op_type order after to keep this level here
        level_count++;
    } else if (op_type == CAN_REP) {
        // by definition cancel id is NOT in hui/lui, but there is just one level in between them
        // with at least the replace order
        level_count++;
    } else if (op_type == EXACT) {
        // there is nothign but lui and hui, cancel only affects us if it's in hui/lui
        level_count += 0;
    }

    if (is_can_rep) {
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

    // ok now we can really have fun
    // copy in lower block
    // do op_type crazy
    // copy in higher block

    MBORunner* new_runner = mbor_init(new_mbo);
    MBORunner* old_runner = mbor_init(old_mbo);

    u16 i = 0;
    for ( ; i < lui; i++) {
        // ah we do need to be a bit careful
        if (is_can_rep && old_runner->metadata->price == can->price) {
            if (cancel_was_sole){
                //skip level
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

    u16 hbi;

    // ok we have a five way branch here
    if (op_type == NEW){
        // assuming cancel was sole...

        // a few scenarios
        // 0 < new < cancel < old hbi < end - +0
        // 0 < new < old hbi < cancel < end - +1
        // 0 < cancel < new < old hbi < end - +0
        // 0 < old_hbi < cancel < new < end - +0
        // 0 < cancel < old hbi < new < end - -1
        // 0 < old_hbi < new < cancel < end - +0

        // 0 < new < cancel == old hbi < end - +0
        // 0 < old_hbi == cancel < new < end - -1 // ask

        // bruh i have no idae

        u16 new = new_runner->index;

        hbi = old_mbo->hi_bid_index;

        if (is_can_rep && cancel_index < lui && cancel_was_sole && cancel_index <= hbi)
            hbi--;

        if (direction == BUY)
            hbi++;
        /*if (is_can_rep){
          if (hbi == MAX_U16){
          if (direction == BUY)
          hbi++;
          } else {
          if (new < hbi && hbi < cancel_index)
          hbi++;
          else if (cancel_index <= hbi && hbi < new)
          hbi--;
          }
          } else {
          if (hbi == MAX_U16){
          if (direction == BUY)
          hbi++;
          } else {
          if (new < hbi)
          hbi++;
          else if (new > hbi)
          hbi--;
          }
          }*/

        // old not relevant
        mbo_insert_level(new_runner, order_id, price, quantity);
    } else if (op_type == APPEND) {

        if (is_can_rep && cancel_was_sole && cancel_index < old_mbo->hi_bid_index)
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
        mbo_insert_level(new_runner, order_id, price, remaining_quantity);
    } else if (op_type == FILL_SOME) {
        if (direction == BUY)
            hbi = new_runner->index - 1;
        else 
            hbi = new_runner->index;

        mbo_to_index(old_runner, modified_level);
        // need to make sure to filter out cancelid too

        // the most complex one, for the most complex cases
        // fill, but dont coutn the cancel_id
        mbo_fill_remove(old_runner, new_runner, price, remaining_quantity, fills, is_can_rep ? cancel_id: MAX_U32);
    } else if (op_type == CAN_REP) {
        // we can assume is_can_rep
        hbi = old_mbo->hi_bid_index;
        // old already in correct position
        mbo_splice_level(old_runner, new_runner, cancel_id);
        // need to decompose this into copy + split
        mbo_app_level(new_runner, order_id, quantity);
    } else if (op_type == EXACT) {
        hbi = new_runner->index - 1;

        // skip
    }

    if (op_type != EXACT)
        mbo_jump(new_runner);


    mbo_to_index(old_runner, hui + 1);
    for (i = hui + 1; i < old_mbo->level_count; i++){
        if (is_can_rep && old_runner->metadata->price == can->price) {
            if (cancel_was_sole){
                printf("copying hui, cancel was sole\n");
                //skip level
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

    new_mbo->hi_bid_index = hbi;
    if (new_mbo->level_count == 0)
        new_mbo->hi_bid_index = MAX_U16;
    // then do some calculation like
    return ((void*)(new_runner->level)) - new_mbo_raw;

}

