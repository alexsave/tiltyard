#ifndef TIER07_TAPPERS_H
#define TIER07_TAPPERS_H

#include "types.h"
#include "client_settings.h"
#include "order.h"
#include "context.h"

// T7 - Tappers. Robinhood-style retail: tens of millions of accounts, near-pure taker,
// essentially no strategy at all. see TILTYARD_AGENTS.md T7.
//
// the distinction from T5 Degens matters and is easy to lose. a Degen has a THESIS - it
// reads momentum, sizes into it, parks a stop, and manages the position. a Tapper opens an
// app, taps buy, and closes the app. there is no signal here, no trend following, no exit
// plan: just a weak sentiment drift and a mild net-long bias. if this tier ever appears to
// be trading cleverly, something has been added that does not belong.
//
// what it contributes is UNINFORMED FLOW - the background against which informed flow is
// informed. a book with only strategic participants has no one to be right against; every
// order carries information and the makers have nothing to earn a spread from. this is the
// tier that pays the spread, and the sim is unrealistic without it.
//
// near-pure taker (the doc puts taker_probability at ~0.95) and price-INSENSITIVE in the
// same direction as T10: mild net-long bias means it leans buy at any level. it is not as
// unconditional as a payroll deduction, but it is not a ceiling either.
//
// every behavioural knob lives in T7Params so a sweep harness can vary it without
// touching this file. ALL DEFAULTS ARE /* UNCALIBRATED */ PLACEHOLDERS.

#define T7_PEND_NONE 0
#define T7_PEND_ORDER 2

typedef struct T7Params {
    // WAKE_POISSON. mean gap between one person opening the app and the next time they do.
    // per agent this is slow - the tier's flow comes from there being millions of them
    u64 wake_rate_ns;

    // TOD_MODULATION. retail clusters at the open and again after work; midday is quiet.
    // expressed as a multiplier on the wake gap in each phase - larger gap = quieter
    u16 open_burst_pct;      // % of the base gap during the first hour (smaller = busier)
    u16 midday_quiet_pct;    // % of the base gap in the middle of the session

    // ORDER_SIZE_DIST. small, and fractional in reality - the book only takes whole shares,
    // so the floor is one. sized in MONEY, because that is how the app asks the question
    i64 order_value_min;
    i64 order_value_max;

    // BUY_BIAS. the doc's "mild net-long bias" - retail buys more often than it sells, and
    // sells mostly to raise cash rather than because of a view
    u8 buy_bias_pct;

    // LEAN. ~0.95 taker. the remaining sliver is a marketable limit rather than a naked
    // market order - the app's "limit" button, set close enough that it fills anyway
    u8 taker_probability_pct;
    u16 marketable_limit_ticks;

    // a sell needs something to sell. with nothing held, the order flips to a buy rather
    // than being dropped, which is also what the app would do
    u16 min_lot;

    u64 retry_wake_ns;
    u64 reject_backoff_ns;

    // how far apart the tier's agents first show up - see T5, same reasoning
    u64 initial_wake_spread_ns;

    // capital. median account is low thousands
    i64 capital_min;
    i64 capital_max;

    // hundreds of ms wire-to-book: human tap, retail broadband, PFOF routing
    u64 processing_time;
    u64 net_latency;
    u64 initial_wake;
} T7Params;

typedef struct T7 {
    T7Params p;

    i64 capital;
    i64 inventory;
    i64 cash_guess;

    // local replica of the touch, for the marketable-limit sliver only
    u16 last_bid;
    u16 last_ask;
    u8  have_bid;
    u8  have_ask;

    u64 first_wake_ns;     // this agent's own arrival, drawn once

    u8  pending;
    u32 pending_id;

    u32 name_idx;
    u32 rng;
} T7;

T7* t7_init();
char* t7_get_name(T7* t7);
u8 t7_on_snapshot(T7* t7, Context* ctx);
void t7_get_settings(T7* t7, ClientSettings* client_settings);
void t7_free(T7* t7);

#endif
