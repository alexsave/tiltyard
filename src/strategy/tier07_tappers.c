#include <stdlib.h>
#include <stdio.h>

#include "strategy/tier07_tappers.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"
#include "mbp.h" // MBP1: the one line of price the app shows
#include "server.h"
#include "response.h"

// T7 - Tappers. open app, tap, close app. the uninformed flow everything else trades
// against.
//
// the sim runs 0 for now. the real population is tens of millions (Robinhood alone ~25M
// funded accounts); the doc recommends AGGREGATE for apex work and POPULATED as an
// architecture stress test.

// broker / app handles, loosely parodied. one per instance, handed out in init order
static const char* T7_NAMES[] = {
    "robinghood.user",  "webulls.trader",   "cashe.app.invest", "sofa.invest",
    "publik.trader",    "stashe.away",      "acornz.roundup",   "m1.financier",
    "etoros.copier",    "moomu.trader",     "tastyworx.retail", "firsttrade.tap",
};
static const u32 T7_NAME_COUNT = sizeof(T7_NAMES) / sizeof(T7_NAMES[0]);
static u32 t7_next_name = 0;

// the tunable block. EVERY value here is a placeholder - none of it is calibrated, and
// none of it should be tuned to make the tape "look right". that is the owner's job.
// a function rather than file scope so it can use the time constants: a const u64 is not
// a constant expression in C, so S_TO_NS cannot seed a static
static T7Params t7_defaults() {
    T7Params p;

    // per agent this is slow. the tier's weight comes from headcount, not frequency
    p.wake_rate_ns          = 6 * H_TO_NS;  /* UNCALIBRATED */

    // the intraday U: busy at the bell, dead at lunch, busy again after work
    p.open_burst_pct        = 25;   /* UNCALIBRATED */ // a quarter of the base gap
    p.midday_quiet_pct      = 250;  /* UNCALIBRATED */ // two and a half times it

    p.order_value_min       = 100;  /* UNCALIBRATED */
    p.order_value_max       = 5000; /* UNCALIBRATED */

    p.buy_bias_pct          = 60;   /* UNCALIBRATED */ // mild, per the doc
    p.taker_probability_pct = 95;   /* UNCALIBRATED */
    p.marketable_limit_ticks= 2;    /* UNCALIBRATED */
    p.min_lot               = 1;    /* UNCALIBRATED */

    p.retry_wake_ns         = 2 * H_TO_NS;  /* UNCALIBRATED */
    p.reject_backoff_ns     = 30 * MIN_TO_NS; /* UNCALIBRATED */
    p.initial_wake_spread_ns= 8 * H_TO_NS;    /* UNCALIBRATED */

    p.capital_min           = 500;   /* UNCALIBRATED */
    p.capital_max           = 40000; /* UNCALIBRATED */

    // human tap + retail broadband + PFOF routing: hundreds of ms wire-to-book
    p.processing_time       = 400 * (S_TO_NS / 1000); /* UNCALIBRATED */ // 400ms
    p.net_latency           = 120 * (S_TO_NS / 1000); /* UNCALIBRATED */ // 120ms
    p.initial_wake          = 14 * H_TO_NS; /* UNCALIBRATED */

    return p;
}

// xorshift, per agent. determinism is a hard rule: same seed in, same sequence out
static u32 t7_rand(T7* t7) {
    u32 x = t7->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t7->rng = x;
    return x;
}

T7* t7_init() {
    T7* t7 = malloc(sizeof(T7));

    t7->p = t7_defaults();

    t7->inventory = 0;
    t7->last_bid = 0;
    t7->last_ask = 0;
    t7->have_bid = 0;
    t7->have_ask = 0;
    t7->pending = 0;
    t7->pending_id = MAX_U32;

    // names go out in init order, so the roster is stable across runs
    t7->name_idx = t7_next_name % T7_NAME_COUNT;
    // each agent carries its own rng state, seeded off its slot. no shared global rng
    t7->rng = 0xc2b2ae35u * (t7_next_name + 1);

    t7->first_wake_ns = t7->p.initial_wake
                      + (u64)(t7_rand(t7) % 1000) * (t7->p.initial_wake_spread_ns / 1000);

    i64 span = t7->p.capital_max - t7->p.capital_min;
    t7->capital = t7->p.capital_min + (i64)(t7_rand(t7) % (u32)(span + 1));
    t7->cash_guess = t7->capital;

    t7_next_name++;

    return t7;
}

char* t7_get_name(T7* t7) {
    return (char*)T7_NAMES[t7->name_idx];
}

// WAKE_POISSON with TOD_MODULATION. an exponential gap would need a log; a sum of uniforms
// is close enough in shape for a tier whose whole personality is "arrives at random", and
// it keeps the tier deterministic and integer-only
static u64 t7_next_gap(T7* t7, Context* ctx) {
    u64 base = t7->p.wake_rate_ns;

    // where we are in the session decides how busy people are. seconds into the day
    u64 tod = (ctx->real_time_ns % DAY_TO_NS) / S_TO_NS;
    u16 scale = 100;
    if (tod >= 52200 && tod < 55800)        // first hour after the bell
        scale = t7->p.open_burst_pct;
    else if (tod >= 59400 && tod < 68400)   // the midday lull
        scale = t7->p.midday_quiet_pct;

    base = base * (u64)scale / 100;
    if (base == 0)
        base = S_TO_NS;

    // two uniforms averaged: a hump rather than a flat block, cheap and deterministic
    u64 a = (u64)(t7_rand(t7) % 1000) * (base / 500);
    u64 b = (u64)(t7_rand(t7) % 1000) * (base / 500);
    return (a + b) / 2 + base / 4;
}

// same rule as the Degens, and it matters more here: this tier is supposed to be a
// memoryless arrival process, and a memoryless process with a fixed overnight step is not
// memoryless. t7_next_gap already draws a random intraday gap, but retry_wake_ns (2h) and
// reject_backoff_ns (30m) were exact - so every night the whole population stepped in
// clean 2-hour multiples off a 14:00:00.000000000 boot and arrived at the open perfectly
// in phase, having thrown away whatever scatter the previous session gave it
static u64 t7_scatter(T7* t7, u64 base) {
    if (base == 0)
        return 0;
    return base / 2 + (u64)(t7_rand(t7) % 1001) * (base / 1000);
}

static u8 t7_sleep(T7* t7, Context* ctx, u64 base) {
    u64 delay = t7_scatter(t7, base);
    if (ctx->real_time_ns + delay >= ctx->next_wake_ns)
        return 0;
    ctx->wake_delay_ns = delay;
    return 2;
}

// the BBO the app shows. only the marketable-limit sliver needs it
static void t7_read_book(T7* t7, Context* ctx) {
    MBP1* bbo = (MBP1*)ctx->data_snapshot;
    if (!bbo)
        return;
    if (bbo->hi_bid.price) { t7->last_bid = bbo->hi_bid.price; t7->have_bid = 1; }
    if (bbo->lo_ask.price) { t7->last_ask = bbo->lo_ask.price; t7->have_ask = 1; }
}

u8 t7_on_snapshot(T7* t7, Context* ctx) {
    Order* out = ctx->next_order_ptr;

    u8 is_fill   = (ctx->status >> FILL_BIT) & 1;
    u8 is_reject = (ctx->status >> REJECT_BIT) & 1;

    t7_read_book(t7, ctx);

    if (t7->pending && ctx->order_id == t7->pending_id) {
        t7->pending = 0;
        t7->pending_id = MAX_U32;
        if (is_fill && ctx->quantity_filled) {
            u16 px = ctx->price ? ctx->price : ctx->mark;
            // direction is not carried on the response, and this tier does not track it -
            // inventory moves with the sign we remember on the way out
            t7->inventory += (i64)ctx->quantity_filled;
            t7->cash_guess -= (i64)ctx->quantity_filled * (i64)px;
        }
        if (is_reject)
            return t7_sleep(t7, ctx, t7->p.reject_backoff_ns);
    }

    if (t7->pending)
        return t7_sleep(t7, ctx, t7_next_gap(t7, ctx));

    if (!ctx->is_open)
        return t7_sleep(t7, ctx, t7->p.retry_wake_ns);

    if (ctx->mark == 0)
        return t7_sleep(t7, ctx, t7->p.retry_wake_ns);

    // THE ENTIRE DECISION. a coin weighted slightly to the buy side, and nothing else -
    // no price test, no trend, no valuation. this is what uninformed means
    u8 buy = (t7_rand(t7) % 100) < t7->p.buy_bias_pct;

    i64 vspan = t7->p.order_value_max - t7->p.order_value_min;
    i64 value = t7->p.order_value_min + (i64)(t7_rand(t7) % (u32)(vspan + 1));
    i64 shares = value / (i64)ctx->mark;
    if (shares < (i64)t7->p.min_lot)
        shares = t7->p.min_lot;
    if (shares > MAX_U16)
        shares = MAX_U16;

    // a cash account cannot sell what it does not hold. rather than drop the order, it
    // becomes a buy - which is also what the app would offer someone with no position
    if (!buy && t7->inventory < shares) {
        if (t7->inventory <= 0)
            buy = 1;
        else
            shares = t7->inventory;
    }

    if (buy && (i64)shares * (i64)ctx->mark > t7->cash_guess) {
        shares = t7->cash_guess / (i64)ctx->mark;
        if (shares < (i64)t7->p.min_lot)
            return t7_sleep(t7, ctx, t7_next_gap(t7, ctx)); // out of money
    }

    if (!buy)
        t7->inventory -= shares; // sells are booked out on send; buys settle on the fill

    u8 cross = (t7_rand(t7) % 100) < t7->p.taker_probability_pct;
    u8 have_side = buy ? t7->have_ask : t7->have_bid;

    if (!cross && have_side) {
        // the app's limit button, set close enough that it fills anyway
        u32 px = buy ? (u32)t7->last_ask + t7->p.marketable_limit_ticks
                     : (u32)t7->last_bid > t7->p.marketable_limit_ticks
                       ? (u32)t7->last_bid - t7->p.marketable_limit_ticks : 1;
        out->status = (1 << IOC_BIT) | (buy ? (1 << BUY_DIRECTION_BIT) : 0);
        out->price = px > MAX_U16 ? MAX_U16 : (u16)px;
    } else {
        out->status = (1 << IS_MARKET_BIT) | (1 << IOC_BIT) |
                      (buy ? (1 << BUY_DIRECTION_BIT) : 0);
        out->price = 0;
    }
    out->quantity = (u16)shares;

    t7->pending = 1;
    t7->pending_id = ctx->next_order_id;
    return 1;
}

void t7_get_settings(T7* t7, ClientSettings* client_settings) {
    client_settings->initial_wake    = t7->first_wake_ns;
    client_settings->processing_time = t7->p.processing_time;
    client_settings->net_latency     = t7->p.net_latency;

    // cash account. retail brokerage default, and it is what keeps this tier long-only
    client_settings->is_cash_account = 1;
    client_settings->cash            = t7->capital;
    client_settings->reserved_cash   = 0;
    client_settings->shares          = 0;
    client_settings->reserved_shares = 0;

    // L1: one line of price in an app. the doc notes it is sometimes delayed, which we do
    // not model - the tier is price-insensitive enough that a stale quote changes nothing
    client_settings->sub_tier = TIER_MBP1;
    client_settings->noii     = 0;
}

void t7_free(T7* t7) {
    free(t7);
}
