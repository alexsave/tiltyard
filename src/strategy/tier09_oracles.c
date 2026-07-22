#include <stdlib.h>
#include <stdio.h>

#include "strategy/tier09_oracles.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"
#include "server.h"
#include "response.h"

// T9 - Oracles. patient contrarian value. rest a deep bid below fair value (the crash
// floor) and an ask above it (the ceiling), anchored to the fundamental, not the tape.
//
// the sim runs 30. true concentrated value pickers are professionally rare; the design doc
// puts the tier at 10s-100s. 30 deep pools against a population that otherwise lets price
// wander is enough to give a shock a floor without turning the market into a value machine.

// value-shop names, loosely parodied. one per instance, handed out in init order
static const char* T9_NAMES[] = {
    "berkshire.hollow",   // the buy-and-hold-forever crowd, GTA-style
    "graham.dodd.co",
    "margin.of.safety",
    "oakmark.adjacent",
    "dodge.and.cox.ish",
    "baupost.echo",
    "third.avenue.value",
    "tweedy.browne.ish",
    "sequoia.grove",
    "intrinsic.partners",
};
static const u32 T9_NAME_COUNT = sizeof(T9_NAMES) / sizeof(T9_NAMES[0]);
static u32 t9_next_name = 0;

// the tunable block. EVERY value here is a placeholder - none of it is calibrated, and
// none of it should be tuned to make the tape "look right". that is the owner's job.
// a function rather than file scope so it can use the time constants: a const u64 is not
// a constant expression in C, so S_TO_NS cannot seed a static
static T9Params t9_defaults() {
    T9Params p;

    p.fundamental_anchor     = 100;   /* UNCALIBRATED */
    p.news_full_swing_ticks  = 8;     /* UNCALIBRATED */
    p.fair_value_bias_ticks  = 5;     /* UNCALIBRATED */

    p.value_band_pct         = 10;    /* UNCALIBRATED */ // buy 10% under fair value, sell 10% over

    p.chunk_size             = 1000;  /* UNCALIBRATED */
    // the arrest-depth knob. big on purpose: an Oracle is a deep pool, and this is what
    // decides whether a cascade is caught or runs away
    p.position_limit         = 50000; /* UNCALIBRATED */

    // committee-slow: look a few times a session to track fair value; the resting order is
    // the real trigger, filling when a move reaches it
    p.refresh_ns             = 1 * H_TO_NS; /* UNCALIBRATED */

    p.seed_price             = 100;   /* UNCALIBRATED */

    p.retry_wake_ns          = 30 * MIN_TO_NS; /* UNCALIBRATED */
    p.reject_backoff_ns      = 5 * MIN_TO_NS;  /* UNCALIBRATED */

    p.cash                   = 10000000000; /* UNCALIBRATED */ // $10bn, deep pockets
    // latency is irrelevant at this cadence - this is the least latency-sensitive tier there is
    p.processing_time        = 5 * S_TO_NS; /* UNCALIBRATED */
    p.net_latency            = 5 * S_TO_NS; /* UNCALIBRATED */
    p.initial_wake           = 13 * H_TO_NS; /* UNCALIBRATED */

    return p;
}

// xorshift, per agent. determinism is a hard rule: same seed in, same sequence out
static u32 t9_rand(T9* t9) {
    u32 x = t9->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t9->rng = x;
    return x;
}

// PER-AGENT SKEW ON THE CADENCE PARAMS, drawn once at init and then fixed.
//
// unlike the casual tiers, this one genuinely does have a cadence - the fix is not to
// remove the clock, it is to stop every agent sharing the same one. N instances holding an
// identical timer all fire on the same tick, which makes them one agent N times larger
// rather than N agents. that is what put a 7x spike in minute :00 and a bar on the chart
// every 30 minutes: not real market structure, just arithmetic on identical constants
static u64 t9_skew(T9* t9, u64 base) {
    if (base == 0)
        return 0;
    // uniform on roughly [75%, 125%] of the tier value
    return base * 3 / 4 + (u64)(t9_rand(t9) % 1001) * (base / 2000);
}

T9* t9_init() {
    T9* t9 = malloc(sizeof(T9));

    t9->p = t9_defaults();

    t9->order_id = MAX_U32;
    t9->order_price = 0;
    t9->order_is_buy = 0;
    t9->pending = 0;
    t9->pending_kind = T9_PEND_NONE;
    t9->pending_id = MAX_U32;
    t9->pending_is_buy = 0;
    t9->inventory = 0;
    t9->cash_guess = t9->p.cash;

    // names go out in init order, so the roster is stable across runs
    t9->name_idx = t9_next_name % T9_NAME_COUNT;
    // each agent carries its own rng state, seeded off its slot. no shared global rng
    t9->rng = 0x6c62272eu * (t9_next_name + 1);

    // no two of them run on the same clock
    t9->p.refresh_ns = t9_skew(t9, t9->p.refresh_ns);

    // this Oracle's fixed disagreement with the true fundamental, drawn once: a symmetric
    // offset in [-bias, +bias], mean-zero across the population so there is no baked-in drift
    u16 span = 2 * t9->p.fair_value_bias_ticks + 1;
    t9->fair_value_bias = (i32)(t9_rand(t9) % span) - (i32)t9->p.fair_value_bias_ticks;

    t9_next_name++;
    return t9;
}

char* t9_get_name(T9* t9) {
    return (char*)T9_NAMES[t9->name_idx];
}

// "wake me in n ns" - action bit 2. an Oracle mostly sleeps; the standing limit orders are
// the real triggers. the engine keeps only the earliest wake in flight
static u8 t9_sleep(Context* ctx, u64 delay) {
    // clamp inside a pending wake rather than deferring to it - see tier13_glaciers.c for
    // the failure this avoids: a wake delivered a hair before its own fire_at leaves
    // next_wake_ns advertising one that has already been spent, and every later re-arm is
    // then dropped against that stale value.
    //
    // ONLY the feed-less calendar tiers do this, and the distinction is load-bearing. a
    // tier on a live stream is woken by the tape whatever happens, so deferring costs it
    // nothing - and clamping would defeat the wake dedup that stops a client arming a wake
    // per event. applying this to the makers reintroduced exactly that storm and crashed
    // the sim inside one second. this tier has no feed, so a lost wake is the end of it,
    // and its cadence is minutes-to-days so a clamp cannot spin
    if (ctx->real_time_ns + delay >= ctx->next_wake_ns) {
        if (ctx->next_wake_ns <= ctx->real_time_ns + 1)
            return 0;
        delay = ctx->next_wake_ns - ctx->real_time_ns - 1;
    }
    ctx->wake_delay_ns = delay;
    return 2;
}

static u8 t9_await(T9* t9, Context* ctx, u8 kind, u8 is_buy) {
    t9->pending = 1;
    t9->pending_kind = kind;
    t9->pending_id = ctx->next_order_id;
    t9->pending_is_buy = is_buy;
    return 1;
}

// this Oracle's fair value in whole ticks: the true fundamental from the signal plus its own
// fixed bias. before any news the signal is neutral, so everyone sits near the anchor
static u32 t9_fair_value(T9* t9, Context* ctx) {
    i32 level = ctx->last_news_ns ? (i32)ctx->news_signal : 128; // neutral until news prints
    i32 offset = level - 128;
    i64 fv = (i64)t9->p.fundamental_anchor
           + ((i64)offset * (i64)t9->p.news_full_swing_ticks) / 128
           + (i64)t9->fair_value_bias;
    if (fv < 1)
        fv = 1; // price 0 is reserved
    return (u32)fv;
}

// fold a response into our own books, and clear the in-flight message
static u8 t9_settle(T9* t9, Context* ctx) {
    u8 is_fill   = (ctx->status >> FILL_BIT) & 1;
    u8 is_partial = (ctx->status >> PARTIAL_FILL_BIT) & 1;
    u8 is_reject = (ctx->status >> REJECT_BIT) & 1;

    // a fill on our resting value order: the crash reached our floor (or a spike our
    // ceiling). accumulate/distribute. a resting order can fill long after we stopped
    // waiting on its ack, so credit it whether or not it is the pending message
    if (t9->order_id != MAX_U32 && ctx->order_id == t9->order_id && is_fill) {
        if (t9->order_is_buy) {
            t9->inventory += (i64)ctx->quantity_filled;
            t9->cash_guess -= (i64)ctx->quantity_filled * (i64)ctx->price;
        } else {
            t9->inventory -= (i64)ctx->quantity_filled;
            t9->cash_guess += (i64)ctx->quantity_filled * (i64)ctx->price;
        }
        if (!is_partial)
            t9->order_id = MAX_U32; // fully filled, nothing rests under this id now
    }

    if (t9->pending && ctx->order_id == t9->pending_id) {
        u8 kind = t9->pending_kind;
        t9->pending = 0;
        t9->pending_kind = T9_PEND_NONE;
        t9->pending_id = MAX_U32;

        if (kind == T9_PEND_QUOTE) {
            if (is_reject) {
                t9->order_id = MAX_U32;
                return 1;
            }
            // it rested (or partially filled and rested). remember it so we can refresh it
            if (!(is_fill && !is_partial)) {
                t9->order_id = ctx->order_id;
                t9->order_price = ctx->price;
                t9->order_is_buy = t9->pending_is_buy;
            }
        } else if (kind == T9_PEND_CANCEL) {
            t9->order_id = MAX_U32;
        } else if (is_reject) {
            return 1;
        }
    }

    return 0;
}

// post a fresh value order, or replace the resting one if it is on the wrong side or at the
// wrong price. a plain limit, GTC (default TIF): it rests patiently and survives the close,
// which is what "held for years" means. if the level is already through the market it crosses
// and accumulates immediately - which is exactly the aggression an arrest wants during a crash
static u8 t9_send_quote(T9* t9, Context* ctx, Order* out, u8 is_buy, u16 price, u16 qty) {
    u8 replacing = t9->order_id != MAX_U32;
    out->status = (is_buy ? (1 << BUY_DIRECTION_BIT) : 0);
    if (replacing) {
        out->status |= (1 << CAN_REP_BIT);
        out->other_id = t9->order_id;
    }
    out->price = price;
    out->quantity = qty;
    return t9_await(t9, ctx, T9_PEND_QUOTE, is_buy);
}

static u8 t9_send_cancel(T9* t9, Context* ctx, Order* out) {
    out->status = (1 << CANCEL_BIT) | (t9->order_is_buy ? (1 << BUY_DIRECTION_BIT) : 0);
    out->other_id = t9->order_id;
    out->price = t9->order_price;
    return t9_await(t9, ctx, T9_PEND_CANCEL, t9->order_is_buy);
}

u8 t9_on_snapshot(T9* t9, Context* ctx) {
    Order* out = ctx->next_order_ptr;

    if (t9_settle(t9, ctx))
        return t9_sleep(ctx, t9->p.reject_backoff_ns);

    if (t9->pending)
        return t9_sleep(ctx, t9->p.refresh_ns);

    if (!ctx->is_open)
        return t9_sleep(ctx, t9->p.retry_wake_ns);

    u32 fv = t9_fair_value(t9, ctx);
    u32 ref = ctx->mark ? ctx->mark : t9->p.seed_price;

    // the value band around fair value: buy below it, sell above it, sleep inside it
    u32 buy_level  = fv * (100u - t9->p.value_band_pct) / 100u;
    u32 sell_level = fv * (100u + t9->p.value_band_pct) / 100u;
    if (buy_level < 1)
        buy_level = 1;
    if (sell_level > MAX_U16)
        sell_level = MAX_U16;

    // decide the single side we want standing right now.
    //
    // THE BID IS UNCONDITIONAL ON WHERE PRICE IS NOW, and that is the whole point. it used
    // to be posted only once ref had already fallen to buy_level, and pulled the moment it
    // hadn't - which inverts the mechanism completely. a floor that appears in response to
    // a price is not a floor, it is a late reaction: on day 8 the session opened and ran
    // 90 -> 34 in under five seconds while every Oracle sat with reserved_cash = 0 and
    // nothing in the book, then woke half an hour later and posted into the wreckage.
    //
    // a real value investor's bid is a STANDING bid, parked below the market for as long
    // as it takes, precisely so that it is already there when something falls into it.
    // resting it costs nothing but reserved cash, and this tier has more of that than any
    // other. if price is already BELOW buy_level the same order is marketable and buys
    // aggressively - which is not a bug, it is the arrest: everything under the band is a
    // bargain by this tier's own reckoning, and position_limit is what bounds it.
    //
    // the ask still needs a real reason, because it needs INVENTORY - it can only ever
    // sell what it actually holds, so it takes priority over the bid when both apply
    u8  want_active = 0, want_buy = 0;
    u16 want_price = 0;
    if (ref >= sell_level && t9->inventory > 0) {
        want_active = 1; want_buy = 0; want_price = (u16)sell_level;
    } else if (t9->inventory < (i64)t9->p.position_limit) {
        want_active = 1; want_buy = 1; want_price = (u16)buy_level;
    }

    // reconcile the one resting order toward what we want. one action per wake; the pending
    // guard keeps a single message in flight
    if (!want_active) {
        // the only way to want nothing now: full at position_limit with price not yet dear
        // enough to distribute into. that is the one case where there is genuinely no order
        // to rest - we are out of capacity to buy and have no reason to sell
        if (t9->order_id != MAX_U32)
            return t9_send_cancel(t9, ctx, out);
        return t9_sleep(ctx, t9->p.refresh_ns);
    }

    // an order rests on the wrong side: cancel it before posting the side we want, so we are
    // never quoting a bid and an ask at once from one slot
    if (t9->order_id != MAX_U32 && t9->order_is_buy != want_buy)
        return t9_send_cancel(t9, ctx, out);

    // already resting the right side at the right price: hold, just look again later
    if (t9->order_id != MAX_U32 && t9->order_is_buy == want_buy && t9->order_price == want_price)
        return t9_sleep(ctx, t9->p.refresh_ns);

    // size: top up toward the position limit on the buy side; sell down what we hold
    u16 qty = t9->p.chunk_size;
    if (want_buy) {
        i64 room = (i64)t9->p.position_limit - t9->inventory;
        if (room < (i64)qty) qty = (u16)(room < 0 ? 0 : room);
    } else {
        if (t9->inventory < (i64)qty) qty = (u16)t9->inventory;
    }
    if (qty == 0)
        return t9_sleep(ctx, t9->p.refresh_ns);

    return t9_send_quote(t9, ctx, out, want_buy, want_price, qty);
}

void t9_get_settings(T9* t9, ClientSettings* client_settings) {
    client_settings->initial_wake    = t9->p.initial_wake;
    client_settings->processing_time = t9->p.processing_time;
    client_settings->net_latency     = t9->p.net_latency;

    // cash account: it accumulates longs and distributes them, never shorts. deep capital
    client_settings->is_cash_account = 1;
    client_settings->cash            = t9->p.cash;
    client_settings->reserved_cash   = 0;
    client_settings->shares          = 0;
    client_settings->reserved_shares = 0;

    // DATA_NONE: no book, no tape. it needs only the last price and the news signal, both on
    // the Context - so the free tier, which takes no stream and pays no data fee
    client_settings->sub_tier = TIER_FREE;
    client_settings->noii     = 0;
}

void t9_free(T9* t9) {
    free(t9);
}
