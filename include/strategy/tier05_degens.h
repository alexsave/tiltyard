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
#define T5_PEND_BUY    2  // the momentum entry, a market-buy IOC
#define T5_PEND_STOP   3  // arming the protective sell-stop

typedef struct T5Params {
    // momentum. an EMA of the last-trade tape is the trend; price above it by
    // up_threshold_ticks is "strength" worth chasing. lower alpha = longer lookback
    u16 ema_alpha_pct;         // 0-100, weight of the newest print in the EMA
    u16 up_threshold_ticks;    // how far above the trend before we pile in

    // position. small and concentrated, retail size
    u16 position_size;

    // the protective sell-stop, a percent below the entry. scales with price (a fixed tick
    // stop is trivial at $100 and enormous at $10). this IS the sell-weakness exit, and the
    // cascade fuel: a degen buys strength, parks this, and rides until it is stopped out
    u16 stop_loss_pct;

    // herd shaping. a participation gate so a trigger fires a CLUSTER, not one identical
    // block; a per-agent threshold jitter so they do not all cross at the exact same tick
    u8  participation_pct;     // chance (0-100) an eligible degen actually acts this wake
    u16 threshold_jitter_ticks;

    // WAKE_SIGNAL, modelled as a bursty self-wake: look every so often, act only when the
    // tape is moving. between looks, sleep
    u64 burst_wake_ns;
    u64 burst_jitter_ns;

    // nothing has printed yet - momentum needs a reference to start from
    u16 seed_price;

    u64 retry_wake_ns;         // market shut
    u64 reject_backoff_ns;
    u64 missed_backoff_ns;     // an IOC that found nothing: a race lost, not an error

    // capital. thousands to low-hundreds-of-thousands, cash account (long-only, no shorting)
    i64 cash;
    u64 processing_time;       // human seconds + retail broadband + PFOF: hundreds of ms
    u64 net_latency;
    u64 initial_wake;
} T5Params;

typedef struct T5 {
    T5Params p;

    // the momentum reference, and whether it has been seeded off a real print yet
    u32 ema_hy;                // EMA of mark, in half ticks, for a little resolution
    u8  ema_ready;

    // position lifecycle. flat -> long (buy) -> armed (stop parked) -> flat (stop fires)
    i64 inventory;
    u16 entry_price;
    u8  need_stop;             // long but the protective stop is not parked yet

    u32 shot_id;               // the entry buy in flight
    u32 stop_id;               // the armed protective stop (MAX_U32 = none)

    // in-flight message
    u8  pending;
    u8  pending_kind;
    u32 pending_id;

    i64 cash_guess;

    u32 name_idx;
    u32 rng;
} T5;

T5* t5_init();
char* t5_get_name(T5* t5);
u8 t5_on_snapshot(T5* t5, Context* ctx);
void t5_get_settings(T5* t5, ClientSettings* client_settings);
void t5_free(T5* t5);

#endif
