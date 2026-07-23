#ifndef TEST_CANREP_RELEASE_H
#define TEST_CANREP_RELEASE_H

#include <assert.h>
#include <stdlib.h>
#include "server.h"
#include "order.h"
#include "constants.h"
#include "fl.h"
#include "client_settings.h"
#include "ob.h"

// the slot a cancel/cancel-replace retires. the client is only ever told about the new order,
// so main.c - which frees the slot its response is about - never hands the old one back

extern void server_order(ServerContext* sc, u32 exec_order_id);

static u32 cr_limit(ServerContext* sc, u32 client, u8 is_buy, u16 price, u16 qty) {
    Order o = {0};
    o.client_id = client;
    o.price = price;
    o.quantity = qty;
    o.status = is_buy ? (1 << BUY_DIRECTION_BIT) : 0;
    u32 id = fl_insert(sc->orders, &o);
    server_order(sc, id);
    return id;
}

static u32 cr_book_qty(ServerContext* sc, u16 price) {
    MBO* m = (MBO*)bs_get_no_ref(sc->mbo_bs, sc->last_mbo);
    for (u16 i = 0; i < m->level_count; i++)
        if (m->levels[i].price == price)
            return m->levels[i].quantity;
    return 0;
}

static u32 cr_probe(ServerContext* sc) {
    Order probe = {0};
    return fl_insert(sc->orders, &probe);
}

void test_canrep_releases_slot() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 43);
    ClientSettings* c0 = sc->client_settings + 0;

    server_market_open(sc);

    u32 a = cr_limit(sc, 0, 1, 100, 10);
    assert(cr_book_qty(sc, 100) == 10);
    assert(c0->reserved_cash == 1000);

    // replace it a dollar higher
    Order rep = {0};
    rep.client_id = 0;
    rep.price = 101;
    rep.quantity = 10;
    rep.status = (1 << BUY_DIRECTION_BIT) | (1 << CAN_REP_BIT);
    rep.other_id = a;
    u32 r = fl_insert(sc->orders, &rep);
    server_order(sc, r);

    assert(((Order*)fl_get(sc->orders, a))->quantity == 0);
    assert(cr_book_qty(sc, 100) == 0);
    assert(cr_book_qty(sc, 101) == 10);
    assert(c0->reserved_cash == 1010);

    // the replaced order is out of the book and nobody downstream owns its id, so this call
    // is the last reference to it. released, it is the top of the stack and comes straight back
    assert(cr_probe(sc) == a);

    printf("test_canrep_releases_slot passed\n");

    server_free(sc);
}

void test_pair_cancel_same_id_rejected() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 47);
    ClientSettings* c0 = sc->client_settings + 0;

    server_market_open(sc);

    u32 x = cr_limit(sc, 0, 1, 100, 10);
    assert(c0->reserved_cash == 1000);

    // a pure cancel pair naming the same resting order in both legs. each leg's precheck runs
    // before either leg wipes anything, so both would pass and both blocks would retire the
    // same order - releasing its id twice, and overrunning the reserved snapshot in ob_pair
    Order pc = {0};
    pc.client_id = 0;
    pc.status = (1 << BUY_DIRECTION_BIT) | (1 << CANCEL_BIT) | (1 << ASK_BID_PAIR_BIT);
    pc.other_id = x;
    pc.second_id = x;
    u32 c = fl_insert(sc->orders, &pc);
    server_order(sc, c);

    // rejected at the precheck, so the order it named twice is untouched
    assert((((Order*)fl_get(sc->orders, c))->status >> REJECT_BIT) & 1);
    assert(((Order*)fl_get(sc->orders, x))->quantity == 10);
    assert(cr_book_qty(sc, 100) == 10);
    assert(c0->reserved_cash == 1000);

    // and its id is not in the free stack at all, let alone in it twice - released twice it
    // would be handed to two live owners at once, which fl_release does not refuse
    u32 p1 = cr_probe(sc);
    u32 p2 = cr_probe(sc);
    assert(p1 != x && p2 != x && p1 != p2);

    printf("test_pair_cancel_same_id_no_double_free passed\n");

    server_free(sc);
}

// a pair leg that swallows a whole price level, where that level also holds the OTHER leg's
// cancel target. the level the taker only bites into has been safe since mbo_fill_remove
// learned about both cancel ids, but the fills for a level consumed whole are queued in
// ob_affected_range, which only ever skipped its own leg's target. the co-leg's order was
// filled AND retired as a cancel, so its id went back on the free stack twice: once
// server-side here, once by the client acting on the fill response. minted to two owners,
// it then rested twice in the book, and a later cancel spliced both - leaving the level
// declaring one more entry than was written, a ghost that the next crossing order filled
void test_pair_cross_spares_co_leg_cancel() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 53);
    ClientSettings* c0 = sc->client_settings + 0;
    ClientSettings* c1 = sc->client_settings + 1;

    server_market_open(sc);

    // our two resting quotes, plus a stranger sharing the ask level
    u32 old_bid = cr_limit(sc, 0, 1, 100, 10);
    u32 victim  = cr_limit(sc, 0, 0, 110, 10);
    u32 other   = cr_limit(sc, 1, 0, 110, 10);
    assert(cr_book_qty(sc, 110) == 20);

    // requote both sides at once: the bid steps up to 110 - onto the ask level our own ask
    // is resting on - and the ask steps up to 115. it asks for the whole level, which is what
    // puts it on the consume-the-level-whole path rather than the bite-into-it one, and only
    // the stranger's 10 are actually there to trade with: our own 10 are being pulled
    Order pr = {0};
    pr.client_id = 0;
    pr.price = 110;
    pr.quantity = 20;
    pr.other_id = old_bid;
    pr.second_price = 115;
    pr.second_quantity = 10;
    pr.second_id = victim;
    pr.status = (1 << BUY_DIRECTION_BIT) | (1 << ASK_BID_PAIR_BIT) | (1 << CAN_REP_BIT);
    u32 p = fl_insert(sc->orders, &pr);
    server_order(sc, p);

    // both old quotes are retired and the crossed level is gone
    assert(((Order*)fl_get(sc->orders, old_bid))->quantity == 0);
    assert(((Order*)fl_get(sc->orders, victim))->quantity == 0);
    assert(cr_book_qty(sc, 100) == 0);

    // the stranger sold us its 10 and nothing more
    assert(((Order*)fl_get(sc->orders, other))->quantity == 0);
    assert(c1->shares == 990);

    // we bought 10 and sold nothing. filling our own pulled ask would have put this back at
    // 1000 - and handed its id out twice on the way
    assert(c0->shares == 1010);

    // so only half the bid traded, and the other half rests where the level used to be
    assert(cr_book_qty(sc, 110) == 10);
    assert(cr_book_qty(sc, 115) == 10);
    assert(c0->reserved_cash == 1100);  // 10 resting at 110
    assert(c0->reserved_shares == 10);  // the new ask at 115

    printf("test_pair_cross_spares_co_leg_cancel passed\n");

    server_free(sc);
}

#endif
