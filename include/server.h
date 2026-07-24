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
// NOII imbalance feed. an orthogonal add-on: delivered like the other streams but never chosen
// as a base sub_tier - a client flags it separately and pays on top of its base subscription
static const u8 TIER_IMBALANCE = 9;
static const u8 TIER_COUNT = TIER_IMBALANCE + 1;
// no feed, no ws broadcasts, last trade price only. pays nothing
static const u8 TIER_FREE = 10;

// what working one leg costs, walking the same levels ob will so what we charge is what 
// actually fills. cost is the pool a cash account draws from - cash for a buy, shares for a
// sell. open_notional is the reg t charge: everything past the free close, priced at what it
// fills or rests at, so no total ever has to be divided back out. q_remain is what would rest 
typedef struct LegCost {
    u32 cost;
    u64 open_notional;
    u32 q_remain;
} LegCost;

// basically everything we were holding as locals in src/main
// bundled into a struct
// also takes it off the stack and onto the beautiful heap
// if it seems like a lot note that this is THE "global" object
// one line of the run log, deferred. LOG_ORDER carries everything the accepted-order line prints,
// including the account snapshot, because that is read at print time and the account moves on.
// status is the raw Order.status so the flag tests stay where they were rather than being
// re-encoded into bools. 64 bytes, which is one cache line per record
#define LOG_ORDER 0
#define LOG_TRADE 1

// where server_log_dump writes the raw records. `make logdump && ./logdump` turns it back into
// the text the inline printfs used to emit
#define LOG_BIN_PATH "tiltyard.bin"

typedef struct LogRec {
    u64 now_ns;
    i64 cash;
    i64 shares;
    u32 order_id;
    u32 client_id;
    u32 reserved_cash;
    u32 reserved_shares;
    u32 other_id;
    u32 status;
    u32 quantity_filled;   // trade only
    u16 price;
    u16 quantity;
    u16 stop_price;
    u16 second_quantity;
    u8  kind;
    u8  second_direction;
    u8  partial;           // trade only
    u8  taker_is_buy;      // trade only
} LogRec;

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

    // running closing-interest totals for the imbalance/NOII feed, maintained as orders park
    // and cancel, reset to 0 after each cross. the published imbalance is their signed difference
    u32 imbalance_buy;
    u32 imbalance_sell;
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

    // NOII publications, appended once a second during an accumulation window and broadcast to
    // add-on subscribers. append-only like the candles, so it reads back as a flat array
    CB* imbalances;

    // the run log, captured rather than printed. formatting an accepted order took 4-9 separate
    // printf calls in the hot path, and at ~26M records that was 12% of total runtime spent in
    // vfprintf/ultoa/write. one memcpy of a LogRec here instead, formatted once at the end.
    // orders and trades share the buffer so their relative order survives
    CB* log;

    // scratch: the client_ids that had will_notify set this cycle, so the reset at the end of
    // server_stream touches only them. clearing the flag used to mean walking every client on
    // every book change, which profiled at a fifth of total runtime for a handful of real
    // flags. queued once per schedule_response, drained and cleared once per stream
    CB* notified;

    // stream roster: client_ids grouped by sub_tier, built once and never mutated. tier_offset
    // is CSR - tier t owns stream_roster[tier_offset[t] .. tier_offset[t+1]). this is the full
    // membership list; broadcasts go through live_roster below, and this is what it is rebuilt
    // from, so it has to stay in ascending client_id order
    u32* stream_roster;
    u32* tier_offset;

    // the ws-connected subset of the above, packed, with live_offset as its CSR index. only ~80
    // of 1421 subscribers ever connect, so rebuilt on a ws toggle rather than re-derived on every
    // book change. filled by scanning stream_roster in order, so broadcast order is unchanged
    CB* live_roster;
    u32* live_offset;

    // TIER_COUNT entries mapping a tier to its data structure: a BS* for blob tiers (0-3),
    // a CB* for the trade/candle buffers (4-8). a broadcast response's u8 tier indexes this.
    void** tier_source;

    LegCost* other_leg_cost;
    LegCost* leg_cost;

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

// replay the deferred run log to stdout. call before server_free
void server_log_dump(ServerContext* sc);

void server_free(ServerContext* sc);

#endif

