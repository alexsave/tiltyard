#ifndef TIER08_SETTERS_H
#define TIER08_SETTERS_H

#include "types.h"
#include "client_settings.h"
#include "order.h"
#include "context.h"

// T8 - Setters. swing traders: check the chart after work, leave orders, walk away.
// see TILTYARD_AGENTS.md T8.
//
// the name is the strategy. a Setter does not react to anything - it SETS resting orders
// and is not present when they fill. that makes it the one retail tier whose response
// delay is genuinely irrelevant: it is not racing anyone, it went to bed.
//
// what it contributes that no other retail tier does is RESTING DEPTH AWAY FROM THE TOUCH.
// Tappers and Degens are takers who consume the book; a Setter's patient limit sits below
// the market for days waiting to be hit, and its stop sits below that. so it thickens the
// ladder rather than thinning it, and it is the tier that makes support and resistance
// levels a real feature of the book rather than a story people tell about one.
//
// the flip side, and it matters for cascade dynamics: those stops are ANOTHER dense band
// of protective sell orders sitting under the market, on top of the Degens'. the two
// populations stack, and a move that reaches either one finds the other just below it.
//
// GTC/GTD is what makes this tier different mechanically - its orders survive the close.
// everything else here trades within a session and goes flat or gets swept at the bell.
//
// every behavioural knob lives in T8Params so a sweep harness can vary it without
// touching this file. ALL DEFAULTS ARE /* UNCALIBRATED */ PLACEHOLDERS.

#define T8_PEND_NONE   0
#define T8_PEND_ENTRY  2  // the patient limit at support
#define T8_PEND_STOP   3  // the protective stop under the position
#define T8_PEND_PULL   4  // cancelling an entry whose ttl ran out
#define T8_PEND_UNSTOP 5  // pulling the stop ahead of a horizon exit
#define T8_PEND_EXIT   6  // the horizon exit - the trade simply ran out of time

typedef struct T8Params {
    // CHECK_TIME_OF_DAY. seconds into the day when this person looks at the chart. after
    // work, roughly, and spread across the population - not everyone eats at the same hour
    u32 check_time_of_day_s;
    u32 check_spread_s;

    // ENTRY_OFFSET. support is "a bit below where it is now" - a patient bid that only
    // fills if the market comes to it. proportional, because a tick is a different trade
    // at $10 and at $10,000
    u16 entry_offset_bp;

    // the protective stop, below the entry. same cascade-fuel role as T5's
    u16 stop_loss_pct;

    // HOLD_HORIZON. days to weeks, then out regardless - a swing trade that has not worked
    // is closed and the capital redeployed
    u64 hold_horizon_ns;

    // ORDER_TTL. a resting entry that never filled is not left in the book forever. this is
    // what GTD expresses, and pulling it is the client's job when the ttl is behavioural
    u64 order_ttl_ns;

    // LEAN. the doc puts taker_probability at ~0.3 - a Setter mostly rests, but sometimes
    // sees the move already happening and takes it rather than miss it
    u8 taker_probability_pct;

    // sizing, in money like every other retail tier
    u16 position_concentration_pct;
    u16 min_position_size;

    u64 retry_wake_ns;
    u64 reject_backoff_ns;

    i64 capital_min;
    i64 capital_max;

    // irrelevant, and deliberately so - see the header
    u64 processing_time;
    u64 net_latency;
    u64 initial_wake;
} T8Params;

typedef struct T8 {
    T8Params p;

    i64 capital;
    i64 inventory;
    i64 cash_guess;
    u16 entry_price;

    // the resting entry limit, and the stop that guards the position once it fills
    u32 entry_id;
    u64 entry_since_ns;
    u32 stop_id;
    u8  need_stop;

    u64 position_since_ns;
    u8  exiting;

    // local replica of the touch. an entry is priced off it, then left alone
    u16 last_bid;
    u16 last_ask;
    u8  have_bid;
    u8  have_ask;

    u8  pending;
    u8  pending_kind;
    u32 pending_id;

    u32 check_offset_s;   // this person's own after-work hour, drawn once

    u32 name_idx;
    u32 rng;
} T8;

T8* t8_init();
char* t8_get_name(T8* t8);
u8 t8_on_snapshot(T8* t8, Context* ctx);
void t8_get_settings(T8* t8, ClientSettings* client_settings);
void t8_free(T8* t8);

#endif
