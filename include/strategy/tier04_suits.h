#ifndef TIER04_SUITS_H
#define TIER04_SUITS_H

#include "types.h"
#include "client_settings.h"
#include "order.h"
#include "context.h"

// T4 - Suits. discretionary hedge funds: the INFORMED FLOW. read the engine's news signal,
// form a view of fair value, size a position by conviction, and work it into the book -
// then hold it until the view changes. see TILTYARD_AGENTS.md T4.
//
// this is the highest-value tier for realism after the apex. it is the adverse selection
// that makes a maker's queue modelling earn anything, and the tier that produces
// volatility clustering and fat tails - informed flow arriving in bursts around catalysts.
//
// two things make this tier a POPULATION rather than one big bet:
//  - each fund carries its own persistent NOISE on the fundamental (fair_value_bias). they
//    read the same signal and disagree about what it is worth, so their entries spread into
//    a distribution instead of 60 identical orders at one price.
//  - conviction sizing: the further price is from a fund's fair value, the bigger the
//    position it wants - but it never trades THROUGH fair value, so it buys the dip toward
//    fundamental and stops there. that self-limiting behaviour is the price-discovery force.
//
// ROUTING. the design doc's default is VIA_SLICER: a suit emits a parent to a T3 Slicer and
// its flow becomes sliced children. that needs one client to hand an order to another, which
// is the T11 routing layer and does not exist (and may never). so DIRECT is the only
// workable routing today - the suit works its own view into the book - and VIA_SLICER is
// gated off, reported rather than faked.
//
// every behavioural knob lives in T4Params so a sweep harness can vary it without
// touching this file. ALL DEFAULTS ARE /* UNCALIBRATED */ PLACEHOLDERS.

typedef enum {
    T4_DIRECT,      // work the view into the book ourselves. the only path without routing
    T4_VIA_SLICER   // GATED - emit a parent to a T3 Slicer. needs the routing layer (F15/T11)
} t4_routing_t;

// what we last sent and have not heard back about. a response is keyed by the MESSAGE id,
// never the order it acts on, so remember what we sent to read its ack correctly
#define T4_PEND_NONE   0
#define T4_PEND_PROBE  1  // a tiny order purely to get a book snapshot back
#define T4_PEND_TRADE  2  // a conviction child, working toward target

typedef struct T4Params {
    t4_routing_t routing;

    // fundamental value from the signal. same absolute mapping T2 uses: 128 is neutral and
    // sits on the anchor, a full swing to 0/255 is worth news_full_swing_ticks either side.
    // absolute, never an offset from the book - an offset makes fair value chase price
    u16 fundamental_anchor;
    u16 news_full_swing_ticks;

    // the persistent per-fund disagreement, in ticks. each fund's estimate is the true
    // fundamental plus a fixed draw in [-bias, +bias]. this is what spreads 60 funds into a
    // distribution instead of one block, and its width is the single biggest realism knob
    u16 fair_value_bias_ticks;

    // conviction: shares of target position per tick that price sits away from fair value.
    // bigger mispricing -> bigger intended position, capped at position_limit
    u32 conviction_per_tick;
    i32 position_limit;

    // a dead zone around fair value - inside it we hold, so we do not churn on every tick of
    // noise. a PM does not reposition for a penny
    u16 conviction_deadzone_ticks;

    // how we work toward target. taker-leaning: a suit that has decided to move crosses,
    // it does not sit passively. but never THROUGH its own fair value
    u16 child_size;
    u16 participation_cap_pct;

    // WAKE model. a suit is slow - it looks a few times a day, not tick by tick. between
    // looks it sleeps. news is read whenever it next wakes (the engine does not wake clients
    // on a news event), so a faster cadence just means it reacts to a catalyst sooner
    u64 rebalance_interval_ns;
    u64 decision_delay_ns;   // PM forms a view, then hands to execution: seconds, not ns

    // nothing has printed and no book seen yet - somewhere to price the first probe
    u16 seed_price;

    u64 retry_wake_ns;       // market shut
    u64 reject_backoff_ns;

    // AUM is the fund's capital. carried for realism/reporting; position_limit is the
    // binding constraint on what actually reaches the book
    i64 aum;
    u8  margin_mult;
    u8  maint_pct;
    u64 processing_time;
    u64 net_latency;
    u64 initial_wake;
    // spread the boot phase across the tier. a whole-hour initial_wake shared by every
    // instance starts the entire population on one tick, and no amount of per-agent period
    // skew can pull them apart afterwards - they just march in step at slightly different
    // rates. this is the master phase reference and it has to be per-agent
    u64 initial_wake_spread_ns;
} T4Params;

typedef struct T4 {
    T4Params p;

    // this fund's fixed disagreement with the true fundamental, drawn once. signed ticks
    i32 fair_value_bias;

    // local replica of the touch, cached off our own order responses - no live stream at
    // this cadence, same as a slicer
    u8  have_book;
    u8  book_have_bid;
    u8  book_have_ask;
    u16 last_bid;
    u16 last_ask;
    u32 last_bid_depth;
    u32 last_ask_depth;

    // in-flight message
    u8  pending;
    u8  pending_kind;
    u8  pending_buy;
    u32 pending_id;

    i64 inventory;
    i64 cash_guess;

    u64 first_wake_ns;     // this agent's own boot phase, drawn once
    u32 name_idx;
    u32 rng;
} T4;

T4* t4_init();
char* t4_get_name(T4* t4);
u8 t4_on_snapshot(T4* t4, Context* ctx);
void t4_get_settings(T4* t4, ClientSettings* client_settings);
void t4_free(T4* t4);

#endif
