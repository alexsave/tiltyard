#ifndef TEST_SESSIONS_H
#define TEST_SESSIONS_H

#include <assert.h>
#include <stdlib.h>
#include "server.h"
#include "order.h"
#include "constants.h"
#include "fl.h"
#include "cb.h"
#include "client_settings.h"
#include "ob.h"

// market sessions + tif, and the id-recycling guards: a freed day/gtd id picked up by a new
// order must not get that new order pulled at the close

extern void server_order(ServerContext* sc, u32 exec_order_id);

static u32 sess_send(ServerContext* sc, u32 client, u8 is_buy, u16 price, u16 qty, u32 bits, u32 second_id) {
    Order o = {0};
    o.client_id = client;
    o.price = price;
    o.quantity = qty;
    o.status = (is_buy ? (1 << BUY_DIRECTION_BIT) : 0) | bits;
    o.second_id = second_id;
    u32 id = fl_insert(sc->orders, &o);
    server_order(sc, id);
    return id;
}

static u32 sess_book_qty(ServerContext* sc, u16 price) {
    MBO* m = (MBO*)bs_get_no_ref(sc->mbo_bs, sc->last_mbo);
    for (u16 i = 0; i < m->level_count; i++)
        if (m->levels[i].price == price)
            return m->levels[i].quantity;
    return 0;
}

// jump the scheduler clock so a close lands on the day we want
static void sess_warp(ServerContext* sc, u64 t_ns) {
    sc->sch->current_bucket = t_ns >> P_BITS;
    sc->sch->now = t_ns & P_MASK;
}

void test_sessions() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 42);
    ClientSettings* c0 = sc->client_settings + 0;

    // closed until the bell - nothing rests, the order bounces
    u32 pre = sess_send(sc, 0, 0, 105, 10, (1 << DAY_BIT), 0);
    assert((((Order*)fl_get(sc->orders, pre))->status >> REJECT_BIT) & 1);
    assert(sess_book_qty(sc, 105) == 0);

    server_market_open(sc);

    // a day sell rests, fills entirely, and main frees it once the client reads the fill
    u32 a = sess_send(sc, 0, 0, 105, 10, (1 << DAY_BIT), 0);
    assert(c0->reserved_shares == 10);
    sess_send(sc, 1, 1, 105, 10, 0, 0);
    assert(c0->reserved_shares == 0);
    fl_release(sc->orders, a);

    // the freed id comes back as a plain gtc, plus a genuine day order as the control
    u32 re = sess_send(sc, 0, 0, 106, 7, 0, 0);
    assert(re == a);
    u32 d = sess_send(sc, 0, 0, 107, 3, (1 << DAY_BIT), 0);

    server_market_close(sc);

    // the stale day entry found a live order without the day bit - it must survive
    assert(((Order*)fl_get(sc->orders, re))->quantity == 7);
    assert(sess_book_qty(sc, 106) == 7);
    // while the real day order died with its reserve given back
    assert(((Order*)fl_get(sc->orders, d))->quantity == 0);
    assert(sess_book_qty(sc, 107) == 0);
    assert(c0->reserved_shares == 7);

    server_market_open(sc);

    // same dance for gtd: date-10 order fills, frees, and its id comes back dated 20.
    // priced under the 106 leftover so the cross really does hit it
    u32 b = sess_send(sc, 0, 0, 101, 5, (1 << GTD_BIT), 10);
    sess_send(sc, 1, 1, 101, 5, 0, 0);
    fl_release(sc->orders, b);
    u32 rb = sess_send(sc, 0, 0, 111, 5, (1 << GTD_BIT), 20);
    assert(rb == b);
    assert(c0->reserved_shares == 12);

    // close of day 10: the stale (10,id) entry fires but the live order is dated 20
    sess_warp(sc, 10 * DAY_TO_NS + 21 * H_TO_NS);
    server_market_close(sc);
    assert(((Order*)fl_get(sc->orders, rb))->quantity == 5);
    assert(sess_book_qty(sc, 111) == 5);
    assert(c0->reserved_shares == 12);

    // close of day 20: its own entry fires and now it goes
    server_market_open(sc);
    sess_warp(sc, 20 * DAY_TO_NS + 21 * H_TO_NS);
    server_market_close(sc);
    assert(((Order*)fl_get(sc->orders, rb))->quantity == 0);
    assert(sess_book_qty(sc, 111) == 0);
    assert(c0->reserved_shares == 7);

    printf("test_sessions passed\n");

    // holder_free owns the allocations array, so no free(alloc) here
    server_free(sc);
}

#endif
