#include <stdlib.h>
#include <stdio.h>

#include "strategy/tier13_glaciers.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"
#include "server.h"
#include "response.h"

// T13 - Glaciers. target-weight institutional money, working enormous parent orders on a
// committee's clock. the ceiling, and on the way down the deepest part of the floor.
//
// the sim runs 20. US public pensions number ~5,000+, SWFs ~100, large endowments in the
// hundreds; the doc puts the tier at 10s-100s in sim. 20 funds is enough that they
// rebalance on staggered calendars rather than as one block.

// institutional names, loosely parodied. one per instance, handed out in init order
static const char* T13_NAMES[] = {
    "calpurse",              // the big public pensions
    "teachers.retirement.tx",
    "ontario.teachers.plan",
    "florida.state.board",
    "nyc.employees.fund",
    "norse.oil.fund",        // the sovereigns
    "gulf.sovereign.auth",
    "temasekh.holdings",
    "china.investment.corp2",
    "alaska.permanent.trust",
    "harvest.endowment",     // and the endowments
    "yalie.investments",
    "stanfjord.management",
    "princetown.endowment",
    "mit.investment.co",
    "wellcome.charitable",
    "gates.foundation.trust",
    "dutch.pension.abp",
    "japan.gpif.trust",
    "swiss.federal.pension",
};
static const u32 T13_NAME_COUNT = sizeof(T13_NAMES) / sizeof(T13_NAMES[0]);
static u32 t13_next_name = 0;

// the tunable block. EVERY value here is a placeholder - none of it is calibrated, and
// none of it should be tuned to make the tape "look right". that is the owner's job.
// a function rather than file scope so it can use the time constants: a const u64 is not
// a constant expression in C, so S_TO_NS cannot seed a static
static T13Params t13_defaults() {
    T13Params p;

    // the largest pool in the sim by a wide margin
    p.aum_min                = 20000000000LL;  /* UNCALIBRATED */
    p.aum_max                = 200000000000LL; /* UNCALIBRATED */

    // a single equity is a small slice of a diversified institutional portfolio
    p.target_weight_bp       = 5;    /* UNCALIBRATED */ // 0.05% of AUM
    // the tracking band. a committee does not act on noise - it acts when the weight has
    // genuinely drifted, which for a real mandate is a large move
    p.rebalance_threshold_bp = 2;    /* UNCALIBRATED */

    // worked patiently over weeks. impact aversion is this horizon, not a clever algo
    p.execution_horizon_ns   = 10 * DAY_TO_NS; /* UNCALIBRATED */
    p.slice_interval_ns      = 20 * MIN_TO_NS; /* UNCALIBRATED */
    p.impact_aversion_pct    = 2;    /* UNCALIBRATED */ // show 2% of what is left
    p.max_slice              = 2000; /* UNCALIBRATED */
    p.min_slice              = 100;  /* UNCALIBRATED */

    // WAKE_CALENDAR. this is the CHECK cadence, not the trade cadence - the two are different things and
    // conflating them is what made this tier inert. a real institution monitors its weights
    // continuously and acts when the band breaks; the committee calendar governs the
    // REVIEW. checking is free (it is one multiply against the mark), so a slow check
    // cadence buys nothing and costs everything: at a 30-day check in a 40-day sim each
    // fund got exactly one look at the market and slept through the rest of the run
    p.rebalance_frequency_ns = 1 * DAY_TO_NS;  /* UNCALIBRATED */
    p.slice_patience_ns      = 60 * MIN_TO_NS; /* UNCALIBRATED */

    p.seed_price             = 100;  /* UNCALIBRATED */

    p.retry_wake_ns          = 6 * H_TO_NS;  /* UNCALIBRATED */
    p.reject_backoff_ns      = 30 * MIN_TO_NS; /* UNCALIBRATED */

    // committee-slow, and utterly latency-insensitive
    p.processing_time        = 5 * S_TO_NS;  /* UNCALIBRATED */
    p.net_latency            = 10 * (S_TO_NS / 1000); /* UNCALIBRATED */ // 10ms
    p.initial_wake           = 14 * H_TO_NS; /* UNCALIBRATED */

    return p;
}

// xorshift, per agent. determinism is a hard rule: same seed in, same sequence out
static u32 t13_rand(T13* t13) {
    u32 x = t13->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t13->rng = x;
    return x;
}

// the mandate in shares at a given price: what the fund is SUPPOSED to hold right now
static i64 t13_target_shares(T13* t13, u16 price) {
    if (price == 0)
        return 0;
    i64 target_value = t13->aum * (i64)t13->target_weight / 10000;
    return target_value / (i64)price;
}

T13* t13_init() {
    T13* t13 = malloc(sizeof(T13));

    t13->p = t13_defaults();

    t13->parent_remaining = 0;
    t13->parent_is_buy = 0;
    t13->work_deadline_ns = 0;
    t13->slice_id = MAX_U32;
    t13->slice_price = 0;
    t13->slice_since_ns = 0;
    t13->pending = 0;
    t13->pending_kind = T13_PEND_NONE;
    t13->pending_id = MAX_U32;

    // names go out in init order, so the roster is stable across runs
    t13->name_idx = t13_next_name % T13_NAME_COUNT;
    // each agent carries its own rng state, seeded off its slot. no shared global rng
    t13->rng = 0x27d4eb2fu * (t13_next_name + 1);

    i64 span = t13->p.aum_max - t13->p.aum_min;
    t13->aum = t13->p.aum_min + (i64)(t13_rand(t13) % (u32)(span / 1000 + 1)) * 1000;

    // a per-fund tilt on the mandate. twenty funds with an identical target weight would
    // all breach the same band on the same print and rebalance as one block, which is a
    // single enormous participant wearing twenty hats
    u16 tilt = (u16)(t13_rand(t13) % 3); // 0..2 bp either side
    t13->target_weight = t13->p.target_weight_bp + tilt;

    // SEEDED AT TARGET. a fund that started flat would have to buy its whole mandate on
    // day one, and twenty of those arriving at once is the fat-finger scenario, not a
    // pension. it begins where its mandate says it belongs and trades only when price
    // moves it off that
    t13->inventory = t13_target_shares(t13, t13->p.seed_price);
    t13->cash_guess = t13->aum;

    t13_next_name++;

    return t13;
}

char* t13_get_name(T13* t13) {
    return (char*)T13_NAMES[t13->name_idx];
}

// WAKE_CALENDAR - action bit 2. the engine keeps only the earliest wake in flight
static u8 t13_sleep(T13* t13, Context* ctx, u64 base) {
    // stagger, so twenty committees do not all sit on the same day
    u64 jitter = base ? (u64)(t13_rand(t13) % 1000) * (base / 1000) / 4 : 0;
    u64 delay = base + jitter;

    // NEVER DEFER TO A PENDING WAKE - CLAMP INSIDE IT INSTEAD.
    //
    // the engine keeps only the earliest wake per client and silently drops any request at
    // or after the one it already holds, so "something sooner is already coming, ask for
    // nothing" looks like the polite thing to do. it is a trap. a wake can be delivered a
    // hair BEFORE its own fire_at, and the engine only clears next_wake_ns when the wake
    // lands at or after it - so the client is handed a next_wake_ns advertising a wake that
    // has already been spent. defer to that and nothing ever arrives; and because every
    // re-arm is now compared against the same stale value, every re-arm is dropped too.
    //
    // a tier with a live feed never notices - the tape wakes it regardless. this tier has
    // DATA_NONE and no resting-order traffic to speak of, so it simply stops existing,
    // mid-rebalance, holding the stock it was in the middle of selling. that is exactly how
    // 20 funds went dark on day 0 and the market melted up unopposed for the next 39 days.
    //
    // asking for the longest delay the engine will still accept is safe either way: if the
    // pending wake is real we wake a moment early and decide again, and if it was already
    // spent this is the only thing that gets us moving again
    if (ctx->real_time_ns + delay >= ctx->next_wake_ns) {
        if (ctx->next_wake_ns <= ctx->real_time_ns + 1)
            return 0; // pending wake is already in the past - nothing would be accepted
        delay = ctx->next_wake_ns - ctx->real_time_ns - 1;
    }

    ctx->wake_delay_ns = delay;
    return 2;
}

static u8 t13_await(T13* t13, Context* ctx, u8 kind) {
    t13->pending = 1;
    t13->pending_kind = kind;
    t13->pending_id = ctx->next_order_id;
    return 1;
}

// fold a response into our own books and drive the parent down
static u8 t13_settle(T13* t13, Context* ctx) {
    u8 is_fill    = (ctx->status >> FILL_BIT) & 1;
    u8 is_reject  = (ctx->status >> REJECT_BIT) & 1;
    u8 is_partial = (ctx->status >> PARTIAL_FILL_BIT) & 1;

    // a resting slice can fill long after its ack, and that fill arrives under the SLICE's
    // id rather than under anything we are pending on - so it is handled on its own
    if (t13->slice_id != MAX_U32 && ctx->order_id == t13->slice_id && is_fill) {
        i64 q = (i64)ctx->quantity_filled;
        u16 px = ctx->price ? ctx->price : ctx->mark;
        if (t13->parent_is_buy) {
            t13->inventory += q;
            t13->cash_guess -= q * (i64)px;
        } else {
            t13->inventory -= q;
            t13->cash_guess += q * (i64)px;
        }
        t13->parent_remaining -= q;
        if (t13->parent_remaining < 0)
            t13->parent_remaining = 0;
        // fully filled means nothing of it is left resting
        if (!is_partial)
            t13->slice_id = MAX_U32;
    }

    if (t13->pending && ctx->order_id == t13->pending_id) {
        u8 kind = t13->pending_kind;
        t13->pending = 0;
        t13->pending_kind = T13_PEND_NONE;
        t13->pending_id = MAX_U32;

        if (kind == T13_PEND_SLICE) {
            // whatever did not fill on arrival is now resting and needs its id kept so we
            // can reprice it later. one that filled outright rests nothing
            if (is_reject)
                t13->slice_id = MAX_U32;
            else if (!is_fill || is_partial)
                t13->slice_id = ctx->order_id;
            if (is_reject)
                return 1;
        } else if (kind == T13_PEND_CANCEL) {
            t13->slice_id = MAX_U32;
            if (is_reject)
                return 1;
        }
    }

    return 0;
}

// look at the weight and decide whether the committee acts. THE WHOLE STRATEGY IS HERE,
// and there is no view of value in it anywhere: the fund holds a mandate, price moves the
// mandate out of tolerance, the fund trades back to it. selling more the higher price
// goes is not a judgement that price is wrong - it is arithmetic
static void t13_check_mandate(T13* t13, Context* ctx) {
    if (ctx->mark == 0 || t13->aum <= 0)
        return;

    i64 value = t13->inventory * (i64)ctx->mark;
    i64 weight_bp = value * 10000 / t13->aum;
    i64 drift = weight_bp - (i64)t13->target_weight;
    if (drift < 0)
        drift = -drift;

    if (drift < (i64)t13->p.rebalance_threshold_bp)
        return; // inside the tracking band. nothing to do, go back to sleep

    i64 want = t13_target_shares(t13, ctx->mark);
    i64 delta = want - t13->inventory;
    if (delta == 0)
        return;

    t13->parent_is_buy = delta > 0;
    t13->parent_remaining = delta > 0 ? delta : -delta;

    // a cash account cannot sell what it does not hold
    if (!t13->parent_is_buy && t13->parent_remaining > t13->inventory)
        t13->parent_remaining = t13->inventory;

    t13->work_deadline_ns = ctx->real_time_ns + t13->p.execution_horizon_ns;
}

// one slice of the parent, resting as a patient limit AT the last print. impact aversion
// is expressed as size and time, not as a clever price: show a sliver, leave it, reprice
// it when the tape walks away. resting rather than crossing is also the only reason this
// tier adds depth to the book instead of taking it
static u8 t13_send_slice(T13* t13, Context* ctx, Order* out) {
    i64 want = t13->parent_remaining * (i64)t13->p.impact_aversion_pct / 100;
    if (want < (i64)t13->p.min_slice)
        want = t13->p.min_slice;
    if (want > (i64)t13->p.max_slice)
        want = t13->p.max_slice;
    if (want > t13->parent_remaining)
        want = t13->parent_remaining;
    if (want <= 0)
        return t13_sleep(t13, ctx, t13->p.rebalance_frequency_ns);

    out->status = (1 << DAY_BIT) | (t13->parent_is_buy ? (1 << BUY_DIRECTION_BIT) : 0);
    out->price = ctx->mark;
    out->quantity = (u16)want;

    t13->slice_price = ctx->mark;
    t13->slice_since_ns = ctx->real_time_ns;
    return t13_await(t13, ctx, T13_PEND_SLICE);
}

static u8 t13_send_cancel(T13* t13, Context* ctx, Order* out) {
    out->status = (1 << CANCEL_BIT);
    out->other_id = t13->slice_id;
    return t13_await(t13, ctx, T13_PEND_CANCEL);
}

u8 t13_on_snapshot(T13* t13, Context* ctx) {
    Order* out = ctx->next_order_ptr;


    u8 settled = t13_settle(t13, ctx);
    if (settled == 1)
        return t13_sleep(t13, ctx, t13->p.reject_backoff_ns);

    if (t13->pending)
        return t13_sleep(t13, ctx, t13->p.slice_interval_ns);

    if (!ctx->is_open)
        return t13_sleep(t13, ctx, t13->p.retry_wake_ns);

    // not working anything: this is the committee wake. check the weight, and almost
    // always go straight back to sleep for another month
    if (t13->parent_remaining <= 0) {
        t13_check_mandate(t13, ctx);
        if (t13->parent_remaining <= 0)
            return t13_sleep(t13, ctx, t13->p.rebalance_frequency_ns);
    }

    // the horizon ran out. a fund does not chase its own order forever - it books what it
    // got, drops the rest, and picks the drift up again at the next committee
    if (ctx->real_time_ns >= t13->work_deadline_ns) {
        t13->parent_remaining = 0;
        if (t13->slice_id != MAX_U32)
            return t13_send_cancel(t13, ctx, out);
        return t13_sleep(t13, ctx, t13->p.rebalance_frequency_ns);
    }

    // a slice is resting. leave it alone unless the print has walked away from it, in
    // which case it is no longer a slice of this order at a price anyone will trade
    if (t13->slice_id != MAX_U32) {
        u8 stale = (ctx->real_time_ns - t13->slice_since_ns) >= t13->p.slice_patience_ns ||
                   (ctx->mark != 0 && ctx->mark != t13->slice_price);
        if (stale)
            return t13_send_cancel(t13, ctx, out);
        return t13_sleep(t13, ctx, t13->p.slice_interval_ns);
    }

    if (ctx->mark == 0)
        return t13_sleep(t13, ctx, t13->p.retry_wake_ns);

    return t13_send_slice(t13, ctx, out);
}

void t13_get_settings(T13* t13, ClientSettings* client_settings) {
    client_settings->initial_wake    = t13->p.initial_wake;
    client_settings->processing_time = t13->p.processing_time;
    client_settings->net_latency     = t13->p.net_latency;

    // cash account. a pension does not short - the entire ceiling this tier provides comes
    // from selling down a position it genuinely holds, which is exactly why it is a real
    // ceiling and not a bet
    client_settings->is_cash_account = 1;
    client_settings->cash            = t13->aum;
    client_settings->reserved_cash   = 0;
    // seeded AT the mandate, so it starts in balance rather than having to buy its way in
    client_settings->shares          = (u32)t13->inventory;
    client_settings->reserved_shares = 0;

    // DATA_NONE. it has no use for a feed - the last print on the Context is the only
    // price a monthly rebalance decision needs
    client_settings->sub_tier = TIER_FREE;
    client_settings->noii     = 0;
}

void t13_free(T13* t13) {
    free(t13);
}
