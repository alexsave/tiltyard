#ifndef TEST_FEES_H
#define TEST_FEES_H

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "server.h"
#include "order.h"
#include "constants.h"
#include "fl.h"
#include "client_settings.h"

// exchange economics: maker-taker on continuous trades, EOD short borrow, EOM data fees

extern void server_order(ServerContext* sc, u32 exec_order_id);

// one order straight through the server on the continuous path
static u32 fee_limit(ServerContext* sc, u32 client, u8 is_buy, u16 price, u16 qty) {
    Order o = {0};
    o.client_id = client;
    o.price = price;
    o.quantity = qty;
    o.status = is_buy ? (1 << BUY_DIRECTION_BIT) : 0;
    u32 id = fl_insert(sc->orders, &o);
    server_order(sc, id);
    return id;
}

// send a bare ws-connect toggle for a client
static void fee_ws_toggle(ServerContext* sc, u32 client) {
    Order w = {0};
    w.client_id = client;
    w.status = (1 << WS_BIT);
    server_order(sc, fl_insert(sc->orders, &w));
}

void test_fees() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 7);
    ClientSettings* c0 = sc->client_settings + 0;
    ClientSettings* c1 = sc->client_settings + 1;

    sc->auctioning = 0;
    sc->is_open = 1;
    sc->mark = 100;

    // --- maker-taker: c0 rests an ask, c1 lifts it. c1 is the taker, c0 the maker.
    // per share in mills: taker 1000*3/1000 = 3, maker rebate 1000*2/1000 = 2, exchange keeps 1
    i64 c0_cash = c0->cash, c1_cash = c1->cash;
    u64 ex_before = sc->exchange_cash;
    fee_limit(sc, 0, 0, 100, 1000); // c0 sells 1000 @ 100, rests
    fee_limit(sc, 1, 1, 100, 1000); // c1 buys 1000 @ 100, crosses
    assert(c1->cash == c1_cash - 1000 * 100 - 3);
    assert(c0->cash == c0_cash + 1000 * 100 + 2);
    assert(sc->exchange_cash == ex_before + 1);

    // --- EOD short borrow: a 1000-share short marked at 100.
    // short value 100*1000 = 100000, borrow 100000 * 300 / (10000*360) = 8
    c0->shares = -1000;
    sc->mark = 100;
    i64 c0_pre_eod = c0->cash;
    i64 c1_pre_eod = c1->cash; // c1 is long, owes nothing
    u64 ex_pre_eod = sc->exchange_cash;
    server_eod(sc);
    assert(c0->cash == c0_pre_eod - 8);
    assert(c1->cash == c1_pre_eod);
    assert(sc->exchange_cash == ex_pre_eod + 8);

    // --- EOM data fees: full-depth tier pays NYSE Integrated, free tier pays nothing
    c0->sub_tier = TIER_MBO;
    c1->sub_tier = TIER_FREE;
    i64 c0_pre_eom = c0->cash, c1_pre_eom = c1->cash;
    u64 ex_pre_eom = sc->exchange_cash;
    server_eom(sc);
    assert(c0->cash == c0_pre_eom - 8400);
    assert(c1->cash == c1_pre_eom);
    assert(sc->exchange_cash == ex_pre_eom + 8400);

    // --- free tier can't open a ws stream, a paid tier can
    c1->sub_tier = TIER_FREE;
    c1->ws = 0;
    fee_ws_toggle(sc, 1);
    assert(c1->ws == 0); // refused

    c0->sub_tier = TIER_MBO;
    c0->ws = 0;
    fee_ws_toggle(sc, 0);
    assert(c0->ws == 1); // connected

    printf("test_fees passed\n");

    server_free(sc);
}

#endif
