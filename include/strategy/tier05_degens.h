#ifndef TIER05_DEGENS_H
#define TIER05_DEGENS_H

#include "types.h"
#include "client_settings.h"
#include "order.h"
#include "context.h"

// T5 - Degens. WSB / retail day traders: MOMENTUM. buy strength, sell weakness. the
// opposite of the mean-reverting informed tiers - degens CHASE a move and amplify it.
// see TILTYARD_AGENTS.md T5.
//
// two things make this tier matter:
//  - it is DESTABILIZING. T2/T4 push price toward fair value; degens push it further in
//    whatever direction it is already going. they are the amplifier a crash needs.
//  - THE STOP-LOSS POPULATION IS THE FUEL FOR THE CASCADE. every degen enters long and
//    parks a protective sell-stop just below. a dip into that dense band of stops triggers
//    a wave of market sells, which drops price into the next band, and so on. because these
//    are retail longs, the stops are lopsided to the SELL side (few buy-stops above) - which
//    is exactly why real flash crashes are almost always crashes, not melt-ups.
//
// HERD. degens are heavily correlated - everyone hits the same name at once. that falls out
// here for free: they all read the same last-trade tape, so they all compute the same price
// momentum and fire together. a per-agent threshold + a participation gate spread them from
// one identical block into a tight cluster, which is what a herd actually looks like.
//
// LONG-ONLY by construction: a cash account cannot short, which both matches the retail
// reality and produces the sell-stop asymmetry above. a short-heavy melt-up population is a
// deliberate separate config, not this one.
//
// every behavioural knob lives in T5Params so a sweep harness can vary it without
// touching this file. ALL DEFAULTS ARE /* UNCALIBRATED */ PLACEHOLDERS.

// what we last sent and have not heard back about. keyed by the MESSAGE id, like every
// other tier here - a response comes back under the message's id, not the position's
#define T5_PEND_NONE   0
#define T5_PEND_BUY    2  // the momentum entry, crossing the spread
#define T5_PEND_STOP   3  // arming the protective sell-stop
#define T5_PEND_LIMIT  4  // the impatient limit entry, resting at the touch
#define T5_PEND_PULL   5  // pulling an impatient limit that never filled
#define T5_PEND_UNSTOP 6  // pulling the protective stop ahead of a time exit
#define T5_PEND_EXIT   7  // the time exit itself - day traders do not hold overnight

typedef struct T5Params {
    // momentum. an EMA of the last-trade tape is the trend; price above it by
    // up_threshold_ticks is "strength" worth chasing. lower alpha = longer lookback
    u16 ema_alpha_pct;         // 0-100, weight of the newest print in the EMA
    // how far above the trend before we pile in, in BASIS POINTS of the trend - the same
    // reasoning as stop_loss_pct, and it has to be proportional for the same reason. an
    // absolute tick threshold is a 3% move on a $100 stock and a 0.03% move on a $10,000
    // one, so as price runs it quietly decays into "always true" and a momentum trader
    // stops being conditional at all. that is a melt-up with no off switch
    u16 up_threshold_bp;

    // POSITION_CONCENTRATION. retail does not size in fixed share counts, it sizes in
    // money - "I'm putting half my account into this". sizing off cash is also the only
    // thing that survives a trending market: a flat 200 shares is $20k at $100 and $140k
    // at $700, so a fixed count silently stops being affordable and every entry starts
    // coming back rejected. a percent of the account is affordable at any price
    u16 position_concentration_pct;
    u16 min_position_size;   // below this the trade is not worth the commission

    // LEAN. the doc puts taker_probability at ~0.8: four times in five a degen crosses the
    // spread to get in NOW, and the fifth time it perches an impatient limit at the touch
    // and yanks it when it doesn't fill. both are impatient - only one of them waits
    u8  taker_probability_pct;
    u16 marketable_limit_ticks;  // how far THROUGH the touch a marketable limit reaches
    u64 limit_patience_ns;       // how long the perched limit waits before we pull it

    // HERD_CORRELATION. 100 = everyone trades the shared tape signal and the tier moves as
    // one block; 0 = everyone trades their own private noise and there is no herd at all.
    // in between is what a herd actually is - a common signal that most of them read the
    // same way, plus enough private disagreement that they arrive in a cluster not a step
    u8  herd_correlation_pct;
    u16 idiosyncratic_bp;        // scale of the private half of that blend

    // the protective sell-stop, a percent below the entry. scales with price (a fixed tick
    // stop is trivial at $100 and enormous at $10). this IS the sell-weakness exit, and the
    // cascade fuel: a degen buys strength, parks this, and rides until it is stopped out
    u16 stop_loss_pct;

    // herd shaping. a participation gate so a trigger fires a CLUSTER, not one identical
    // block; a per-agent threshold jitter so they do not all cross at the exact same tick
    u8  participation_pct;     // chance (0-100) an eligible degen actually acts this wake
    u16 threshold_jitter_bp;   // proportional, for the same reason as up_threshold_bp

    // WAKE_SIGNAL, modelled as a bursty self-wake: look every so often, act only when the
    // tape is moving. between looks, sleep
    u64 burst_wake_ns;
    u64 burst_jitter_ns;

    // how far apart the tier's agents first show up. a whole-hour initial_wake identical
    // across every instance boots the entire population on the same tick, and fixed
    // periods then keep it there forever
    u64 initial_wake_spread_ns;

    // nothing has printed yet - momentum needs a reference to start from
    u16 seed_price;

    u64 retry_wake_ns;         // market shut
    u64 reject_backoff_ns;
    u64 missed_backoff_ns;     // an IOC that found nothing: a race lost, not an error

    // CAPITAL. the doc's range is thousands to low-hundreds-of-thousands, so it is a
    // DISTRIBUTION, not a number - drawn once per agent. an identical account across the
    // whole tier makes the herd fire in one indivisible block of identical size, which is
    // the one thing the herd is not
    i64 capital_min;
    i64 capital_max;

    // day traders do not hold overnight - it is in the name. without a time exit a degen
    // that never gets stopped out holds its first position forever, and in a trending
    // market the entire tier ends up long and inert, contributing nothing either way
    u64 max_hold_ns;
    u64 processing_time;       // human seconds + retail broadband + PFOF: hundreds of ms
    u64 net_latency;
    u64 initial_wake;
} T5Params;

typedef struct T5 {
    T5Params p;

    // the momentum reference, and whether it has been seeded off a real print yet
    u32 ema_hy;                // EMA of mark, in half ticks, for a little resolution
    u8  ema_ready;

    // local replica of the touch. TIER_MBP1 is a BBO feed - the two prices a broker app
    // shows - and with no live stream it only arrives on the ack to one of our own orders.
    // the market-order path needs none of this; only the limit paths do
    u16 last_bid;
    u16 last_ask;
    u8  have_bid;
    u8  have_ask;

    // this agent's own account, drawn once from [capital_min, capital_max]
    i64 capital;

    // position lifecycle. flat -> long (buy) -> armed (stop parked) -> flat (stop fires,
    // or the time exit pulls the stop and sells out)
    i64 inventory;
    u16 entry_price;
    u8  need_stop;             // long but the protective stop is not parked yet
    u64 position_since_ns;     // when this position was opened, for the intraday time exit
    u8  exiting;               // the time exit is under way: stop pulled, sell to follow

    u32 shot_id;               // the entry buy in flight
    u32 stop_id;               // the armed protective stop (MAX_U32 = none)
    u32 limit_id;              // the perched impatient limit (MAX_U32 = none)
    u64 limit_since_ns;        // when it was perched, for the patience timeout

    // in-flight message
    u8  pending;
    u8  pending_kind;
    u32 pending_id;

    i64 cash_guess;

    // 200 agents against a much shorter handle list, so the slot number goes in the name
    u64 first_wake_ns;         // this agent's own arrival, drawn once
    char name[40];
    u32 name_idx;
    u32 rng;
} T5;

T5* t5_init();
char* t5_get_name(T5* t5);
u8 t5_on_snapshot(T5* t5, Context* ctx);
void t5_get_settings(T5* t5, ClientSettings* client_settings);
void t5_free(T5* t5);

#endif
