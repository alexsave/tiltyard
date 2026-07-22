#ifndef TIER03_SLICERS_H
#define TIER03_SLICERS_H

#include "types.h"
#include "client_settings.h"
#include "order.h"
#include "context.h"

// T3 - Slicers. execution algos: take one big parent order and work it into the book as a
// stream of small children, tracking a schedule. see TILTYARD_AGENTS.md T3.
//
// this is the tier that produces ORDER-FLOW AUTOCORRELATION - a parent worked over hours
// is a persistent stream of same-side children, which is exactly the stylized fact a
// population of independent draws cannot produce. it is also the channel through which
// T4 Suit and T13 Glacier flow will reach the book.
//
// every behavioural knob lives in T3Params so a sweep harness can vary it without
// touching this file. ALL DEFAULTS ARE /* UNCALIBRATED */ PLACEHOLDERS.

typedef enum {
    T3_TWAP,  // even across the horizon. schedule is pure elapsed time
    T3_IS,    // implementation shortfall: front-loaded, pay spread early to cut drift risk
    T3_VWAP,  // GATED - needs realized market volume, see t3_algo_supported()
    T3_POV    // GATED - same
} t3_algo_t;

// what we last sent and have not heard back about. a response is keyed by the MESSAGE id,
// never by the order it acts on, so a cancel ack matches nothing we hold unless we
// remember what we sent
#define T3_PEND_NONE    0
#define T3_PEND_WS      1
#define T3_PEND_PASSIVE 2  // posting or replacing the resting child
#define T3_PEND_CROSS   3  // marketable catch-up, never rests
#define T3_PEND_CANCEL  4

typedef struct T3Params {
    t3_algo_t algo_type;

    // the parent: what the client actually wants done. quantity is ours to track in u32 -
    // an Order's quantity is u16, which is a big part of WHY a parent must be sliced
    u32 parent_qty;
    u64 horizon_ns;         // how long we have to finish

    // children
    u16 child_size;         // round lots, the base slice
    u64 child_interval_ns;  // WAKE_INTERVAL: how often we look at the schedule

    // never take more than this share of the liquidity showing at the touch. a real cap is
    // a share of market VOLUME - see t3_algo_supported() for why this is depth instead
    u16 participation_cap_pct;

    // how far behind schedule (in shares) before we stop posting and start crossing. this
    // is the implementation-shortfall tradeoff in one number
    u32 catch_up_threshold;

    // the price band around the parent's arrival price, outside which we will not trade at
    // all. every real parent carries a client limit, and this is it.
    //
    // without a band a behind-schedule algo pays literally anything to catch up: it crosses,
    // moves the price against itself, falls further behind because the schedule does not
    // care, and crosses again. forty of them doing that walks the book to the floor in a
    // single session. an algo that cannot fill inside its band is SUPPOSED to under-fill and
    // hand the shortfall back - that is what implementation shortfall measures
    u16 max_slippage_ticks;

    // IS front-loading: what share of the parent the curve wants done at the halfway mark.
    // 50 is TWAP, higher is more urgent early
    u16 aggression_half_pct;

    // how far off the touch a passive child sits. 0 joins the touch
    u16 passive_offset_ticks;
    // the resting child is this far from the touch -> repost to follow the market
    u16 passive_chase_ticks;

    // THE FAT-FINGER FLAG. set, and the whole parent goes as one marketable order with no
    // slicing at all - equivalently participation_cap = 1.0 and horizon_ns = 0. wired here
    // as a config flag rather than a separate agent type, per the design doc
    u8 slice_disabled;

    // a slicer with no parent is inert. in the design doc the parents come from clients -
    // T4 Suits and T13 Glaciers route their flow THROUGH a slicer. but routing an order from
    // one client to another needs the T11 routing layer, which may never be built - there is
    // no mechanism today for a client to hand work to another client. so auto_parent is not
    // a temporary stand-in: absent routing it is the model, and this tier is a self-directed
    // systematic execution desk that works its own book rather than an agent for someone else
    u8 auto_parent;
    u64 restart_delay_ns;

    // keep the desk a conduit, not a whale. the doc frames a slicer as an AGENT whose
    // capital is the client's - a finished parent is delivered and the desk left flat. with
    // no routing there is no client to deliver to, so the position stays on the slicer and
    // self-generated parents would compound without limit (early runs hit +-46k on a 4k
    // parent). leaning each new parent's side against whatever we already carry past this
    // size caps the book and is a fair model of a real desk anyway - a house that is long
    // gets given sells to work. the arresting tiers still move price; this just stops the
    // execution tier from becoming an unbounded directional trader it was never meant to be
    u32 inventory_balance_qty;

    // nothing has printed and we have not seen a book yet, so the first child has to be
    // posted somewhere. same role as a maker's seed price
    u16 seed_price;

    // no book / market shut / nothing to do
    u64 idle_wake_ns;
    u64 retry_wake_ns;
    u64 reject_backoff_ns;

    // account / connection shape
    i64 cash;
    u8  margin_mult;
    u8  maint_pct;
    u64 processing_time;
    u64 net_latency;
    u64 initial_wake;
} T3Params;

typedef struct T3 {
    T3Params p;

    // the live parent
    u8  have_parent;
    u8  parent_buy;
    u32 parent_qty;
    u32 parent_filled;
    u64 parent_start_ns;
    u16 arrival_price;   // where the market was when we took the parent on

    // the one child we allow to rest at a time
    u32 passive_id;
    u16 passive_price;
    u16 passive_qty;

    // in-flight message
    u8  pending;
    u8  pending_kind;
    u32 pending_id;
    u16 pending_qty;

    i64 cash_guess;
    i64 inventory;

    // local replica of the touch. this tier does NOT open a live stream - see
    // t3_get_settings - so the only book it ever sees rides along on the responses to its
    // own orders. that is plenty at this cadence, and it has to be remembered between them
    u8  have_book;
    u8  booted;         // have we EVER seen a book. the probe is a one-time bootstrap: once
                        // we have, an empty book means wait, not spam fresh resting probes -
                        // re-probing on every empty-book tick leaks a resting order each time
    u8  book_have_bid;
    u8  book_have_ask;
    u16 last_bid;
    u16 last_ask;
    u32 last_bid_depth;
    u32 last_ask_depth;

    u32 name_idx;
    u32 rng;
} T3;

T3* t3_init();
char* t3_get_name(T3* t3);
u8 t3_on_snapshot(T3* t3, Context* ctx);
void t3_get_settings(T3* t3, ClientSettings* client_settings);
void t3_free(T3* t3);

#endif
