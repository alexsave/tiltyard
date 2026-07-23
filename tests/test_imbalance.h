#ifndef TEST_IMBALANCE_H
#define TEST_IMBALANCE_H

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "server.h"
#include "order.h"
#include "constants.h"
#include "fl.h"
#include "client_settings.h"

// the running closing-interest totals behind the NOII feed, and the NOII add-on fee

extern void server_order(ServerContext* sc, u32 exec_order_id);
extern void server_auction(ServerContext* sc);

static u32 imb_park(ServerContext* sc, u32 client, u8 is_buy, u16 price, u16 qty) {
    Order o = {0};
    o.client_id = client;
    o.price = price;
    o.quantity = qty;
    o.status = is_buy ? (1 << BUY_DIRECTION_BIT) : 0;
    u32 id = fl_insert(sc->orders, &o);
    server_order(sc, id);
    return id;
}

static void imb_cancel(ServerContext* sc, u32 client, u32 target) {
    Order o = {0};
    o.client_id = client;
    o.other_id = target;
    o.status = (1 << CANCEL_BIT);
    server_order(sc, fl_insert(sc->orders, &o));
}

void test_imbalance() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 5);
    ClientSettings* c0 = sc->client_settings + 0;
    ClientSettings* c1 = sc->client_settings + 1;

    sc->mark = 100;
    sc->auctioning = 1;

    // parked closing interest accrues into the running totals as orders come in
    u32 bid = imb_park(sc, 1, 1, 100, 10); // c1 buys 10
    imb_park(sc, 0, 0, 100, 4);            // c0 sells 4
    assert(sc->imbalance_buy == 10);
    assert(sc->imbalance_sell == 4);

    // a cancel backs its quantity out
    imb_cancel(sc, 1, bid);
    assert(sc->imbalance_buy == 0);
    assert(sc->imbalance_sell == 4);

    // re-add something crossable, then the cross clears and resets the imbalance
    imb_park(sc, 1, 1, 100, 4);
    assert(sc->imbalance_buy == 4);
    server_auction(sc);
    assert(sc->imbalance_buy == 0 && sc->imbalance_sell == 0);

    // the NOII add-on bills on top of the base tier; the free tier without it pays nothing
    c0->sub_tier = TIER_MBO;
    c0->noii = 1;
    c1->sub_tier = TIER_FREE;
    c1->noii = 0;
    i64 c0_before = c0->cash, c1_before = c1->cash;
    u64 ex_before = sc->exchange_cash;
    server_eom(sc);
    assert(c0->cash == c0_before - (8400 + 500));
    assert(c1->cash == c1_before);
    assert(sc->exchange_cash == ex_before + 8400 + 500);

    printf("test_imbalance passed\n");
    server_free(sc);
}

#endif
