#ifndef TEST_STOPS_H
#define TEST_STOPS_H

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

// stop orders: arrival shapes (stop-only, combined, canrep both ways), trigger fire with
// ns arrival ordering across recycled ids, cancels, and the close-time interactions

extern void server_order(ServerContext* sc, u32 exec_order_id);
extern void server_exec_to_sw(ServerContext* sc);

static u32 stops_send(ServerContext* sc, Order* o) {
    u32 id = fl_insert(sc->orders, o);
    server_order(sc, id);
    return id;
}

// plain limit order, no stop half
static u32 stops_limit(ServerContext* sc, u32 client, u8 is_buy, u16 price, u16 qty) {
    Order o = {0};
    o.client_id = client;
    o.price = price;
    o.quantity = qty;
    o.status = is_buy ? (1 << BUY_DIRECTION_BIT) : 0;
    return stops_send(sc, &o);
}

// stop-only order: params live in the second_* fields, trigger in stop_price
static u32 stops_stop(ServerContext* sc, u32 client, u8 is_buy, u16 trigger, u16 qty, u16 limit, u32 bits) {
    Order o = {0};
    o.client_id = client;
    o.status = (1 << HAS_STOP_BIT) | bits;
    o.stop_price = trigger;
    o.second_direction = is_buy;
    o.second_quantity = qty;
    o.second_price = limit;
    return stops_send(sc, &o);
}

static u32 stops_book_qty(ServerContext* sc, u16 price) {
    MBO* m = (MBO*)bs_get_no_ref(sc->mbo_bs, sc->last_mbo);
    for (u16 i = 0; i < m->level_count; i++)
        if (m->levels[i].price == price)
            return m->levels[i].quantity;
    return 0;
}

// mimic exec_end + exec_to_sw: drain the convert holder into the sw queue and run it,
// repeating for anything the conversions themselves mint
static void stops_pump(ServerContext* sc) {
    while (!cb_is_empty(sc->convert_holder)) {
        u32 sentinel = CONVERT_SENTINEL_VALUE;
        cb_queue(sc->convert_holder, &sentinel);
        server_exec_to_sw(sc);

        while (!cb_is_empty(sc->sw_queue)) {
            u32 id = *(u32*)cb_deque(sc->sw_queue);
            server_order(sc, id);
        }
    }
}

static void stops_warp(ServerContext* sc, u64 t_ns) {
    sc->sch->current_bucket = t_ns >> P_BITS;
    sc->sch->now = t_ns & P_MASK;
}

void test_stops() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 7);
    ClientSettings* c0 = sc->client_settings + 0;
    ClientSettings* c1 = sc->client_settings + 1;
    FL* orders = sc->orders;

    server_market_open(sc);

    // --- shape rejects: no quantity, no trigger, stop limit without a limit price
    u32 r = stops_stop(sc, 0, 0, 90, 0, 0, 0);
    assert((((Order*)fl_get(orders, r))->status >> REJECT_BIT) & 1);
    r = stops_stop(sc, 0, 0, 0, 5, 0, 0);
    assert((((Order*)fl_get(orders, r))->status >> REJECT_BIT) & 1);
    r = stops_stop(sc, 0, 0, 90, 5, 0, (1 << STOP_LIMIT_BIT));
    assert((((Order*)fl_get(orders, r))->status >> REJECT_BIT) & 1);

    // --- a buy stop-market arms, a print at its trigger fires it, and it eats the book
    stops_limit(sc, 0, 0, 106, 2);
    stops_limit(sc, 0, 0, 107, 5);
    u32 s2 = stops_stop(sc, 1, 1, 106, 5, 0, 0);
    assert(sc->buy_stops->current == 2); // armed
    assert(c1->reserved_cash == 0);      // and nothing reserved for it

    stops_limit(sc, 1, 1, 106, 2); // print 106 hits the trigger
    assert(!cb_is_empty(sc->convert_holder));
    stops_pump(sc);
    assert(((Order*)fl_get(orders, s2))->quantity == 0);
    assert(!((((Order*)fl_get(orders, s2))->status >> REJECT_BIT) & 1)); // filled, not killed
    assert(stops_book_qty(sc, 107) == 0);
    assert(pq_is_empty(sc->buy_stops));

    // --- arrival order must survive id recycling: free two slots so the later stop gets
    // the lower id, then check the earlier one still fires first (only food for one)
    stops_limit(sc, 0, 0, 110, 2);
    stops_limit(sc, 0, 0, 111, 5);

    u32 ta = stops_limit(sc, 0, 0, 100, 1);
    u32 tb = stops_limit(sc, 1, 1, 100, 1); // fills ta
    fl_release(orders, ta);
    fl_release(orders, tb); // freelist top: tb, then ta

    stops_warp(sc, 1 * S_TO_NS);
    u32 sa = stops_stop(sc, 1, 1, 110, 5, 0, 0); // first arrival
    stops_warp(sc, 2 * S_TO_NS);
    u32 sb = stops_stop(sc, 1, 1, 110, 5, 0, 0); // second arrival, lower id
    assert(sb < sa); // ids really are jumbled against arrival

    stops_limit(sc, 1, 1, 110, 2); // print 110, both fire
    stops_pump(sc);
    // sa got the 5 @ 111, sb found nothing and died as an unfilled ioc
    assert(((Order*)fl_get(orders, sa))->quantity == 0);
    assert(!((((Order*)fl_get(orders, sa))->status >> REJECT_BIT) & 1));
    assert((((Order*)fl_get(orders, sb))->status >> REJECT_BIT) & 1);

    // --- a sell stop-limit (max heap side) converts to a resting limit
    u32 s4 = stops_stop(sc, 0, 0, 95, 3, 94, (1 << STOP_LIMIT_BIT));
    stops_limit(sc, 1, 1, 95, 2);
    stops_limit(sc, 0, 0, 95, 2); // print 95 fires it
    stops_pump(sc);
    assert(stops_book_qty(sc, 94) == 3);
    assert(((Order*)fl_get(orders, s4))->quantity == 3);
    u32 res_before = c0->reserved_shares;

    // clear the leftover 94 ask so later prints don't trip on it
    Order cxl = {0};
    cxl.client_id = 0;
    cxl.status = (1 << CANCEL_BIT);
    cxl.other_id = s4;
    stops_send(sc, &cxl);
    assert(c0->reserved_shares == res_before - 3);
    assert(stops_book_qty(sc, 94) == 0);

    // --- cancelling an armed stop, then printing through its trigger, fires nothing
    u32 s5 = stops_stop(sc, 0, 0, 90, 4, 0, 0);
    Order cxl5 = {0};
    cxl5.client_id = 0;
    cxl5.status = (1 << CANCEL_BIT);
    cxl5.other_id = s5;
    stops_send(sc, &cxl5);
    assert(((Order*)fl_get(orders, s5))->quantity == 0);

    stops_limit(sc, 1, 1, 90, 1);
    stops_limit(sc, 0, 0, 90, 1); // print 90: the stale entry pops into the guards
    assert(cb_is_empty(sc->convert_holder)); // and nothing fired

    // --- canrep an armed stop into a plain resting limit (fidelity style, one message)
    u32 s6 = stops_stop(sc, 0, 0, 85, 2, 0, 0);
    Order rep6 = {0};
    rep6.client_id = 0;
    rep6.status = (1 << CAN_REP_BIT);
    rep6.other_id = s6;
    rep6.price = 112;
    rep6.quantity = 2;
    stops_send(sc, &rep6);
    assert(((Order*)fl_get(orders, s6))->quantity == 0);
    assert(stops_book_qty(sc, 112) == 2);

    // --- and the reverse: canrep a resting limit into a stop-only order
    u32 l7 = stops_limit(sc, 0, 0, 113, 4);
    u32 res7 = c0->reserved_shares;
    Order rep7 = {0};
    rep7.client_id = 0;
    rep7.status = (1 << CAN_REP_BIT) | (1 << HAS_STOP_BIT);
    rep7.other_id = l7;
    rep7.stop_price = 60;
    rep7.second_direction = 0;
    rep7.second_quantity = 4;
    stops_send(sc, &rep7);
    assert(((Order*)fl_get(orders, l7))->quantity == 0);
    assert(stops_book_qty(sc, 113) == 0);
    assert(c0->reserved_shares == res7 - 4); // the stop holds no reserve
    // the minted leg is armed on the sell side at 60. stale tombstoned entries (the
    // cancelled 90, the replaced 85) still sit above it in the heap, so scan for it
    u32 leg7 = 0;
    for (u32 i = 1; i < sc->sell_stops->current; i++)
        if ((sc->sell_stops->heap[i] >> 32) == 60)
            leg7 = (u32)(sc->sell_stops->heap[i] & MAX_U32);
    Order* leg7_o = (Order*)fl_get(orders, leg7);
    assert(((leg7_o->status >> HAS_STOP_BIT) & 1) && leg7_o->stop_price == 60 && leg7_o->quantity == 4);

    // --- combined order: a NOW ask plus a protective sell stop leg in one message
    stops_limit(sc, 0, 1, 82, 3); // food for the leg later
    u32 sells_before = sc->sell_stops->current;
    Order comb = {0};
    comb.client_id = 1;
    comb.status = (1 << HAS_STOP_BIT); // NOW half is a plain limit sell
    comb.price = 115;
    comb.quantity = 3;
    comb.stop_price = 83;
    comb.second_direction = 0;
    comb.second_quantity = 3;
    u32 c8 = stops_send(sc, &comb);
    assert(stops_book_qty(sc, 115) == 3); // NOW half rested
    assert(((Order*)fl_get(orders, c8))->quantity == 3);
    assert(sc->sell_stops->current == sells_before + 1); // leg armed

    stops_limit(sc, 0, 1, 83, 1);
    stops_limit(sc, 1, 0, 83, 1); // print 83 fires the leg (s7's 60 stays put)
    stops_pump(sc);
    assert(stops_book_qty(sc, 82) == 0); // leg sold 3 into the 82 bid
    assert(stops_book_qty(sc, 115) == 3); // NOW half untouched

    // --- a day stop that never fires dies armed at the close
    u32 s9 = stops_stop(sc, 0, 1, 130, 2, 0, (1 << DAY_BIT));
    server_market_close(sc);
    Order* s9_o = (Order*)fl_get(orders, s9);
    assert(s9_o->quantity == 0 && ((s9_o->status >> REJECT_BIT) & 1));

    // --- fired at the last second: the conversion is in flight at the close, so the day
    // sweep must leave it alone, and it bounces off the closed-market gate instead
    server_market_open(sc);
    stops_limit(sc, 0, 0, 109, 1);
    u32 s10 = stops_stop(sc, 1, 1, 109, 1, 0, (1 << DAY_BIT));
    stops_limit(sc, 1, 1, 109, 1); // print 109 fires it into the convert holder
    server_market_close(sc); // sweep runs while the conversion is still queued
    Order* s10_o = (Order*)fl_get(orders, s10);
    assert(s10_o->quantity == 1 && !((s10_o->status >> REJECT_BIT) & 1));

    stops_pump(sc); // now it reaches exec, and the market is closed
    s10_o = (Order*)fl_get(orders, s10);
    assert(s10_o->quantity == 0 && ((s10_o->status >> REJECT_BIT) & 1));

    // --- oco bracket: a resting limit sell (take profit) + a protective sell stop, one
    // message. the stop fires first, so the take profit gets pulled
    server_market_open(sc);
    stops_warp(sc, 3 * S_TO_NS);
    Order br = {0};
    br.client_id = 0;
    br.status = (1 << HAS_STOP_BIT) | (1 << OCO_BIT); // now half: limit sell take profit
    br.price = 140;
    br.quantity = 5;
    br.stop_price = 70; // protective sell stop below
    br.second_direction = 0;
    br.second_quantity = 5;
    u32 tp = stops_send(sc, &br);
    u32 sl = ((Order*)fl_get(orders, tp))->other_id;
    assert(stops_book_qty(sc, 140) == 5);                         // tp resting
    assert(((Order*)fl_get(orders, sl))->other_id == tp);         // mutual link
    assert(((((Order*)fl_get(orders, sl))->status >> OCO_BIT) & 1));

    // give client 0 shares to actually sell into the stop, then print through 70
    stops_limit(sc, 1, 1, 71, 5);
    stops_limit(sc, 0, 0, 71, 5); // fills the 71 bid, prints 71... still above 70
    stops_limit(sc, 1, 1, 70, 1);
    stops_limit(sc, 0, 0, 70, 1); // print 70 fires the sl
    stops_pump(sc);
    // sl fired and swept the tp: tp is gone from the book, both slots done
    assert(stops_book_qty(sc, 140) == 0);
    assert(((Order*)fl_get(orders, tp))->quantity == 0);

    // --- the other direction: the take profit fills, pulling the stop leg
    stops_warp(sc, 4 * S_TO_NS);
    Order br2 = {0};
    br2.client_id = 0;
    br2.status = (1 << HAS_STOP_BIT) | (1 << OCO_BIT);
    // priced under the leftover asks so a matching buy hits it, not the cruft
    br2.price = 100;
    br2.quantity = 4;
    br2.stop_price = 65;
    br2.second_direction = 0;
    br2.second_quantity = 4;
    u32 tp2 = stops_send(sc, &br2);
    u32 sl2 = ((Order*)fl_get(orders, tp2))->other_id;
    assert(stops_book_qty(sc, 100) == 4);

    // a buy lifts the take profit in full - its fill must pull the stop leg
    stops_limit(sc, 1, 1, 100, 4);
    stops_pump(sc); // the synthetic oco cancel rides the convert path
    assert(stops_book_qty(sc, 100) == 0);
    Order* sl2_o = (Order*)fl_get(orders, sl2);
    assert(sl2_o->quantity == 0); // stop leg pulled
    // and a later print through 65 fires nothing
    assert(cb_is_empty(sc->convert_holder));
    stops_limit(sc, 0, 0, 64, 1);
    stops_limit(sc, 1, 1, 64, 1);
    stops_pump(sc);

    printf("test_stops passed\n");

    // holder_free owns the allocations array, so no free(alloc) here
    server_free(sc);
}

#endif
