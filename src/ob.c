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
// mbor_init is always called exactly twice per top-level ob_* op (old then new), never nested,
// single-threaded - so a 2-slot static pool replaces the malloc/free pair with no growth needed,
// since MBORunner is a small fixed-size struct regardless of book size
static MBORunner mbor_pool[2];
static u8 mbor_pool_idx = 0;

MBORunner * mbor_init(MBO* mbo) {
    MBORunner * mbor = &mbor_pool[mbor_pool_idx];
    mbor_pool_idx ^= 1;

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

// rebuild one price level after a marketable order ate into it. remaining_quantity is what the
// taker still has to spend when it reaches this level; it fills the resting orders FIFO. up to two
// order ids are cancels riding in the same book pass (the two legs of an atomic pair): a cancelled
// order is being pulled, so it is never filled and never copied through - its size just comes off
// the level. this is what stops one leg's marketable cross from filling, or leaving a ghost of, the
// OTHER leg's cancel target when both land on the same shared level.
void mbo_fill_remove(MBORunner* old, MBORunner* new, u16 price, u32 remaining_quantity, CB* fills, u32 cancel_id, u32 cancel_id2) {
    new->metadata->price = old->metadata->price;
    new->metadata->byte_offset = ((void*)(new->level)) - new->data_start;

    MBOLevel* mod_level = old->level;
    MBOLevel* out_level = new->level;

    u32 level_quantity = old->metadata->quantity;
    u16 out = 0;

    for (u16 i = 0; i < mod_level->order_count; i++) {
        MBOEntry* e = mod_level->entries + i;
        u32 oid = e->order_id;
        u32 oq = e->quantity;

        // pulled by either leg: cancelled, so never fill and never copy - just drop its size
        if (oid == cancel_id || oid == cancel_id2) {
            level_quantity -= oq;
            continue;
        }

        if (remaining_quantity == 0) {
            // taker spent: this order survives whole
            out_level->entries[out].order_id = oid;
            out_level->entries[out].quantity = oq;
            out++;
        } else if (oq > remaining_quantity) {
            // partial fill of this order - it stays with the remainder, taker is now spent
            Fill f = { .order_id = oid, .quantity_filled = remaining_quantity, .partial = 1 };
            cb_queue(fills, &f);
            level_quantity -= remaining_quantity;
            out_level->entries[out].order_id = oid;
            out_level->entries[out].quantity = oq - remaining_quantity;
            out++;
            remaining_quantity = 0;
        } else {
            // full fill of this order - consumed, not copied
            Fill f = { .order_id = oid, .quantity_filled = oq };
            cb_queue(fills, &f);
            level_quantity -= oq;
            remaining_quantity -= oq;
        }
    }

    new->metadata->quantity = level_quantity;
    out_level->order_count = out;
}

// used for cancellations. the cancel is matched by id, so the count written is the number of
// entries actually carried over - never old_count - 1. an id that is missing (already filled
// out from under the cancel) or that appears more than once would otherwise leave the level
// declaring more entries than were written, and the surplus reads whatever the reused buffer
// last held: a ghost order that the crossing walk in ob_affected_range will happily fill
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
    new->level->order_count = j;
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

// is order_id among cancels[0..n)? their low 32 bits are ids, ascending within one price
// (the heap breaks price ties by id), so a binary search does it
u8 cancels_hit(u64* cancels, u32 n, u32 order_id) {
    u32 lo = 0;
    u32 hi = n;
    while (lo < hi) {
        u32 mid = (lo + hi) >> 1;
        u32 mid_id = (u32)(cancels[mid] & MAX_U32);
        if (mid_id == order_id)
            return 1;
        if (mid_id < order_id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

// copy one level minus any entry whose id is in cancels[0..hits) (just this price's run).
// only call when something survives (hits < order_count) - a fully cancelled level has no
// index slot in the new book, so there is nothing here we could legally write
void mbo_prune_level(MBORunner* old, MBORunner* new, u64* cancels, u32 hits) {
    MBOLevel* old_level = old->level;

    new->metadata->price = old->metadata->price;
    new->metadata->byte_offset = ((void*)(new->level)) - new->data_start;

    u32 kept_quantity = 0;
    u16 w = 0;
    for (u16 i = 0; i < old_level->order_count; i++) {
        MBOEntry* e = old_level->entries + i;
        if (cancels_hit(cancels, hits, e->order_id))
            continue;
        new->level->entries[w].order_id = e->order_id;
        new->level->entries[w].quantity = e->quantity;
        kept_quantity += e->quantity;
        w++;
    }

    new->metadata->quantity = kept_quantity;
    new->level->order_count = w;
}

// advance *c past any cancels priced below `price`, then return how many sit exactly at it.
// the book and the sorted array both climb in price, so one shared cursor walks both
u32 level_hits(u64* sorted, u32 n, u32* c, u16 price) {
    while (*c < n && (u16)(sorted[*c] >> 32) < price)
        (*c)++;
    u32 hits = 0;
    while (*c + hits < n && (u16)(sorted[*c + hits] >> 32) == price)
        hits++;
    return hits;
}

// prune a whole batch of resting orders in one new snapshot. walks the book low price to high
// alongside the sorted cancels, dropping the cancelled entries off each level and any level
// that empties out. see ob.h for the buffer's layout
u32 ob_expire(CB* cancels, u32 n, void* old_mbo_raw, void* new_mbo_raw) {
    MBO* old_mbo = (MBO*)old_mbo_raw;
    MBO* new_mbo = (MBO*)new_mbo_raw;

    // filled from empty, so the entries sit flat at [0, n)
    u64* sorted = (u64*)cancels->buffer;
    u16 old_hi_bid = old_mbo->hi_bid_index;

    // pass one: how many levels survive, and how many survivors are bids. we need the new
    // level_count before mbor_init, because that is what fixes where the level data starts
    u16 new_level_count = 0;
    u16 new_bid_levels = 0;
    u32 c = 0;
    for (u16 i = 0; i < old_mbo->level_count; i++) {
        MBOIndex* lvl = old_mbo->levels + i;
        u32 hits = level_hits(sorted, n, &c, lvl->price);

        MBOLevel* data = (MBOLevel*)(mbo_data_start(old_mbo) + lvl->byte_offset);
        if (hits < data->order_count) {
            new_level_count++;
            if (old_hi_bid != MAX_U16 && i <= old_hi_bid)
                new_bid_levels++;
        }
        c += hits;
    }

    new_mbo->level_count = new_level_count;
    new_mbo->hi_bid_index = new_bid_levels ? new_bid_levels - 1 : MAX_U16;

    // pass two: build it, now that data_start is fixed
    MBORunner* old_runner = mbor_init(old_mbo);
    MBORunner* new_runner = mbor_init(new_mbo);
    c = 0;
    for (u16 i = 0; i < old_mbo->level_count; i++) {
        u32 hits = level_hits(sorted, n, &c, old_runner->metadata->price);

        if (hits == 0) {
            mbo_copy_level(old_runner, new_runner);
            mbo_jump(new_runner);
        } else if (hits < old_runner->level->order_count) {
            mbo_prune_level(old_runner, new_runner, sorted + c, hits);
            mbo_jump(new_runner);
        }
        // else every order on the level was cancelled: write nothing at all - the new book
        // has no index slot for it, so even a speculative metadata write would land on data
        c += hits;
        mbo_jump(old_runner);
    }

    u32 new_size = ((void*)(new_runner->level)) - new_mbo_raw;
    // old_runner/new_runner are pooled statics now (see mbor_init), nothing to free
    return new_size;
}

// figure out which old-mbo levels a single side (bid OR ask) touches, and how
// lui/hui are the untouched indicies bracketing the affected range, op_type says what to do
// remaining_out gets the leftover quantity after any fills, modified_level the level FILL_SOME bit into
void ob_affected_range(MBO* old_mbo, Order* rep, Order* can,
                       u8 is_can_rep, u8 is_cancel, u32 cancel_id,
                       u16 cancel_index, u8 cancel_was_sole,
                       Order* co_can, u32 co_cancel_id,
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
            // only a strict reduction keeps its place - that's the partial cancel nasdaq keeps
            // priority for. same size at the same price changed nothing, so it re-times like any
            // other replace and goes to the back, which is what stops a maker requoting for free.
            //
            // nasdaq is stricter than this: per the OUCH spec a replace ALWAYS re-times, and the
            // only way to shed size and keep priority is a cancel carrying the new intended size.
            // we fold that into the shrink op instead of a second message type. no venue rejects
            // a no-change replace - they take it, publish it, and bill for it: nasdaq's excessive
            // messaging policy charges $0.005/order past a 100:1 weighted order-to-trade ratio and
            // $0.01 past 1000:1, eurex the same idea via its OTR / excessive system usage fee.
            // losing the queue is the cheap version of that disincentive, and it needs no billing
            if (rep->quantity >= can->quantity)
                *op_type = CAN_REP;
            else
                *op_type = SHRINK;

            // assuming rep quanity not zero
        } else {
            u8 is_market = (rep->status >> IS_MARKET_BIT) & 1;
            // gtc is the default: no tif bit set rests the remainder, like a plain limit always did
            u8 is_gtc = !(((rep->status >> IOC_BIT) & 1) | ((rep->status >> FOK_BIT) & 1));

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
            if (has_opponents && (is_market || (multiplier)*(old_level->price) <= multiplier*price)) {
                untouched_below = start_search;
                untouched_above = top;
                // this will also take the lowest ask -> replace with bid case

                for(u16 current_level = start_search; ; current_level += multiplier) {
                    old_level = old_mbo->levels + current_level;

                    if (!is_market && (remaining_quantity > 0 && multiplier*price < multiplier*old_level->price)) {
                        // we dont want to touch the next level, rest the remaining quantity
                        if (is_gtc) {
                            *op_type = REST_REMAINDER;
                        } else {
                            // ioc/fok. leftover remaining_quantity is fine, EXACT writes no level = dropped
                            *op_type = EXACT;
                        }
                        untouched_above = current_level - multiplier;
                        break;
                    }

                    // could be zero, the same as eating the entire level
                    u32 effective_level_quantity = old_level->quantity;
                    // pairs let us operate on the book even when nothing is cancelled, and then can is 0.
                    // is_can_rep is only ever set here once cancel_precheck found the order resting in the
                    // book, so if we subtract, the quantity really is on this level
                    if (is_can_rep && old_level->price == can->price)
                        effective_level_quantity -= can->quantity;
                    // the other leg of a pair is pulling its own target, which can be resting
                    // right here. it is not ours to trade with, so it comes off this level's
                    // size for the same reason ours does - mbo_fill_remove already does this
                    // for the level we only bite into (cancel_id2), and a level we swallow
                    // whole needs it just as much. without it the co-leg's target is filled
                    // AND retired as a cancel, so its id is handed back twice
                    if (co_can && old_level->price == co_can->price)
                        effective_level_quantity -= co_can->quantity;

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
                            // cancelled by the other leg: pulled, never filled (see above)
                            if (entry->order_id == co_cancel_id)
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
                            else
                                *op_type = EXACT;
                            untouched_above = top;
                        }
                        break;
                    }

                }

            } else if (is_market || !is_gtc) {
                // nobody to trade with, and a market order has no price to rest at, so it evaporates.
                // an ioc/fok that crossed nothing evaporates for the other reason - it never rests,
                // and crossing zero levels is just the degenerate case of the residual drop the two
                // fill paths above already do. without this it falls through to the resting branch
                // and writes a level the server has already rejected and unreserved: a phantom bid
                // that nobody can cancel, funded by nobody, and best-of-book to every replica.
                //
                // empty affected range (lui = hui+1) + EXACT means we touch nothing and write no level.
                // the u16 wrap is intended: a sell into no bids has start_search = MAX_U16, which lands
                // lui 0 / hui MAX_U16, so hui+1 comes back to 0 and the copy ranges still cover the book.
                // a can_rep still drops its cancel - ob_copy_range splices on price, not on op_type
                *op_type = EXACT;
                untouched_below = start_search;
                untouched_above = start_search - multiplier;
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

// does the op's affected range [lui, hui] cover old level `idx`? all three are u16-modular:
// a pure insertion has lui == hui + 1 and covers nothing, and hui may be the MAX_U16 sentinel
// for "one before level 0". the subtraction keeps both cases honest where a signed compare
// would not - an empty range gives length 0, so nothing reads as covered
u8 ob_op_covers(u16 idx, u16 lui, u16 hui) {
    return (u16)(idx - lui) < (u16)(hui - lui + 1);
}

// bulk-copy `count` levels starting wherever both runners are sitting, with nothing spliced out.
// level payloads are laid out contiguously in index order - mbo_jump walks them by size, which is
// only consistent with byte_offset if they are - so a whole run of untouched levels is one memcpy
// instead of one per level. the index entries still have to be written, but every byte_offset in
// the run shifts by the SAME delta, because both source and destination are contiguous
static void ob_copy_span(MBORunner* old, MBORunner* new, u16 count) {
    if (!count)
        return;

    MBOIndex* src = old->metadata;
    MBOIndex* last = src + (count - 1);
    MBOLevel* last_level = (MBOLevel*)(old->data_start + last->byte_offset);
    u32 span = last->byte_offset + _mbo_level_size(last_level->order_count) - src->byte_offset;

    // signed: a splice earlier in this book makes the destination the shorter of the two
    i64 delta = (i64)(((void*)new->level) - new->data_start) - (i64)src->byte_offset;

    memcpy(new->level, old->level, span);

    MBOIndex* dst = new->metadata;
    for (u16 i = 0; i < count; i++) {
        dst[i].price = src[i].price;
        dst[i].quantity = src[i].quantity;
        dst[i].byte_offset = (u32)((i64)src[i].byte_offset + delta);
    }

    new->level = ((void*)new->level) + span;
    new->metadata += count;
    new->index += count;

    old->level = ((void*)old->level) + span;
    old->metadata += count;
    old->index += count;
}

// one level that a watched cancel lands on: splice the cancelled order out, drop the level if that
// order was the only one on it, or fall through to a plain copy if the price does not actually
// match. this is the original per-level body, now reached only for the few levels that need it
static void ob_copy_cut(MBORunner* old, MBORunner* new, OBSide** sides, u8 n_sides) {
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

// copy old levels [from, to) into new, splicing out any watched cancel that shows up
// (up to two, so the pair can watch both the bid and ask cancels in one pass).
//
// a range is almost always dozens of untouched levels around at most two spliced ones, so rather
// than walking level by level we locate the cuts up front and bulk-copy the runs between them.
// ob_locate_cancel already resolved each cancel to its level index, so "is it in this range" is a
// bounds check rather than a price compare per level. an index that is stale or never resolved can
// only add a spurious cut, and a cut still re-checks the price - so it copies, same as before
void ob_copy_range(MBORunner* old, MBORunner* new, u16 from, u16 to, OBSide** sides, u8 n_sides) {
    mbo_to_index(old, from);
    if (to <= from)
        return;   // empty range, but the caller still wants old parked at `from`

    u16 cut[2];
    u8 n_cut = 0;
    for (u8 s = 0; s < n_sides; s++) {
        OBSide* sd = sides[s];
        if (!(sd->is_can_rep | sd->is_cancel))
            continue;
        u16 ci = sd->cancel_index;
        if (ci < from || ci >= to)
            continue;
        // both legs can name the same level - that is one cut, handled by whichever side matches
        // first, exactly as the single pass used to
        u8 dup = 0;
        for (u8 k = 0; k < n_cut; k++)
            dup |= (cut[k] == ci);
        if (!dup)
            cut[n_cut++] = ci;
    }
    if (n_cut == 2 && cut[0] > cut[1]) {
        u16 swap = cut[0];
        cut[0] = cut[1];
        cut[1] = swap;
    }

    u16 at = from;
    for (u8 k = 0; k < n_cut; k++) {
        ob_copy_span(old, new, cut[k] - at);
        ob_copy_cut(old, new, sides, n_sides);
        at = cut[k] + 1;
    }
    ob_copy_span(old, new, to - at);
}

// serialize one side's op into new_runner. old_runner must be sitting at s->lui.
// returns the hi_bid_index this side implies for the new book.
u16 ob_apply_op(OBSide* s, MBO* old_mbo, MBORunner* old_runner, MBORunner* new_runner, CB* fills, u32 co_cancel_id) {
    u8 op_type = s->op_type;
    u8 direction = (s->rep->status >> BUY_DIRECTION_BIT) & 1;
    u16 price = s->rep->price;
    u16 quantity = s->rep->quantity;
    u32 order_id = s->order_id;
    u32 cancel_id = s->cancel_id;
    u8 is_can_rep = s->is_can_rep;
    u16 cancel_index = s->cancel_index;
    u8 cancel_was_sole = s->cancel_was_sole;

    // default to the old boundary: every branch that does not move it wants exactly this,
    // and an unhandled op_type returns a stale-but-valid index instead of stack garbage
    u16 hbi = old_mbo->hi_bid_index;

    if (op_type == NEW){
        hbi = old_mbo->hi_bid_index;

        if (hbi != MAX_U16 && is_can_rep && cancel_was_sole && cancel_index <= hbi)
            hbi--;

        if (direction == BUY)
            hbi++;

        // old not relevant
        mbo_insert_level(new_runner, order_id, price, quantity);
    } else if (op_type == APPEND) {
        // read hbi before assigning it and the guard is testing whatever was on the stack.
        // same retreat as NEW, just joining a level instead of making one, so no ++
        hbi = old_mbo->hi_bid_index;

        if (hbi != MAX_U16 && is_can_rep && cancel_was_sole && cancel_index <= hbi)
            hbi--;

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
        // fill, but drop this op's own replaced order and the other leg's cancel target if
        // either shares this crossed level (co_cancel_id is MAX_U32 when there is no other leg)
        mbo_fill_remove(old_runner, new_runner, price, s->remaining, fills, is_can_rep ? cancel_id: MAX_U32, co_cancel_id);
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

// run a side through ob_affected_range, filling in its lui/hui/op_type/etc. `co` is the pair's
// other leg (0 for a single order) - whatever it is cancelling is off limits to this leg
void ob_side_range(OBSide* s, OBSide* co, FL* orders, MBO* old_mbo, CB* fills) {
    Order* can = (s->is_can_rep | s->is_cancel) ? (Order*)fl_get(orders, s->cancel_id) : 0;
    u8 co_pulls = co && (co->is_can_rep | co->is_cancel);
    Order* co_can = co_pulls ? (Order*)fl_get(orders, co->cancel_id) : 0;
    u32 co_cancel_id = co_pulls ? co->cancel_id : MAX_U32;
    ob_affected_range(old_mbo, s->rep, can,
                      s->is_can_rep, s->is_cancel, s->cancel_id,
                      s->cancel_index, s->cancel_was_sole,
                      co_can, co_cancel_id,
                      &s->lui, &s->hui, &s->op_type,
                      &s->modified_level, &s->remaining, fills);
}

// atomic bid + ask replace/cancel. bid is bid_order_id, ask is ask_order_id.
// requires a non-crossing pair (bid price < ask price, at most one leg marketable),
// so the two affected ranges stay disjoint with the bid region below the ask region.
u32 ob_pair(FL* orders, u32 bid_order_id, u32 ask_order_id, void* old_mbo_raw, void* new_mbo_raw, CB* fills) {
    MBO* old_mbo = (MBO*)old_mbo_raw;
    MBO* new_mbo = (MBO*)new_mbo_raw;

    OBSide bid = {0};
    OBSide ask = {0};
    ob_side_init(&bid, orders, old_mbo, bid_order_id);
    ob_side_init(&ask, orders, old_mbo, ask_order_id);

    ob_side_range(&bid, &ask, orders, old_mbo, fills);
    ob_side_range(&ask, &bid, orders, old_mbo, fills);

    // serialization form: [0, bid_lui) bid_op (bid_hui, ask_lui) ask_op (ask_hui, end]
    u16 level_count = bid.lui
                    + ob_op_level_delta(bid.op_type)
                    + (ask.lui - bid.hui - 1)
                    + ob_op_level_delta(ask.op_type)
                    + (old_mbo->level_count - ask.hui - 1);

    // a sole cancel only costs a level if ob_copy_range is the one that would have carried it:
    // inside either op's range the op already decides that level's fate, and the deltas above
    // have counted it. checking only the cancel's own side double-drops a bid whose level the
    // ask op consumed, and a pure insertion (empty range, hui = MAX_U16) covers nothing at all
    u8 bid_cancel_drops = (bid.is_can_rep | bid.is_cancel) && bid.cancel_was_sole
            && !ob_op_covers(bid.cancel_index, bid.lui, bid.hui)
            && !ob_op_covers(bid.cancel_index, ask.lui, ask.hui);
    u8 ask_cancel_drops = (ask.is_can_rep | ask.is_cancel) && ask.cancel_was_sole
            && !ob_op_covers(ask.cancel_index, bid.lui, bid.hui)
            && !ob_op_covers(ask.cancel_index, ask.lui, ask.hui);

    level_count -= bid_cancel_drops + ask_cancel_drops;

    new_mbo->level_count = level_count;

    MBORunner* new_runner = mbor_init(new_mbo);
    MBORunner* old_runner = mbor_init(old_mbo);

    // either cancel can surface in any copy region, so watch both throughout
    OBSide* sides[2] = { &bid, &ask };

    // each leg's op must also pull the OTHER leg's cancel target if it lands on the same crossed
    // level - the copy ranges watch both cancels, but an op consuming its level would otherwise
    // leave the co-leg's cancelled order behind (a ghost that later phantom-fills and double-frees)
    u32 bid_co_cancel = (ask.is_can_rep | ask.is_cancel) ? ask.cancel_id : MAX_U32;
    u32 ask_co_cancel = (bid.is_can_rep | bid.is_cancel) ? bid.cancel_id : MAX_U32;

    ob_copy_range(old_runner, new_runner, 0, bid.lui, sides, 2);
    ob_apply_op(&bid, old_mbo, old_runner, new_runner, fills, bid_co_cancel);
    ob_copy_range(old_runner, new_runner, bid.hui + 1, ask.lui, sides, 2);
    ob_apply_op(&ask, old_mbo, old_runner, new_runner, fills, ask_co_cancel);
    ob_copy_range(old_runner, new_runner, ask.hui + 1, old_mbo->level_count, sides, 2);

    // hi_bid_index = (bid levels in the new book) - 1, counted region by region:
    //   lower untouched [0, bid.lui)      -> all bids                        : bid.lui
    //   bid op                            -> rests a bid unless it left no level (EXACT,
    //                                        CAN_WIPE) or the level it left is a bitten ask
    //                                        (FILL_SOME)
    //   middle untouched (bid.hui, ask.lui) -> bids up to the old boundary
    //   ask op                            -> only FILL_SOME leaves a (bitten) bid
    //   upper untouched (ask.hui, end]    -> all asks                        : 0
    u16 old_hbi = old_mbo->hi_bid_index;
    u16 bids = bid.lui
             + (bid.op_type != FILL_SOME && ob_op_level_delta(bid.op_type))
             + (ask.op_type == FILL_SOME);
    // the middle is old [bid.hui + 1, ask.lui), in the same u16-modular index arithmetic the
    // copy ranges above use - a bid op resting under the whole book carries hui = MAX_U16, so
    // the +1 wraps to 0. widening to int instead loses that and buries the count
    if (old_hbi != MAX_U16) {
        u16 mid_lo = bid.hui + 1;
        u16 mid_len = ask.lui - mid_lo;
        if (mid_len && old_hbi >= mid_lo) {
            u16 mid_bids = old_hbi - mid_lo + 1;
            bids += mid_bids < mid_len ? mid_bids : mid_len;
        }

        // the same drops the level_count adjustment above makes: a copy-region level that goes
        // away was counted as a bid up there if it sat at or below the old boundary. either leg
        // can be cancelling either side - cancel_precheck only proved it was ours and resting
        if (bid_cancel_drops && bid.cancel_index <= old_hbi)
            bids--;
        if (ask_cancel_drops && ask.cancel_index <= old_hbi)
            bids--;
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
    ob_side_range(&side, 0, orders, old_mbo, fills);

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
    u16 hbi = ob_apply_op(&side, old_mbo, old_runner, new_runner, fills, MAX_U32);

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

