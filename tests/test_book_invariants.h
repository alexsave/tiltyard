#ifndef TEST_BOOK_INVARIANTS_H
#define TEST_BOOK_INVARIANTS_H

#include <assert.h>
#include <stdlib.h>
#include "ob.h"
#include "order.h"
#include "constants.h"
#include "fl.h"
#include "cb.h"
#include "fill.h"

// two things every book coming out of ob must satisfy, and neither is checked in the hot path:
//
//   1. prices run strictly ascending across levels
//   2. every level at or below hi_bid_index holds only buys, every level above only sells
//
// break the second and ob_affected_range reads the highest bid level as the lowest ask, so a
// buy taker crosses into resting bids at a locked price. the trades that come out look normal
// - right price, right quantity - and the damage only surfaces later in settlement, where the
// fill hook picks which reserve to release from the taker's direction and gives back shares
// against an order that reserved cash. by then the op that caused it is millions of events back.
//
// call this after any ob_pair / ob_canrep in a test. it walks every order on every level, which
// is exactly why it lives here and not in ob.c
u8 book_ok(FL* orders, void* mbo_raw, const char* tag) {
    MBO* mbo = (MBO*)mbo_raw;

    for (u16 i = 1; i < mbo->level_count; i++) {
        if (mbo->levels[i - 1].price >= mbo->levels[i].price) {
            printf("ORDER-BAD [%s] level %u price %u then level %u price %u"
                   " | hi_bid_index %u level_count %u\n",
                    tag, i - 1, mbo->levels[i - 1].price, i, mbo->levels[i].price,
                    mbo->hi_bid_index, mbo->level_count);
            return 0;
        }
    }

    for (u16 i = 0; i < mbo->level_count; i++) {
        MBOLevel* lv = (MBOLevel*)(mbo_data_start(mbo_raw) + mbo->levels[i].byte_offset);
        u8 should_be_bid = mbo->hi_bid_index != MAX_U16 && i <= mbo->hi_bid_index;
        for (u16 o = 0; o < lv->order_count; o++) {
            Order* lo = (Order*)fl_get(orders, lv->entries[o].order_id);
            u8 is_buy = (lo->status >> BUY_DIRECTION_BIT) & 1;
            if (is_buy != should_be_bid) {
                printf("SIDE-BAD [%s] level %u price %u order #%u is a %s on the %s side"
                       " | hi_bid_index %u level_count %u\n",
                        tag, i, mbo->levels[i].price, lv->entries[o].order_id,
                        is_buy ? "buy" : "sell", should_be_bid ? "bid" : "ask",
                        mbo->hi_bid_index, mbo->level_count);
                return 0;
            }
        }
    }

    return 1;
}

// build a whole book in one pass: one resting order per level, prices ascending, `hbi` levels
// worth of bids at the bottom. done in one pass because mbo_data_start is a function of
// level_count, so the data block moves every time the levels array grows - appending one level
// at a time would strand every offset written before it. ids_out gets each level's order id
static void build_book(FL* orders, void* mbo_raw, const u16* prices, const u16* quantities,
                       u16 n, u16 hbi, u32* ids_out) {
    MBO* mbo = (MBO*)mbo_raw;
    mbo->level_count = n;
    mbo->hi_bid_index = hbi;

    void* data = mbo_data_start(mbo_raw);
    u32 offset = 0;

    for (u16 i = 0; i < n; i++) {
        u8 is_buy = hbi != MAX_U16 && i <= hbi;

        Order o = {0};
        o.price = prices[i];
        o.quantity = quantities[i];
        o.status = is_buy ? (1 << BUY_DIRECTION_BIT) : 0;
        u32 id = fl_insert(orders, &o);
        if (ids_out)
            ids_out[i] = id;

        mbo->levels[i].price = prices[i];
        mbo->levels[i].quantity = quantities[i];
        mbo->levels[i].byte_offset = offset;

        MBOLevel* lv = (MBOLevel*)(data + offset);
        lv->order_count = 1;
        lv->entries[0].order_id = id;
        lv->entries[0].quantity = quantities[i];

        offset += sizeof(MBOLevel) + sizeof(MBOEntry);
    }
}

// the regression this file exists for. a maker requoting both legs at once sends an atomic pair
// whose bid rests below every level currently in the book. that carries hui = MAX_U16 - "one
// before level 0", the same wrap ob_copy_range relies on - and the bid-level count used to widen
// it to int, where the sentinel reads as 65535 instead of -1 and buries the whole middle region.
// hi_bid_index came out one short, the next level up got labelled the best ask, and buy takers
// ate resting bids from then on
void test_pair_hbi_insert_below_book() {
    void* old = calloc(1, 1 << 20);
    void* new = calloc(1, 1 << 20);
    FL* orders = fl_init(sizeof(Order), MAX_U32);
    CB* fills = cb_init(sizeof(Fill));

    // book: bids at 94 and 96, asks at 98 and 108
    u16 prices[4]     = { 94, 96, 98, 108 };
    u16 quantities[4] = { 200, 200, 200, 200 };
    build_book(orders, old, prices, quantities, 4, 1, 0);

    assert(book_ok(orders, old, "seed"));

    // quote a new bid at 92 - under the whole book - paired with an ask at 110 above it
    Order bid = {0};
    bid.price = 92; bid.quantity = 200; bid.status = (1 << BUY_DIRECTION_BIT);
    u32 bid_id = fl_insert(orders, &bid);

    Order ask = {0};
    ask.price = 110; ask.quantity = 200; ask.status = 0;
    u32 ask_id = fl_insert(orders, &ask);

    ob_pair(orders, bid_id, ask_id, old, new, fills);

    MBO* nm = (MBO*)new;
    assert(nm->level_count == 6);
    // 92, 94, 96 are bids now; 98, 108, 110 are asks
    assert(nm->levels[0].price == 92);
    assert(nm->hi_bid_index == 2);
    assert(book_ok(orders, new, "pair-insert-below"));
    assert(cb_is_empty(fills)); // 92 crosses nothing

    free(old);
    free(new);
    fl_free(orders);
    cb_free(fills);

    printf("test_pair_hbi_insert_below_book passed\n");
}

// same count, other direction: a pair whose legs both join levels that already exist, each
// cancelling a sole order elsewhere in the book. both cancelled levels disappear, and the bid
// count has to give back only the one that was a bid - and only when a copy range, not one of
// the two ops, was what would have carried it
void test_pair_hbi_sole_cancel_drops() {
    void* old = calloc(1, 1 << 20);
    void* new = calloc(1, 1 << 20);
    FL* orders = fl_init(sizeof(Order), MAX_U32);
    CB* fills = cb_init(sizeof(Fill));

    // bids at 90, 92, 94; asks at 100, 102. each level holds exactly one order, so the two
    // being cancelled below are sole on their levels and take the level with them
    u16 prices[5]     = { 90, 92, 94, 100, 102 };
    u16 quantities[5] = { 200, 200, 200, 200, 200 };
    u32 ids[5];
    build_book(orders, old, prices, quantities, 5, 2, ids);
    u32 stale_bid = ids[2]; // the 94 bid
    u32 stale_ask = ids[4]; // the 102 ask

    assert(book_ok(orders, old, "seed"));

    // join the 90 bid and the 100 ask, pulling the 94 and 102 quotes in the same message
    Order bid = {0};
    bid.price = 90; bid.quantity = 200;
    bid.status = (1 << BUY_DIRECTION_BIT) | (1 << CAN_REP_BIT);
    bid.other_id = stale_bid;
    u32 bid_id = fl_insert(orders, &bid);

    Order ask = {0};
    ask.price = 100; ask.quantity = 200;
    ask.status = (1 << CAN_REP_BIT);
    ask.other_id = stale_ask;
    u32 ask_id = fl_insert(orders, &ask);

    ob_pair(orders, bid_id, ask_id, old, new, fills);

    MBO* nm = (MBO*)new;
    assert(nm->level_count == 3);  // 94 and 102 are gone
    assert(nm->hi_bid_index == 1); // 90 and 92 remain as bids, 100 is the lone ask
    assert(nm->levels[0].price == 90);
    assert(nm->levels[1].price == 92);
    assert(nm->levels[2].price == 100);
    assert(book_ok(orders, new, "pair-sole-cancel"));

    free(old);
    free(new);
    fl_free(orders);
    cb_free(fills);

    printf("test_pair_hbi_sole_cancel_drops passed\n");
}

void test_book_invariants() {
    test_pair_hbi_insert_below_book();
    test_pair_hbi_sole_cancel_drops();
}

#endif
