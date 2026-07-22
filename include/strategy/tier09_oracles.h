#ifndef TIER09_ORACLES_H
#define TIER09_ORACLES_H

#include "types.h"
#include "client_settings.h"
#include "order.h"
#include "context.h"

// T9 - Oracles. value investors: patient, contrarian, fundamental-anchored. buy when price
// is far BELOW fair value, sell when far ABOVE, and otherwise do nothing. see
// TILTYARD_AGENTS.md T9.
//
// THIS IS THE ARRESTING LOOP. it is the one tier whose quotes are anchored to the
// fundamental instead of to the last trade. everyone else (makers on the mark, momentum
// degens on the trend, takers at the touch) lets price random-walk away from value with
// nothing pulling it home; a crash to a tenth of fair value has no floor. an Oracle parks a
// deep, patient bid a fixed fraction BELOW fair value - so when a cascade drives price down
// into it, that bid absorbs the selling and stops the fall near fundamental. symmetrically
// it parks an ask above fair value as a ceiling. the DEPTH of that capital (position_limit)
// is, per the design doc, the single most important calibration knob for crash dynamics:
// same shock, and whether it runs away or mean-reverts is decided here.
//
// near-zero flow the rest of the time - it sleeps through millions of events and the price
// sitting inside its value band, and only acts when price leaves that band. its footprint is
// small until it isn't.
//
// DATA_NONE: it does not read the book or the tape for signal. it needs only the last price
// (to know where price sits vs its estimate) and the fundamental (the news signal). both
// ride on the Context, so it takes no live stream - the free tier.
//
// every behavioural knob lives in T9Params so a sweep harness can vary it without
// touching this file. ALL DEFAULTS ARE /* UNCALIBRATED */ PLACEHOLDERS.

// what we last sent and have not heard back about, keyed by the MESSAGE id
#define T9_PEND_NONE   0
#define T9_PEND_QUOTE  2  // post or replace the resting value order
#define T9_PEND_CANCEL 3  // pull it (price wandered back inside the value band)

typedef struct T9Params {
    // fair value from the signal. same absolute mapping T2/T4 use: 128 neutral sits on the
    // anchor, a full swing to 0/255 is worth news_full_swing_ticks either side. absolute, so
    // the whole tier agrees on roughly where value is - which is what lets them be a floor
    u16 fundamental_anchor;
    u16 news_full_swing_ticks;

    // each Oracle's estimate is the true fundamental plus a fixed per-agent read, so their
    // support/resistance sits at a spread of levels rather than one identical price
    u16 fair_value_bias_ticks;

    // the value band: park the bid this far BELOW fair value and the ask this far ABOVE.
    // wide = deep-value patient (rarely triggered, but a hard floor when it is); narrow =
    // eager. inside the band it does nothing
    u16 value_band_pct;

    // how much to show per resting order, and the most it will ever accumulate. position_limit
    // is the arrest-depth knob: how much contrarian capital stands behind the floor
    u16 chunk_size;
    i32 position_limit;

    // WAKE: it mostly sleeps. a slow refresh so the standing orders track fair value as the
    // news moves it; the price-level trigger is the resting limit itself filling when a move
    // reaches it
    u64 refresh_ns;

    // nothing has printed yet - need somewhere to anchor before the first news
    u16 seed_price;

    u64 retry_wake_ns;
    u64 reject_backoff_ns;

    // enormous capital, patiently deployed. cash account is fine - it accumulates longs it
    // holds for a long time and distributes them; it never needs to short
    i64 cash;
    u64 processing_time;  // irrelevant to this tier - committee-slow, not latency-sensitive
    u64 net_latency;
    u64 initial_wake;
} T9Params;

typedef struct T9 {
    T9Params p;

    // this Oracle's fixed disagreement with the true fundamental, drawn once. signed ticks
    i32 fair_value_bias;

    // the single resting value order (bid floor or ask ceiling). MAX_U32 = none
    u32 order_id;
    u16 order_price;
    u8  order_is_buy;

    // in-flight message
    u8  pending;
    u8  pending_kind;
    u32 pending_id;
    u8  pending_is_buy;

    i64 inventory;
    i64 cash_guess;

    u32 name_idx;
    u32 rng;
} T9;

T9* t9_init();
char* t9_get_name(T9* t9);
u8 t9_on_snapshot(T9* t9, Context* ctx);
void t9_get_settings(T9* t9, ClientSettings* client_settings);
void t9_free(T9* t9);

#endif
