#ifndef TEST_AUCTION_H
#define TEST_AUCTION_H

#include <assert.h>
#include <stdlib.h>
#include "server.h"
#include "order.h"
#include "constants.h"
#include "fl.h"
#include "cb.h"
#include "client_settings.h"
#include "ob.h"
#include "trade.h"

// call auction cross: single clearing price, market-first fill, imbalance remainder, reserves

extern void server_order(ServerContext* sc, u32 exec_order_id);
extern void server_auction(ServerContext* sc);
extern void server_exec_to_sw(ServerContext* sc);
extern void auction_walk(ServerContext* sc, u8 release, u16* clearing_out, u32* matched_out);

// drain the residual server_auction pushed to convert_holder onto the sw queue and run it
static void auc_pump(ServerContext* sc) {
    server_exec_to_sw(sc);
    while (!cb_is_empty(sc->sw_queue)) {
        u32 id = *(u32*)cb_deque(sc->sw_queue);
        server_order(sc, id);
    }
}

static u32 auc_book_qty(ServerContext* sc, u16 price) {
    MBO* m = (MBO*)bs_get_no_ref(sc->mbo_bs, sc->last_mbo);
    for (u16 i = 0; i < m->level_count; i++)
        if (m->levels[i].price == price)
            return m->levels[i].quantity;
    return 0;
}

static u32 auc_send(ServerContext* sc, Order* o) {
    u32 id = fl_insert(sc->orders, o);
    server_order(sc, id);
    return id;
}

static u32 auc_limit(ServerContext* sc, u32 client, u8 is_buy, u16 price, u16 qty) {
    Order o = {0};
    o.client_id = client;
    o.price = price;
    o.quantity = qty;
    o.status = is_buy ? (1 << BUY_DIRECTION_BIT) : 0;
    return auc_send(sc, &o);
}

static u32 auc_market(ServerContext* sc, u32 client, u8 is_buy, u16 qty) {
    Order o = {0};
    o.client_id = client;
    o.quantity = qty;
    o.status = (1 << IS_MARKET_BIT) | (is_buy ? (1 << BUY_DIRECTION_BIT) : 0);
    return auc_send(sc, &o);
}

void test_auction() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 11);
    ClientSettings* c0 = sc->client_settings + 0;
    ClientSettings* c1 = sc->client_settings + 1;
    FL* orders = sc->orders;

    sc->mark = 100;      // reference price
    sc->auctioning = 1;  // enter the auction phase - adds park instead of matching

    // --- symmetric cross: bids 101/100, asks 99/100. volume peaks at 100 (10 vs 10)
    i64 c0_cash = c0->cash, c0_sh = c0->shares;
    i64 c1_cash = c1->cash, c1_sh = c1->shares;

    u32 b1 = auc_limit(sc, 1, 1, 101, 5);
    u32 b2 = auc_limit(sc, 1, 1, 100, 5);
    u32 a1 = auc_limit(sc, 0, 0, 99, 5);
    u32 a2 = auc_limit(sc, 0, 0, 100, 5);
    assert(c1->reserved_cash == 5 * 101 + 5 * 100); // parked bids hold cash
    assert(c0->reserved_shares == 10);              // parked asks hold shares

    server_auction(sc);

    // everything crosses at 100: all four fully fill
    assert(((Order*)fl_get(orders, b1))->quantity == 0);
    assert(((Order*)fl_get(orders, b2))->quantity == 0);
    assert(((Order*)fl_get(orders, a1))->quantity == 0);
    assert(((Order*)fl_get(orders, a2))->quantity == 0);

    // 10 shares @ 100 each way, reserves handed back
    assert(c1->cash == c1_cash - 1000 && c1->shares == c1_sh + 10);
    assert(c0->cash == c0_cash + 1000 && c0->shares == c0_sh - 10);
    assert(c1->reserved_cash == 0 && c0->reserved_shares == 0);

    // --- market buy fills first and drags the clearing price up to consume every ask. a
    // market buy of 10 vs asks 6@100 + 4@105 clears at 105 (volume peaks there at 10)
    c0_cash = c0->cash; c0_sh = c0->shares;
    c1_cash = c1->cash; c1_sh = c1->shares;

    u32 mb = auc_market(sc, 1, 1, 10);
    auc_limit(sc, 0, 0, 100, 6);
    auc_limit(sc, 0, 0, 105, 4);
    assert(c1->reserved_cash == 10 * 100); // market buy holds cash at the mark

    server_auction(sc);

    assert(((Order*)fl_get(orders, mb))->quantity == 0); // market buy fully filled
    assert(c1->cash == c1_cash - 10 * 105 && c1->shares == c1_sh + 10);
    assert(c0->cash == c0_cash + 10 * 105 && c0->shares == c0_sh - 10);
    assert(c1->reserved_cash == 0 && c0->reserved_shares == 0);

    // --- imbalance: an auction-only bid of 10 vs an ask of 6 clears 6 at 100; the bid's
    // unfilled 4 cancels, since auction-only has no continuous life to fall back to
    c0_cash = c0->cash; c0_sh = c0->shares;
    c1_cash = c1->cash; c1_sh = c1->shares;

    Order ao = {0};
    ao.client_id = 1;
    ao.price = 100;
    ao.quantity = 10;
    ao.status = (1 << BUY_DIRECTION_BIT) | (1 << AUCTION_ONLY_BIT);
    u32 aob = auc_send(sc, &ao);
    auc_limit(sc, 0, 0, 100, 6);

    server_auction(sc);

    Order* aob_o = (Order*)fl_get(orders, aob);
    assert(aob_o->quantity == 0 && ((aob_o->status >> REJECT_BIT) & 1)); // remainder cancelled
    assert(c1->cash == c1_cash - 600 && c1->shares == c1_sh + 6);
    assert(c0->cash == c0_cash + 600 && c0->shares == c0_sh - 6);
    assert(c1->reserved_cash == 0 && c0->reserved_shares == 0);

    // --- market vs market, no limits: nothing crosses on the curve, so it clears at the
    // reference price (mark 100), min(5, 5) = 5 shares
    c0_cash = c0->cash; c0_sh = c0->shares;
    c1_cash = c1->cash; c1_sh = c1->shares;

    u32 fb_b = auc_market(sc, 1, 1, 5);
    u32 fb_a = auc_market(sc, 0, 0, 5);
    server_auction(sc);
    assert(((Order*)fl_get(orders, fb_b))->quantity == 0);
    assert(((Order*)fl_get(orders, fb_a))->quantity == 0);
    assert(c1->cash == c1_cash - 500 && c1->shares == c1_sh + 5);
    assert(c0->cash == c0_cash + 500 && c0->shares == c0_sh - 5);

    // --- a bid worth more than buying power is rejected at park, nothing reserved
    u32 broke = auc_limit(sc, 1, 1, 200, 65000); // 13M > 10M cash
    assert((((Order*)fl_get(orders, broke))->status >> REJECT_BIT) & 1);
    assert(c1->reserved_cash == 0);

    // --- plain residual releases to continuous: a bid of 10 vs an ask of 6 clears 6, and the
    // unfilled 4 rests in the book once the auction hands off (auctioning off, market open)
    c1_cash = c1->cash;
    u32 rb = auc_limit(sc, 1, 1, 100, 10);
    auc_limit(sc, 0, 0, 100, 6);
    server_auction(sc);

    sc->auctioning = 0; // auction over, continuous session begins
    sc->is_open = 1;
    auc_pump(sc);

    assert(((Order*)fl_get(orders, rb))->quantity == 4); // remainder rests
    assert(auc_book_qty(sc, 100) == 4);
    assert(c1->reserved_cash == 4 * 100);            // re-reserved on rest
    assert(c1->cash == c1_cash - 600);               // paid for the 6 that crossed

    printf("test_auction passed\n");

    server_free(sc);
}

// resting book crossed by the auction: overnight news floods one side, lifting/hitting the book.
// the key check is conservation - a cross only moves cash and shares between clients, never mints
void test_auction_book() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 13);
    ClientSettings* c0 = sc->client_settings + 0;
    ClientSettings* c1 = sc->client_settings + 1;

    server_market_open(sc); // continuous: is_open, not auctioning

    // --- good news: the book has resting asks, then a big buy floods the auction and lifts them
    auc_limit(sc, 0, 0, 105, 5); // client 0 rests two ask levels
    auc_limit(sc, 0, 0, 106, 5);
    assert(auc_book_qty(sc, 105) == 5 && auc_book_qty(sc, 106) == 5);

    i64 cash0 = c0->cash, sh0 = c0->shares, cash1 = c1->cash, sh1 = c1->shares;
    i64 total_cash = c0->cash + c1->cash;
    i64 total_shares = c0->shares + c1->shares;

    sc->auctioning = 1;
    // MOC-style: with the book live during the closing window, only auction-only orders park
    Order bid = {0};
    bid.client_id = 1;
    bid.price = 110;
    bid.quantity = 8;
    bid.status = (1 << BUY_DIRECTION_BIT) | (1 << AUCTION_ONLY_BIT);
    auc_send(sc, &bid);
    server_auction(sc); // clears at 106, matched 8: 105 fully eaten, 106 reduced to 2

    assert(auc_book_qty(sc, 105) == 0 && auc_book_qty(sc, 106) == 2);
    assert(c0->cash + c1->cash == total_cash);       // nothing minted
    assert(c0->shares + c1->shares == total_shares);
    assert(c0->cash == cash0 + 8 * 106 && c0->shares == sh0 - 8); // book seller
    assert(c1->cash == cash1 - 8 * 106 && c1->shares == sh1 + 8); // auction buyer

    // --- bad news: the book has resting bids, a big sell floods and hits them
    sc->auctioning = 0;
    auc_limit(sc, 1, 1, 95, 5); // client 1 rests two bid levels
    auc_limit(sc, 1, 1, 94, 5);
    assert(auc_book_qty(sc, 95) == 5 && auc_book_qty(sc, 94) == 5);

    cash0 = c0->cash; sh0 = c0->shares; cash1 = c1->cash; sh1 = c1->shares;
    total_cash = c0->cash + c1->cash;
    total_shares = c0->shares + c1->shares;

    sc->auctioning = 1;
    Order ask = {0};
    ask.client_id = 0;
    ask.price = 90;
    ask.quantity = 8;
    ask.status = (1 << AUCTION_ONLY_BIT); // parks against the live book during the window
    auc_send(sc, &ask);
    server_auction(sc); // clears at 90, matched 8: 95 fully eaten, 94 reduced to 2

    assert(auc_book_qty(sc, 95) == 0 && auc_book_qty(sc, 94) == 2);
    assert(c0->cash + c1->cash == total_cash);
    assert(c0->shares + c1->shares == total_shares);
    assert(c1->cash == cash1 - 8 * 90 && c1->shares == sh1 + 8); // book buyer
    assert(c0->cash == cash0 + 8 * 90 && c0->shares == sh0 - 8); // auction seller

    printf("test_auction_book passed\n");

    server_free(sc);
}

// same price, heavy side, only some fill: the fill must go by ARRIVAL, not by (recycled) id.
// recycle ids so the earlier arrival gets the higher id - if the cross sorted by id it'd fill
// the wrong one
void test_auction_priority() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 17);
    FL* orders = sc->orders;
    sc->mark = 100;
    sc->auctioning = 1;

    // two throwaway slots, freed so the next two inserts reuse them lifo (high id first)
    Order dummy = {0};
    u32 d0 = fl_insert(orders, &dummy);
    u32 d1 = fl_insert(orders, &dummy);
    fl_release(orders, d0);
    fl_release(orders, d1);

    // --- ask-heavy: asks 5+5 @ 100, bid 5 @ 100. matched 5 clears at 100; the earlier ask wins
    u32 ask_a = auc_limit(sc, 0, 0, 100, 5); // first arrival, higher id (d1)
    u32 ask_b = auc_limit(sc, 0, 0, 100, 5); // second arrival, lower id (d0)
    assert(ask_b < ask_a);                   // ids really are jumbled against arrival
    auc_limit(sc, 1, 1, 100, 5);
    server_auction(sc);
    assert(((Order*)fl_get(orders, ask_a))->quantity == 0); // earliest filled
    assert(((Order*)fl_get(orders, ask_b))->quantity == 5); // later did not

    // --- bid-heavy: mirror on the bid side
    u32 e0 = fl_insert(orders, &dummy);
    u32 e1 = fl_insert(orders, &dummy);
    fl_release(orders, e0);
    fl_release(orders, e1);
    u32 bid_a = auc_limit(sc, 1, 1, 100, 5); // first arrival, higher id
    u32 bid_b = auc_limit(sc, 1, 1, 100, 5); // second arrival, lower id
    assert(bid_b < bid_a);
    auc_limit(sc, 0, 0, 100, 5);
    server_auction(sc);
    assert(((Order*)fl_get(orders, bid_a))->quantity == 0); // earliest filled
    assert(((Order*)fl_get(orders, bid_b))->quantity == 5); // later did not

    printf("test_auction_priority passed\n");

    server_free(sc);
}

// the closing-window dual book: a regular order trades on the live book, an auction-only order
// parks for the cross instead
void test_auction_dualbook() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 17);

    server_market_open(sc); // is_open, not auctioning
    sc->auctioning = 1;     // closing window: book stays live, only auction-only parks

    // a regular limit trades continuously - it rests on the book, does not join the auction
    auc_limit(sc, 0, 0, 105, 5);
    assert(auc_book_qty(sc, 105) == 5);
    assert(sc->imbalance_sell == 0);

    // an auction-only limit parks for the cross and does not touch the book
    Order moc = {0};
    moc.client_id = 1;
    moc.price = 105;
    moc.quantity = 4;
    moc.status = (1 << BUY_DIRECTION_BIT) | (1 << AUCTION_ONLY_BIT);
    auc_send(sc, &moc);
    assert(sc->imbalance_buy == 4);
    assert(auc_book_qty(sc, 105) == 5);

    printf("test_auction_dualbook passed\n");

    server_free(sc);
}

// hold-all-day plus the offset-only cutoff: an auction-only order parks whenever entered, and
// once the closing window has an imbalance a new same-side add is refused
void test_auction_offset() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 23);

    server_market_open(sc); // is_open, not auctioning yet

    // hold-all-day: a MOC parks mid-day, before any window has opened
    Order moc = {0};
    moc.client_id = 1;
    moc.price = 100;
    moc.quantity = 10;
    moc.status = (1 << BUY_DIRECTION_BIT) | (1 << AUCTION_ONLY_BIT);
    auc_send(sc, &moc);
    assert(sc->imbalance_buy == 10);

    // the closing window opens with a standing buy imbalance
    sc->auctioning = 1;

    // a same-side (buy) add would grow the imbalance, so it is refused offset-only
    Order more = {0};
    more.client_id = 1;
    more.price = 100;
    more.quantity = 5;
    more.status = (1 << BUY_DIRECTION_BIT) | (1 << AUCTION_ONLY_BIT);
    u32 rej = auc_send(sc, &more);
    assert(sc->imbalance_buy == 10);
    assert((((Order*)fl_get(sc->orders, rej))->status >> REJECT_BIT) & 1);

    // a contra (sell) add relieves the imbalance, so it is accepted
    Order off = {0};
    off.client_id = 0;
    off.price = 100;
    off.quantity = 4;
    off.status = (1 << AUCTION_ONLY_BIT);
    auc_send(sc, &off);
    assert(sc->imbalance_sell == 4);

    printf("test_auction_offset passed\n");

    server_free(sc);
}

// the ask-heavy mirror, plus the balanced-book edge: with the book balanced the first add is
// accepted on either side, and once sell-heavy a same-side sell is refused while a buy passes
void test_auction_offset_sell() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 29);

    server_market_open(sc);
    sc->auctioning = 1; // closing window, book still balanced (0 == 0)

    // balanced-book edge: the offset guard is skipped, so this sell is accepted
    Order s = {0};
    s.client_id = 0;
    s.price = 100;
    s.quantity = 10;
    s.status = (1 << AUCTION_ONLY_BIT);
    auc_send(sc, &s);
    assert(sc->imbalance_sell == 10);

    // now sell-heavy: another sell would grow the imbalance, so it is refused
    Order s2 = {0};
    s2.client_id = 0;
    s2.price = 100;
    s2.quantity = 3;
    s2.status = (1 << AUCTION_ONLY_BIT);
    u32 rej = auc_send(sc, &s2);
    assert(sc->imbalance_sell == 10);
    assert((((Order*)fl_get(sc->orders, rej))->status >> REJECT_BIT) & 1);

    // a contra buy relieves the imbalance and is accepted
    Order b = {0};
    b.client_id = 1;
    b.price = 100;
    b.quantity = 4;
    b.status = (1 << BUY_DIRECTION_BIT) | (1 << AUCTION_ONLY_BIT);
    auc_send(sc, &b);
    assert(sc->imbalance_buy == 4);

    printf("test_auction_offset_sell passed\n");

    server_free(sc);
}

// an auction-only cancel routes to the auction handler, tombstones the parked order, releases its
// reserve, and backs the quantity out of the running imbalance
void test_auction_cancel() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 31);
    ClientSettings* c1 = sc->client_settings + 1;

    server_market_open(sc);

    Order moc = {0};
    moc.client_id = 1;
    moc.price = 100;
    moc.quantity = 8;
    moc.status = (1 << BUY_DIRECTION_BIT) | (1 << AUCTION_ONLY_BIT);
    u32 mid = auc_send(sc, &moc);
    assert(sc->imbalance_buy == 8);
    assert(c1->reserved_cash == 8 * 100);

    // the cancel carries the auction-only bit so the gate routes it to the auction, not the book
    Order cxl = {0};
    cxl.client_id = 1;
    cxl.other_id = mid;
    cxl.status = (1 << CANCEL_BIT) | (1 << AUCTION_ONLY_BIT);
    auc_send(sc, &cxl);
    assert(sc->imbalance_buy == 0);
    assert(((Order*)fl_get(sc->orders, mid))->quantity == 0); // tombstoned
    assert(c1->reserved_cash == 0);                            // reserve released

    printf("test_auction_cancel passed\n");

    server_free(sc);
}

// the timing split end to end: pre-open interest crosses at the open and clears, then mid-day
// MOCs accumulate and cross at the close
void test_auction_cross_timing() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 37);
    ClientSettings* c0 = sc->client_settings + 0;
    ClientSettings* c1 = sc->client_settings + 1;

    // pre-open window: regular orders park (market closed), consumed by the opening cross
    sc->auctioning = 1;
    Order pb = {0};
    pb.client_id = 1;
    pb.price = 100;
    pb.quantity = 5;
    pb.status = (1 << BUY_DIRECTION_BIT);
    auc_send(sc, &pb);
    Order ps = {0};
    ps.client_id = 0;
    ps.price = 100;
    ps.quantity = 5;
    auc_send(sc, &ps);
    assert(sc->imbalance_buy == 5 && sc->imbalance_sell == 5);

    i64 c1sh = c1->shares, c0sh = c0->shares;
    server_auction(sc); // opening cross at 100, matched 5
    assert(c1->shares == c1sh + 5 && c0->shares == c0sh - 5);
    assert(sc->imbalance_buy == 0 && sc->imbalance_sell == 0); // cleared, ready for the close

    // market open, continuous. mid-day MOCs are held for the closing cross, not the (past) open
    sc->is_open = 1;
    sc->auctioning = 0;
    Order mb = {0};
    mb.client_id = 1;
    mb.price = 100;
    mb.quantity = 3;
    mb.status = (1 << BUY_DIRECTION_BIT) | (1 << AUCTION_ONLY_BIT);
    auc_send(sc, &mb);
    Order ms = {0};
    ms.client_id = 0;
    ms.price = 100;
    ms.quantity = 3;
    ms.status = (1 << AUCTION_ONLY_BIT);
    auc_send(sc, &ms);
    assert(sc->imbalance_buy == 3 && sc->imbalance_sell == 3);

    c1sh = c1->shares;
    c0sh = c0->shares;
    sc->auctioning = 1; // closing window
    server_auction(sc); // closing cross at 100, matched 3
    assert(c1->shares == c1sh + 3 && c0->shares == c0sh - 3);
    assert(sc->imbalance_buy == 0 && sc->imbalance_sell == 0);

    printf("test_auction_cross_timing passed\n");

    server_free(sc);
}

// the NOII indicative clearing price (read-only auction_walk, release=0) matches the price the
// real cross settles at, and the read-only pass leaves reserves and orders untouched
void test_auction_indicative() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 41);
    ClientSettings* c0 = sc->client_settings + 0;
    ClientSettings* c1 = sc->client_settings + 1;

    sc->mark = 100;
    sc->auctioning = 1; // pre-open window (is_open 0), so regular orders park

    auc_limit(sc, 1, 1, 102, 5); // buy 5 @ 102
    auc_limit(sc, 0, 0, 98, 5);  // sell 5 @ 98

    // read-only indicative: the same walk the cross uses, reserves untouched
    u16 clearing;
    u32 matched;
    auction_walk(sc, 0, &clearing, &matched);
    cb_clear(sc->auction_market_bids);
    cb_clear(sc->auction_market_asks);
    cb_clear(sc->auction_bid_sorted);
    cb_clear(sc->auction_ask_sorted);
    assert(matched == 5);
    assert(c1->reserved_cash == 5 * 102); // release=0 left the park reserve in place

    // the real cross settles at exactly the indicative price
    i64 c1cash = c1->cash, c0cash = c0->cash;
    server_auction(sc);
    assert(c1->cash == c1cash - 5 * clearing);
    assert(c0->cash == c0cash + 5 * clearing);
    assert(c1->reserved_cash == 0); // released and settled by the cross

    printf("test_auction_indicative passed\n");

    server_free(sc);
}

// the cross is a real print: a matched cross moves the mark to the clearing price and appends the
// matched volume to the tape; an empty cross does neither. and the NOII indicative reads that mark
void test_auction_mark() {
    TypeMetadata* tm = get_types();
    u32* alloc = malloc(tm->IMPLS_COUNT * sizeof(u32));
    alloc[tm->cz_index] = 1;
    alloc[tm->co_index] = 1;
    ServerContext* sc = server_init(tm, alloc, 43);

    sc->mark = 50;      // a stale reference before any cross
    sc->auctioning = 1; // pre-open window, orders park

    u32 trades_before = cb_count(sc->trades);

    // a real cross prints: the mark moves to the clearing price and the tape gets the trade
    auc_limit(sc, 1, 1, 102, 5); // buy 5 @ 102
    auc_limit(sc, 0, 0, 98, 5);  // sell 5 @ 98
    server_auction(sc);          // clears at 98, matched 5
    assert(sc->mark == 98);
    assert(cb_count(sc->trades) == trades_before + 1);
    Trade* t = (Trade*)cb_last(sc->trades);
    assert(t->quantity == 5 && t->price == 98);

    // with nothing parked the NOII indicative now reads that mark, not zero
    u16 clearing;
    u32 matched;
    auction_walk(sc, 0, &clearing, &matched);
    cb_clear(sc->auction_market_bids);
    cb_clear(sc->auction_market_asks);
    cb_clear(sc->auction_bid_sorted);
    cb_clear(sc->auction_ask_sorted);
    assert(matched == 0 && clearing == 98);

    // an empty cross (nothing matched) must not print or move the mark
    u32 trades_after = cb_count(sc->trades);
    auc_limit(sc, 1, 1, 100, 3); // a lone buy, no contra
    server_auction(sc);          // matched 0
    assert(sc->mark == 98);                       // unchanged
    assert(cb_count(sc->trades) == trades_after); // no new print

    printf("test_auction_mark passed\n");

    server_free(sc);
}

#endif
