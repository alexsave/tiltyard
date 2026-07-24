#ifndef TEST_MBP_H
#define TEST_MBP_H

#include <assert.h>
#include <stdlib.h>
#include "server.h"
#include "order.h"
#include "constants.h"
#include "fl.h"
#include "ob.h"
#include "mbp.h"

// exercises the price-aggregated views derived from the MBO:
//   MBP   - full depth mirror of the book
//   MBP10 - fixed 20-slot window, indices 0-9 bids (9 = best bid), 10-19 asks (10 = best ask)
//   MBP1  - just best bid / best ask
// mbp_derive() runs inside server_arrival on every book change, so we just build a
// book with resting limits and read sc->last_mbp / last_mbp10 / last_mbp1.

extern void server_order(ServerContext* sc, u32 exec_order_id);

static u32 mbp_limit(ServerContext* sc, u32 client, u8 is_buy, u16 price, u16 qty) {
    Order o = {0};
    o.client_id = client;
    o.price = price;
    o.quantity = qty;
    o.status = is_buy ? (1 << BUY_DIRECTION_BIT) : 0;
    u32 id = fl_insert(sc->orders, &o);
    server_order(sc, id);
    return id;
}

void test_mbp() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 11);

    server_market_open(sc);

    // build a non-crossing book: bids 100/101/102, asks 105/106/107.
    // place the best bid (102) last so the final derive refreshes MBP10 + MBP1.
    mbp_limit(sc, 0, 0, 107, 1); // ask
    mbp_limit(sc, 0, 0, 106, 6); // ask
    mbp_limit(sc, 0, 0, 105, 4); // ask (best ask)
    mbp_limit(sc, 1, 1, 100, 5); // bid
    mbp_limit(sc, 1, 1, 101, 3); // bid
    mbp_limit(sc, 1, 1, 102, 2); // bid (best bid)

    // the views derive lazily now - reading sc->last_mbp* raw needs an ensure first, the
    // same one every pin site goes through
    mbp_ensure(sc);

    // show what the three views look like for this book
    mbp_dump(bs_get_no_ref(sc->mbp_bs, sc->last_mbp));
    mbp10_dump(bs_get_no_ref(sc->mbp10_bs, sc->last_mbp10));
    mbp1_dump(bs_get_no_ref(sc->mbp1_bs, sc->last_mbp1));

    // --- MBP: full-depth mirror, sorted ascending, bids up to hi_bid_index then asks
    MBP* mbp = (MBP*)bs_get_no_ref(sc->mbp_bs, sc->last_mbp);
    assert(mbp->level_count == 6);
    assert(mbp->hi_bid_index == 2); // index of best bid (102)
    u16 want_price[6] = {100, 101, 102, 105, 106, 107};
    u32 want_qty[6]   = {5,   3,   2,   4,   6,   1};
    for (u16 i = 0; i < 6; i++) {
        assert(mbp->levels[i].price == want_price[i]);
        assert(mbp->levels[i].quantity == want_qty[i]);
    }

    // --- MBP10: index 9 = best bid, 10 = best ask; only 3 levels each side, rest zero
    MBP10* mbp10 = (MBP10*)bs_get_no_ref(sc->mbp10_bs, sc->last_mbp10);
    // bids: slot 9,8,7 = 102,101,100
    assert(mbp10->levels[9].price == 102 && mbp10->levels[9].quantity == 2);
    assert(mbp10->levels[8].price == 101 && mbp10->levels[8].quantity == 3);
    assert(mbp10->levels[7].price == 100 && mbp10->levels[7].quantity == 5);
    // asks: slot 10,11,12 = 105,106,107
    assert(mbp10->levels[10].price == 105 && mbp10->levels[10].quantity == 4);
    assert(mbp10->levels[11].price == 106 && mbp10->levels[11].quantity == 6);
    assert(mbp10->levels[12].price == 107 && mbp10->levels[12].quantity == 1);
    // beyond the book on both sides: zeroed
    assert(mbp10->levels[6].price == 0 && mbp10->levels[6].quantity == 0);
    assert(mbp10->levels[13].price == 0 && mbp10->levels[13].quantity == 0);

    // --- MBP1: best bid / best ask
    MBP1* mbp1 = (MBP1*)bs_get_no_ref(sc->mbp1_bs, sc->last_mbp1);
    assert(mbp1->hi_bid.price == 102 && mbp1->hi_bid.quantity == 2);
    assert(mbp1->lo_ask.price == 105 && mbp1->lo_ask.quantity == 4);

    // --- lifting the best ask moves the top of book, and the views follow
    mbp_limit(sc, 1, 1, 105, 4); // buy 4 @ 105 fully consumes the 105 ask

    mbp_ensure(sc);
    mbp1 = (MBP1*)bs_get_no_ref(sc->mbp1_bs, sc->last_mbp1);
    assert(mbp1->lo_ask.price == 106 && mbp1->lo_ask.quantity == 6); // best ask rolled up
    assert(mbp1->hi_bid.price == 102 && mbp1->hi_bid.quantity == 2); // best bid unchanged

    MBP10* mbp10b = (MBP10*)bs_get_no_ref(sc->mbp10_bs, sc->last_mbp10);
    assert(mbp10b->levels[10].price == 106 && mbp10b->levels[10].quantity == 6);
    assert(mbp10b->levels[11].price == 107 && mbp10b->levels[11].quantity == 1);
    assert(mbp10b->levels[12].price == 0 && mbp10b->levels[12].quantity == 0); // one fewer ask

    printf("test_mbp passed\n");

    server_free(sc);
}

#endif
