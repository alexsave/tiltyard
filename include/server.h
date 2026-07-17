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

// basically everything we were holding as locals in src/main
// bundled into a struct
// also takes it off the stack and onto the beautiful heap
// if it seems like a lot note that this is THE "global" object
typedef struct ServerContext {

    u32* client_allocations;

    u32 last_mbo;
    u16 mark; // last trade price, what margin marks LMV/SMV against
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

    u64* rand;

    // mirror of MBO
    u32 last_mbp;
    BS* mbp_bs;

} ServerContext;


// yeah you need these two to initialize the server, cuz really it initalizes everything
ServerContext* server_init(TypeMetadata* tm, u32 * client_allocations, u64 seed);

void server_arrival(ServerContext* sc, u32 order_id);
void server_market_open(ServerContext* sc);
void server_market_close(ServerContext* sc);
void server_hw_to_sw(ServerContext* sc);
void server_exec_end(ServerContext* sc);
void server_exec_start(ServerContext* sc);
void server_exec_to_sw(ServerContext* sc);

void server_free(ServerContext* sc);

#endif

