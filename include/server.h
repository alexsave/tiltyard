#ifndef SERVER_H
#define SERVER_H

#include "types.h"
#include "constants.h"
#include "client_settings.h"
#include "bs.h"
#include "fl.h"
#include "cb.h"
#include "sch.h"
#include "holder.h"
#include "xpq.h"

// order ids live well below MAX_U32 (see MIN_RESERVED_PACKET), so no real slice collides
static const u32 CONVERT_SENTINEL_VALUE = MAX_U32;

// which data stream a roster range feeds. each range carries one of these, so a client that
// wants several streams is listed once per stream it subscribes to.
static const u8 TIER_MBO = 0;
static const u8 TIER_MBP = 1;
static const u8 TIER_MBP10 = 2;
static const u8 TIER_MBP1 = 3;
static const u8 TIER_TRADE = 4;
static const u8 TIER_CANDLE_SEC = 5;
static const u8 TIER_CANDLE_MIN = 6;
static const u8 TIER_CANDLE_HR = 7;
static const u8 TIER_CANDLE_DAY = 8;
static const u8 TIER_COUNT = TIER_CANDLE_DAY + 1;

// basically everything we were holding as locals in src/main
// bundled into a struct
// also takes it off the stack and onto the beautiful heap
// if it seems like a lot note that this is THE "global" object
typedef struct ServerContext {

    u32* client_allocations;

    u32 last_mbo;
    u16 mark; // last trade price, what margin marks LMV/SMV against
    // the venue's take: taker fees, borrow, data, less maker rebates. only ever grows -
    // the taker fee always covers the maker rebate, and borrow/data are pure inflows
    u64 exchange_cash;
    ClientSettings* client_settings;
    BS* mbo_bs;
    u8 executing;
    u8 is_open; // the market: orders off the sw queue are rejected while this is 0
    CB* sw_queue;
    CB* hw_queue;
    FL* orders;
    Holder* ho;
    CB* fills;
    SCH* sch;
    FL* responses;
    FL* icebergs;
    CB* convert_holder;

    // time in force: day orders queued as they rest, gtd orders in a date-keyed heap. at a
    // close the fired ones move into price_pq, which drains (sorted) into expire_cb so the
    // book can be pruned in one snapshot. expire_cb is always filled from empty, so its buffer
    // is read directly as a flat sorted array
    CB* day_orders;
    PQ* gtd;
    PQ* price_pq;
    CB* expire_cb;

    // stop books, keyed (price << 32 | id): buys fire when the print rises to them, so the
    // min heap surfaces the lowest trigger; sells fire on the way down, so a max heap.
    // within a price the heap gives id order (meaningless - ids recycle), so each fired
    // price group is re-sorted by arrival ns straight into convert_holder
    PQ* buy_stops;
    XPQ* sell_stops;

    // price wakes, same (price << 32 | id) key: an above-wake fires when the print rises to
    // it (min heap surfaces the lowest), a below-wake when it falls (max heap). no market
    // side effect, so no ns ordering - each fired entry just sends the client a snapshot
    PQ* wake_above;
    XPQ* wake_below;

    // call auction: while auctioning, orders park instead of matching. limits into the two
    // price min-heaps (the demand/supply curves), markets into their own arrival queues (base
    // demand/supply, filled first), everything into auction_arrivals for the remainder drain
    u8 auctioning;
    u8 auction_frozen; // late in the window: cancels rejected, adds still park
    PQ* auction_bids;
    PQ* auction_asks;
    CB* auction_market_bids;
    CB* auction_market_asks;
    CB* auction_arrivals;
    // scratch: the heaps pop into these ascending price-sorted arrays (u64 entries), reused
    // each auction for both the clearing walk and the fill
    CB* auction_bid_sorted;
    CB* auction_ask_sorted;

    u64* rand;

    // mirror of MBO
    u32 last_mbp;
    BS* mbp_bs;

    u32 last_mbp10;
    BS* mbp10_bs;

    u32 last_mbp1;
    BS* mbp1_bs;

    // trade tape and the candlestick series derived from it (see trade.c). candles are only
    // ever appended, never dequeued, so each buffer reads back as a flat time-ordered array.
    CB* trades;
    CB* candles_sec;
    CB* candles_min;
    CB* candles_hr;
    CB* candles_day;

    // stream roster: client_ids grouped by sub_tier, built once and never mutated (ws
    // connect/disconnect is read live at send time). tier_offset is CSR - tier t owns
    // stream_roster[tier_offset[t] .. tier_offset[t+1]).
    u32* stream_roster;
    u32* tier_offset;

    // TIER_COUNT entries mapping a tier to its data structure: a BS* for blob tiers (0-3),
    // a CB* for the trade/candle buffers (4-8). a broadcast response's u8 tier indexes this.
    void** tier_source;

} ServerContext;


// yeah you need these two to initialize the server, cuz really it initalizes everything
ServerContext* server_init(TypeMetadata* tm, u32 * client_allocations, u64 seed);

void server_arrival(ServerContext* sc, u32 order_id);
void server_market_open(ServerContext* sc);
void server_market_close(ServerContext* sc);
void server_auction_accumulate(ServerContext* sc);
void server_auction_freeze(ServerContext* sc);
void server_eod(ServerContext* sc);
void server_eom(ServerContext* sc);
void server_candle_close(ServerContext* sc);
void server_hw_to_sw(ServerContext* sc);
void server_exec_end(ServerContext* sc);
void server_exec_start(ServerContext* sc);
void server_exec_to_sw(ServerContext* sc);

void server_free(ServerContext* sc);

#endif

