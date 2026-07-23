#ifndef TEST_WAKES_H
#define TEST_WAKES_H

#include <assert.h>
#include <stdlib.h>
#include "server.h"
#include "order.h"
#include "constants.h"
#include "fl.h"
#include "cb.h"
#include "pq.h"
#include "xpq.h"
#include "client_settings.h"
#include "ob.h"

// price wakes: registration shapes (bad price, conflicting bits), firing on the way up
// (above / min heap) and down (below / max heap), no early fire, and cancel by id

extern void server_order(ServerContext* sc, u32 exec_order_id);

static u32 wakes_send(ServerContext* sc, Order* o) {
    u32 id = fl_insert(sc->orders, o);
    server_order(sc, id);
    return id;
}

// plain limit, used to build the book and print through wake triggers
static u32 wakes_limit(ServerContext* sc, u32 client, u8 is_buy, u16 price, u16 qty) {
    Order o = {0};
    o.client_id = client;
    o.price = price;
    o.quantity = qty;
    o.status = is_buy ? (1 << BUY_DIRECTION_BIT) : 0;
    return wakes_send(sc, &o);
}

// an above-wake (buy direction) fires when the print rises to price, a below-wake when it
// falls to it. price is the trigger, reusing the order's price field
static u32 wakes_wake(ServerContext* sc, u32 client, u8 is_above, u16 price) {
    Order o = {0};
    o.client_id = client;
    o.price = price;
    o.status = (1 << WAKE_BIT) | (is_above ? (1 << BUY_DIRECTION_BIT) : 0);
    return wakes_send(sc, &o);
}

static void wakes_cancel(ServerContext* sc, u32 client, u32 target) {
    Order o = {0};
    o.client_id = client;
    o.status = (1 << CANCEL_BIT);
    o.other_id = target;
    wakes_send(sc, &o);
}

void test_wakes() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 9);
    FL* orders = sc->orders;

    server_market_open(sc);

    // --- registration rejects: a zero price, and any conflicting action bit
    u32 r = wakes_wake(sc, 0, 1, 0);
    assert((((Order*)fl_get(orders, r))->status >> REJECT_BIT) & 1);

    Order bad = {0};
    bad.client_id = 0;
    bad.price = 100;
    bad.status = (1 << WAKE_BIT) | (1 << HAS_STOP_BIT); // stop riding a wake is malformed
    r = wakes_send(sc, &bad);
    assert((((Order*)fl_get(orders, r))->status >> REJECT_BIT) & 1);

    // --- a buy wake arms on the above side, reserving nothing and never touching the book
    u32 w1 = wakes_wake(sc, 1, 1, 106);
    assert(sc->wake_above->current == 2);
    Order* w1_o = (Order*)fl_get(orders, w1);
    assert(((w1_o->status >> WAKE_BIT) & 1) && !((w1_o->status >> REJECT_BIT) & 1));

    // a print at the trigger fires it: the entry pops and the heap goes empty
    wakes_limit(sc, 0, 0, 106, 2);
    wakes_limit(sc, 1, 1, 106, 2); // trade prints 106
    assert(pq_is_empty(sc->wake_above));

    // --- a below wake fires on the way down (max heap side)
    u32 w2 = wakes_wake(sc, 0, 0, 90);
    assert(sc->wake_below->current == 2);
    wakes_limit(sc, 1, 1, 90, 2);
    wakes_limit(sc, 0, 0, 90, 2); // trade prints 90
    assert(xpq_is_empty(sc->wake_below));
    (void)w2;

    // --- a wake does not fire on a print short of its trigger
    u32 w3 = wakes_wake(sc, 1, 1, 120);
    wakes_limit(sc, 0, 0, 110, 1);
    wakes_limit(sc, 1, 1, 110, 1); // prints 110, below 120
    assert(sc->wake_above->current == 2); // still armed
    wakes_limit(sc, 0, 0, 120, 1);
    wakes_limit(sc, 1, 1, 120, 1); // prints 120, now it fires
    assert(pq_is_empty(sc->wake_above));
    (void)w3;

    // --- cancel by id before it fires: the tombstone kills its entry on the guard
    u32 w4 = wakes_wake(sc, 1, 1, 100);
    assert(sc->wake_above->current == 2);
    wakes_cancel(sc, 1, w4);
    Order* w4_o = (Order*)fl_get(orders, w4);
    assert((w4_o->status >> REJECT_BIT) & 1); // tombstoned

    wakes_limit(sc, 0, 0, 100, 1);
    wakes_limit(sc, 1, 1, 100, 1); // print 100: the stale entry pops into the guard
    assert(pq_is_empty(sc->wake_above)); // popped, and nothing fired

    printf("test_wakes passed\n");

    // holder_free owns the allocations array, so no free(alloc) here
    server_free(sc);
}

#endif
