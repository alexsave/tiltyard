#ifndef TIER01_FLICKERERS_H
#define TIER01_FLICKERERS_H

#include "types.h"
#include "client_settings.h"
#include "order.h"
#include "context.h"

// T1 - Flickerers. hft market makers: continuous two-sided quotes, skewed away from
// inventory, requoted on queue slip / staleness / sweep. see TILTYARD_AGENTS.md T1.
//
// every behavioural knob lives in T1Params so a sweep harness can vary it without
// touching this file. ALL DEFAULTS ARE /* UNCALIBRATED */ PLACEHOLDERS.

typedef struct T1Params {
    // core quote shape
    u16 quote_size;               // shares per side. NOT derived from inventory
    u16 base_half_spread_ticks;   // half-spread around mid, before skew
    u16 min_half_spread_ticks;    // floor. guards the integer-truncation locked market

    // inventory skew: shift BOTH quotes by (inventory / skew_unit) * skew_coeff ticks,
    // away from the position. long -> both lower, inviting sells
    i32 inventory_limit;          // +/- cap. at the cap that side stops quoting
    u16 skew_coeff;               // ticks of shift per skew_unit of inventory
    u16 skew_unit;                // shares per unit of skew

    // requote triggers
    u32 requote_queue_slip_threshold; // shares ahead of us before we re-post for priority
    u64 max_quote_age_ns;             // stale quote, look at it again
    u64 min_requote_interval_ns;      // rate limiter. without it 8 makers requote each other forever
    u16 requote_move_ticks;           // mid moved this far from our quote -> requote

    // we are event driven, but a dead book sends no events and an auction window sends
    // none either, so we arrange our own next look
    u64 idle_wake_ns;                 // heartbeat while trading
    u64 retry_wake_ns;                // market shut / pre-open window, come back later
    u64 reject_backoff_ns;            // a standing reject does not clear in a nanosecond

    // adverse selection: a top-of-book level draining by this much in one update reads
    // as a sweep, and we widen for the cooldown rather than get picked off again
    u32 sweep_pull_threshold;
    u64 sweep_cooldown_ns;
    u16 sweep_widen_ticks;

    // the book is empty and nothing has printed yet, so there is no mid to quote around
    u16 seed_price;

    // account / connection shape
    i64 cash;
    u8  margin_mult;
    u8  maint_pct;
    u64 processing_time;
    u64 net_latency;
    u64 initial_wake;
} T1Params;

typedef struct T1 {
    T1Params p;

    // resting quote ids. MAX_U32 = not resting
    u32 bid_id;
    u32 ask_id;
    u16 bid_price;
    u16 ask_price;
    u64 quoted_ns;

    // a pair mints the ask id server-side, so we do not know it until the ack lands.
    // do not send another quote while this is set, or we spam ids we can never cancel
    u8 pending;

    // our own books. the engine will not tell us, so we track what we sent and got
    i64 inventory;
    i64 cash_guess;

    // top-of-book depth last time we looked, for sweep detection
    u32 last_bid_depth;
    u32 last_ask_depth;
    u64 sweep_until_ns;

    u8 connected;
    u32 name_idx;
    u32 rng;
} T1;

T1* t1_init();
char* t1_get_name(T1* t1);
u8 t1_on_snapshot(T1* t1, Context* ctx);
void t1_get_settings(T1* t1, ClientSettings* client_settings);
void t1_free(T1* t1);

#endif
