#ifndef TEST_FILLS_H
#define TEST_FILLS_H

#include <assert.h>
#include <stdlib.h>
#include "ob.h"
#include "order.h"
#include "constants.h"
#include "fl.h"
#include "cb.h"
#include "fill.h"

// one price level holding `n` resting orders of `quantity` each, on the side `is_buy` says.
// hi_bid_index is set to match, so the level reads as the whole bid or ask side of the book
static void build_one_level(FL* orders, void* mbo_raw, u16 price, u16 quantity, u16 n,
                            u8 is_buy, u32* ids_out) {
    MBO* mbo = (MBO*)mbo_raw;
    mbo->level_count = 1;
    mbo->hi_bid_index = is_buy ? 0 : MAX_U16;

    mbo->levels[0].price = price;
    mbo->levels[0].quantity = (u32)quantity * n;
    mbo->levels[0].byte_offset = 0;

    MBOLevel* lv = (MBOLevel*)mbo_data_start(mbo_raw);
    lv->order_count = n;

    for (u16 i = 0; i < n; i++) {
        Order o = {0};
        o.price = price;
        o.quantity = quantity;
        o.status = is_buy ? (1 << BUY_DIRECTION_BIT) : 0;
        u32 id = fl_insert(orders, &o);
        if (ids_out)
            ids_out[i] = id;

        lv->entries[i].order_id = id;
        lv->entries[i].quantity = quantity;
    }
}

// a taker that consumes a whole number of resting orders and stops exactly on the boundary.
// mbo_fill_remove walks the level order by order, subtracting as it goes, and decides "this one
// is only partly filled" with `order_quantity > remaining_quantity`. once remaining hits 0 that
// test is true for the very next resting order - it has quantity, the taker has nothing left -
// so the walk emitted a fill of zero shares flagged partial against an order it never touched.
//
// the book came out right (the entry was rewritten with its quantity unchanged), but the fill
// went out as a real one: a zero-volume print on the tape and in the candles, a mark update that
// ran the stop and wake sweeps, and a FILL_BIT | PARTIAL_FILL_BIT notification to a maker whose
// order had not moved
void test_fill_stops_on_boundary() {
    void* old = calloc(1, 1 << 20);
    void* new = calloc(1, 1 << 20);

    FL* orders = fl_init(sizeof(Order), MAX_U32);
    CB* fills = cb_init(sizeof(Fill));

    // three 200-share bids resting at 100
    u32 ids[3];
    build_one_level(orders, old, 100, 200, 3, 1, ids);

    // sell 400 IOC into them - takes the first two exactly, third must not be touched
    Order taker = {0};
    taker.price = 100;
    taker.quantity = 400;
    taker.status = (1 << IOC_BIT); // sell
    u32 taker_id = fl_insert(orders, &taker);

    ob_canrep(orders, taker_id, old, new, fills);

    // exactly two fills, 200 each, and nothing reported against the third order
    u32 n_fills = 0;
    while (!cb_is_empty(fills)) {
        Fill* f = (Fill*)cb_deque(fills);
        printf("fill #%u q %u partial %u\n", f->order_id, f->quantity_filled, f->partial);

        assert(f->quantity_filled > 0);   // a zero-share fill is not a trade
        assert(f->order_id != ids[2]);    // the third order was never reached
        assert(f->quantity_filled == 200);
        assert(!f->partial);
        n_fills++;
    }
    assert(n_fills == 2);

    // and the book still has the untouched order sitting there in full
    MBO* nm = (MBO*)new;
    assert(nm->level_count == 1);
    assert(nm->levels[0].price == 100);
    assert(nm->levels[0].quantity == 200);

    MBOLevel* lv = (MBOLevel*)(mbo_data_start(new) + nm->levels[0].byte_offset);
    assert(lv->order_count == 1);
    assert(lv->entries[0].order_id == ids[2]);
    assert(lv->entries[0].quantity == 200);

    free(old);
    free(new);
    fl_free(orders);
    cb_free(fills);

    printf("test_fill_stops_on_boundary passed\n");
}

// the genuine partial still has to work: a taker that stops mid-order fills that order for what
// it took and leaves the rest resting, flagged partial so settlement keeps the order alive
void test_fill_partial_still_reported() {
    void* old = calloc(1, 1 << 20);
    void* new = calloc(1, 1 << 20);

    FL* orders = fl_init(sizeof(Order), MAX_U32);
    CB* fills = cb_init(sizeof(Fill));

    u32 ids[3];
    build_one_level(orders, old, 100, 200, 3, 1, ids);

    // sell 300: takes the first whole, half of the second
    Order taker = {0};
    taker.price = 100;
    taker.quantity = 300;
    taker.status = (1 << IOC_BIT);
    u32 taker_id = fl_insert(orders, &taker);

    ob_canrep(orders, taker_id, old, new, fills);

    Fill* f0 = (Fill*)cb_deque(fills);
    assert(f0->order_id == ids[0] && f0->quantity_filled == 200 && !f0->partial);

    Fill* f1 = (Fill*)cb_deque(fills);
    assert(f1->order_id == ids[1] && f1->quantity_filled == 100 && f1->partial);

    assert(cb_is_empty(fills)); // ids[2] untouched, no third fill

    // 100 left of the second order plus the whole third
    MBO* nm = (MBO*)new;
    assert(nm->levels[0].quantity == 300);

    MBOLevel* lv = (MBOLevel*)(mbo_data_start(new) + nm->levels[0].byte_offset);
    assert(lv->order_count == 2);
    assert(lv->entries[0].order_id == ids[1] && lv->entries[0].quantity == 100);
    assert(lv->entries[1].order_id == ids[2] && lv->entries[1].quantity == 200);

    free(old);
    free(new);
    fl_free(orders);
    cb_free(fills);

    printf("test_fill_partial_still_reported passed\n");
}

void test_fills() {
    test_fill_stops_on_boundary();
    test_fill_partial_still_reported();
}

#endif
