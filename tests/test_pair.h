#ifndef TEST_PAIR_H
#define TEST_PAIR_H

#include <assert.h>
#include <stdlib.h>
#include "ob.h"
#include "order.h"
#include "constants.h"
#include "fl.h"
#include "cb.h"
#include "fill.h"

// a first-quote atomic pair: new bid + new ask, no cancels (CAN_REP unset on both legs)
void test_pair() {
    void* old = malloc(1 << 20);
    void* new = malloc(1 << 20);
    ((MBO*)old)->level_count = 0;
    ((MBO*)old)->hi_bid_index = MAX_U16;

    FL* orders = fl_init(sizeof(Order), MAX_U32);
    CB* fills = cb_init(sizeof(Fill));

    Order bid = {0};
    bid.price = 100; bid.quantity = 10; bid.status = (1 << BUY_DIRECTION_BIT);
    u32 bid_id = fl_insert(orders, &bid);

    Order ask = {0};
    ask.price = 110; ask.quantity = 10; ask.status = 0; // sell
    u32 ask_id = fl_insert(orders, &ask);

    printf("----- BEFORE -----\n");
    mbo_dump(old);

    ob_pair(orders, bid_id, ask_id, old, new, fills);

    printf("----- AFTER -----\n");
    mbo_dump(new);

    MBO* m = (MBO*)new;
    assert(m->level_count == 2);
    assert(m->hi_bid_index == 0);         // the bid sits at index 0
    assert(m->levels[0].price == 100 && m->levels[0].quantity == 10);
    assert(m->levels[1].price == 110 && m->levels[1].quantity == 10);
    assert(cb_is_empty(fills));            // non-crossing pair, no fills

    free(old);
    free(new);
    fl_free(orders);
    cb_free(fills);
}

#endif
