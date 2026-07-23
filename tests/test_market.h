#ifndef TEST_MARKET_H
#define TEST_MARKET_H

#include <assert.h>
#include <stdlib.h>
#include "ob.h"
#include "order.h"
#include "constants.h"
#include "fl.h"
#include "cb.h"
#include "fill.h"

// market orders + tif. two books we ping pong between, the way server swaps last_mbo

static void* mkt_a;
static void* mkt_b;
static u8 mkt_which;

static void* mkt_cur() { return mkt_which ? mkt_b : mkt_a; }
static void* mkt_nxt() { return mkt_which ? mkt_a : mkt_b; }

static void mkt_reset() {
    mkt_which = 0;
    ((MBO*)mkt_a)->level_count = 0;
    ((MBO*)mkt_a)->hi_bid_index = MAX_U16;
}

// one order through the book, the same call the server makes. tif 0 is gtc
static void mkt_send(FL* orders, CB* fills, u16 price, u16 quantity, u8 is_buy, u16 tif) {
    Order o = {0};
    o.price = price;
    o.quantity = quantity;
    o.status = (is_buy ? (1 << BUY_DIRECTION_BIT) : 0) | tif;
    u32 id = fl_insert(orders, &o);
    ob_canrep(orders, id, mkt_cur(), mkt_nxt(), fills);
    mkt_which = !mkt_which;
}

// same, but hand back the id so we can cancel it later
static u32 mkt_rest(FL* orders, CB* fills, u16 price, u16 quantity, u8 is_buy) {
    Order o = {0};
    o.price = price;
    o.quantity = quantity;
    o.status = is_buy ? (1 << BUY_DIRECTION_BIT) : 0;
    u32 id = fl_insert(orders, &o);
    ob_canrep(orders, id, mkt_cur(), mkt_nxt(), fills);
    mkt_which = !mkt_which;
    return id;
}

// the mass quote pull: one pair, both legs a pure cancel
static void mkt_pull(FL* orders, CB* fills, u32 cancel_bid, u32 cancel_ask) {
    Order bid = {0};
    bid.status = (1 << BUY_DIRECTION_BIT) | (1 << CANCEL_BIT);
    bid.other_id = cancel_bid;
    u32 bid_id = fl_insert(orders, &bid);

    Order ask = {0};
    ask.status = (1 << CANCEL_BIT);
    ask.other_id = cancel_ask;
    u32 ask_id = fl_insert(orders, &ask);

    ob_pair(orders, bid_id, ask_id, mkt_cur(), mkt_nxt(), fills);
    mkt_which = !mkt_which;
}

// how much ob queued as filled, emptying it out for the next case
static u32 mkt_drain(CB* fills) {
    u32 total = 0;
    while (!cb_is_empty(fills)) {
        Fill* f = (Fill*)cb_deque(fills);
        total += f->quantity_filled;
    }
    return total;
}

void test_market() {
    mkt_a = malloc(1 << 20);
    mkt_b = malloc(1 << 20);

    FL* orders = fl_init(sizeof(Order), MAX_U32);
    CB* fills = cb_init(sizeof(Fill));

    u16 MKT_IOC = (1 << IS_MARKET_BIT) | (1 << IOC_BIT);
    MBO* m;

    // sweeps two ask levels and bites into the second
    mkt_reset();
    mkt_send(orders, fills, 110, 10, 0, 0);
    mkt_send(orders, fills, 120, 5, 0, 0);
    mkt_drain(fills);
    mkt_send(orders, fills, 0, 12, 1, MKT_IOC);
    m = (MBO*)mkt_cur();
    assert(mkt_drain(fills) == 12);
    assert(m->level_count == 1);
    assert(m->levels[0].price == 120 && m->levels[0].quantity == 3);
    assert(m->hi_bid_index == MAX_U16);

    // nobody to trade with at all. must not invent a fill off the end of the levels array
    mkt_reset();
    mkt_send(orders, fills, 0, 5, 1, MKT_IOC);
    m = (MBO*)mkt_cur();
    assert(mkt_drain(fills) == 0);
    assert(m->level_count == 0 && m->hi_bid_index == MAX_U16);

    // market sell with only asks resting. no bids to hit, so the book is left alone
    mkt_reset();
    mkt_send(orders, fills, 110, 10, 0, 0);
    mkt_drain(fills);
    mkt_send(orders, fills, 0, 5, 0, MKT_IOC);
    m = (MBO*)mkt_cur();
    assert(mkt_drain(fills) == 0);
    assert(m->level_count == 1 && m->levels[0].price == 110 && m->levels[0].quantity == 10);
    assert(m->hi_bid_index == MAX_U16);

    // eats every bid, and the 5 it couldnt fill is dropped rather than rested
    mkt_reset();
    mkt_send(orders, fills, 100, 10, 1, 0);
    mkt_send(orders, fills, 90, 10, 1, 0);
    mkt_drain(fills);
    mkt_send(orders, fills, 0, 25, 0, MKT_IOC);
    m = (MBO*)mkt_cur();
    assert(mkt_drain(fills) == 20);
    assert(m->level_count == 0 && m->hi_bid_index == MAX_U16);

    // a limit ioc takes what it can and rests nothing
    mkt_reset();
    mkt_send(orders, fills, 110, 10, 0, 0);
    mkt_drain(fills);
    mkt_send(orders, fills, 115, 20, 1, (1 << IOC_BIT));
    m = (MBO*)mkt_cur();
    assert(mkt_drain(fills) == 10);
    assert(m->level_count == 0 && m->hi_bid_index == MAX_U16);

    // an ioc that crosses nothing rests nothing. the fill paths drop the residual only after
    // touching a level, so the zero-fill case used to fall through to the resting branch and
    // leave a level the server had already rejected and unreserved - unfunded, uncancellable,
    // and best-of-book, which is exactly what makes every replica stop seeing the other side
    mkt_reset();
    mkt_send(orders, fills, 110, 10, 0, 0);
    mkt_drain(fills);
    mkt_send(orders, fills, 102, 20, 1, (1 << IOC_BIT)); // bid under the ask, crosses nothing
    m = (MBO*)mkt_cur();
    assert(mkt_drain(fills) == 0);
    assert(m->level_count == 1 && m->levels[0].price == 110 && m->levels[0].quantity == 10);
    assert(m->hi_bid_index == MAX_U16);

    // same shot into an empty book: no opponents at all rather than none in range
    mkt_reset();
    mkt_send(orders, fills, 102, 20, 1, (1 << IOC_BIT));
    m = (MBO*)mkt_cur();
    assert(mkt_drain(fills) == 0);
    assert(m->level_count == 0 && m->hi_bid_index == MAX_U16);

    // and the sell side, with bids resting below the limit
    mkt_reset();
    mkt_send(orders, fills, 100, 10, 1, 0);
    mkt_drain(fills);
    mkt_send(orders, fills, 108, 20, 0, (1 << IOC_BIT));
    m = (MBO*)mkt_cur();
    assert(mkt_drain(fills) == 0);
    assert(m->level_count == 1 && m->levels[0].price == 100 && m->levels[0].quantity == 10);
    assert(m->hi_bid_index == 0);

    // the exact same order as gtc still rests its remainder, like it always did
    mkt_reset();
    mkt_send(orders, fills, 110, 10, 0, 0);
    mkt_drain(fills);
    mkt_send(orders, fills, 115, 20, 1, 0);
    m = (MBO*)mkt_cur();
    assert(mkt_drain(fills) == 10);
    assert(m->level_count == 1);
    assert(m->levels[0].price == 115 && m->levels[0].quantity == 10);
    assert(m->hi_bid_index == 0);

    // a pair pulling both quotes at once. both legs wipe their level, and an empty book
    // must report no bids - a stale hi_bid_index here reads as liquidity that isn't there
    mkt_reset();
    u32 bid_id = mkt_rest(orders, fills, 100, 10, 1);
    u32 ask_id = mkt_rest(orders, fills, 110, 10, 0);
    mkt_drain(fills);
    mkt_pull(orders, fills, bid_id, ask_id);
    m = (MBO*)mkt_cur();
    assert(m->level_count == 0);
    assert(m->hi_bid_index == MAX_U16);

    // same pull with a second bid resting underneath. the survivor is the only bid left
    mkt_reset();
    mkt_rest(orders, fills, 90, 10, 1);
    bid_id = mkt_rest(orders, fills, 100, 10, 1);
    ask_id = mkt_rest(orders, fills, 110, 10, 0);
    mkt_drain(fills);
    mkt_pull(orders, fills, bid_id, ask_id);
    m = (MBO*)mkt_cur();
    assert(m->level_count == 1);
    assert(m->levels[0].price == 90 && m->levels[0].quantity == 10);
    assert(m->hi_bid_index == 0);

    free(mkt_a);
    free(mkt_b);
    fl_free(orders);
    cb_free(fills);
}

#endif
