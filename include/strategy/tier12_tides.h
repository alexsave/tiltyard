#ifndef TIER12_TIDES_H
#define TIER12_TIDES_H

#include "types.h"
#include "client_settings.h"
#include "order.h"
#include "context.h"

// T12 - Tides. index funds and ETFs rebalancing on a schedule. see TILTYARD_AGENTS.md T12.
//
// the defining fact about this tier is not its size, it is its PREDICTABILITY. a Tide does
// not decide anything - a calendar decides for it, and everyone can see the calendar. its
// MOC flow is the single biggest and most forecastable liquidity event of the day, and it
// is what produces the closing-auction volume spike that the stylized-facts harness looks
// for.
//
// A NOTE ON WHETHER THIS IS A CEILING, because it is easy to assume it is. a CAP-WEIGHTED
// index fund is price-NEUTRAL by construction: when a holding appreciates, its weight in
// the index appreciates by exactly the same amount, and no trade is required at all. that
// is the entire elegance of cap weighting. so a pure tracker leans against nothing. what
// does lean is the tracking-error band around a TARGET weight, which is why
// tracking_error_bp exists here as a real knob rather than as decoration - it is the
// difference between this tier being a stabiliser and being furniture. T13 Glaciers, whose
// mandate is a target weight rather than an index weight, is the stronger version of the
// same idea; this tier is the version that mostly does not fight.
//
// MOC/MOO. the doc's headline behaviour: index funds trade the closing auction because
// their NAV is struck at the closing price, so any fill away from the close is tracking
// error by definition. AUCTION_ONLY_BIT is exactly that instruction - cross or cancel,
// never release the residual into continuous trading.
//
// SELF-SLICING for the non-auction portion, for the same reason as T13: routing to T3 is
// not being shipped.
//
// every behavioural knob lives in T12Params so a sweep harness can vary it without
// touching this file. ALL DEFAULTS ARE /* UNCALIBRATED */ PLACEHOLDERS.

#define T12_PEND_NONE   0
#define T12_PEND_MOC    2  // auction-only interest into the closing cross
#define T12_PEND_SLICE  3  // the continuous-market remainder, worked in slices
#define T12_PEND_CANCEL 4

typedef struct T12Params {
    // AUM. enormous and mechanically deployed. a distribution: a sector ETF and a total
    // market fund are not the same participant
    i64 aum_min;
    i64 aum_max;

    // the index weight this fund tracks, and the band it is allowed to drift inside before
    // tracking error forces a trade. see the header on why this band is the whole story
    u16 index_weight_bp;
    u16 tracking_error_bp;

    // MOC_FRACTION. how much of a rebalance goes into the closing cross rather than being
    // worked through the continuous session. index funds are NAV-matched, so this is high
    u16 moc_fraction_pct;

    // REBALANCE_CALENDAR. daily creation/redemption is small and constant; the quarterly
    // reconstitution is the big one. modelled as a routine cadence plus a periodic large
    // event, which is what the calendar actually looks like from the book's side
    u64 rebalance_period_ns;
    u64 reconstitution_period_ns;
    u16 reconstitution_multiple;   // how much bigger a reconstitution is than a routine day

    // how long before the bell the MOC goes in. real index flow is submitted into the
    // accumulation window so it prints in the cross, not before it
    u64 moc_submit_before_close_ns;

    // the continuous remainder, worked patiently
    u64 slice_interval_ns;
    u16 impact_aversion_pct;
    u16 max_slice;
    u16 min_slice;

    u16 seed_price;

    u64 retry_wake_ns;
    u64 reject_backoff_ns;

    u64 processing_time;
    u64 net_latency;
    u64 initial_wake;
} T12Params;

typedef struct T12 {
    T12Params p;

    i64 aum;
    u16 index_weight;

    i64 inventory;
    i64 cash_guess;

    // the continuous-session remainder still to work
    i64 parent_remaining;
    u8  parent_is_buy;

    // the auction-only leg, submitted once per close
    i64 moc_remaining;
    u8  moc_is_buy;
    u8  moc_sent_today;
    u64 last_rebalance_ns;
    u64 last_reconstitution_ns;

    u32 slice_id;
    u16 slice_price;
    u64 slice_since_ns;

    u8  pending;
    u8  pending_kind;
    u32 pending_id;

    u32 name_idx;
    u32 rng;
} T12;

T12* t12_init();
char* t12_get_name(T12* t12);
u8 t12_on_snapshot(T12* t12, Context* ctx);
void t12_get_settings(T12* t12, ClientSettings* client_settings);
void t12_free(T12* t12);

#endif
