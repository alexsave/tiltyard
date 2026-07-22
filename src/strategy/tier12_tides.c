#include <stdlib.h>
#include <stdio.h>

#include "strategy/tier12_tides.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"
#include "server.h"
#include "response.h"

// T12 - Tides. calendar-driven index flow, most of it into the closing cross.
//
// the sim runs 0 for now. the real world has dozens of managers and hundreds-to-thousands
// of individual funds rebalancing on schedules; the doc puts the tier at 5-50 in sim.

// fund / manager names, loosely parodied. one per instance, handed out in init order
static const char* T12_NAMES[] = {
    "vanguardian.total",  "blackstone.ishares", "statestrand.spdr",  "fidelius.index",
    "invescape.qqq",      "charles.schwabb.idx","dimensionful.fund", "wisdomtreeish",
    "first.trusty.etf",   "globalx.thematic",   "vaneckt.sector",    "prosheres.levered",
};
static const u32 T12_NAME_COUNT = sizeof(T12_NAMES) / sizeof(T12_NAMES[0]);
static u32 t12_next_name = 0;

// the tunable block. EVERY value here is a placeholder - none of it is calibrated, and
// none of it should be tuned to make the tape "look right". that is the owner's job.
// a function rather than file scope so it can use the time constants: a const u64 is not
// a constant expression in C, so S_TO_NS cannot seed a static
static T12Params t12_defaults() {
    T12Params p;

    p.aum_min                   = 5000000000LL;  /* UNCALIBRATED */
    p.aum_max                   = 80000000000LL; /* UNCALIBRATED */

    p.index_weight_bp           = 8;   /* UNCALIBRATED */
    // the band. wide enough that a cap-weighted tracker mostly does nothing, which is the
    // honest behaviour - see the header
    p.tracking_error_bp         = 3;   /* UNCALIBRATED */

    // NAV is struck at the closing price, so most of it has to print in the cross
    p.moc_fraction_pct          = 80;  /* UNCALIBRATED */

    // the daily baseline. small against the position, but it happens EVERY session, which
    // is what a closing auction is mostly made of
    p.daily_flow_bp             = 150;  /* UNCALIBRATED */ // 1.5% of the position per day
    p.inflow_bias_pct           = 55;   /* UNCALIBRATED */ // mild net creation, per decades of it

    p.rebalance_period_ns       = 1 * DAY_TO_NS;  /* UNCALIBRATED */ // creation/redemption
    p.reconstitution_period_ns  = 90 * DAY_TO_NS; /* UNCALIBRATED */ // quarterly
    p.reconstitution_multiple   = 20;  /* UNCALIBRATED */

    // into the accumulation window, so it prints in the cross rather than ahead of it
    p.moc_submit_before_close_ns = 5 * MIN_TO_NS; /* UNCALIBRATED */

    p.slice_interval_ns         = 10 * MIN_TO_NS; /* UNCALIBRATED */
    p.impact_aversion_pct       = 5;    /* UNCALIBRATED */
    p.max_slice                 = 1000; /* UNCALIBRATED */
    p.min_slice                 = 100;  /* UNCALIBRATED */

    p.seed_price                = 100;  /* UNCALIBRATED */

    p.retry_wake_ns             = 1 * H_TO_NS;  /* UNCALIBRATED */
    p.reject_backoff_ns         = 15 * MIN_TO_NS; /* UNCALIBRATED */

    // not latency sensitive, but intensely TIMING sensitive - it has to hit the close
    p.processing_time           = 1 * S_TO_NS; /* UNCALIBRATED */
    p.net_latency               = 5 * (S_TO_NS / 1000); /* UNCALIBRATED */ // 5ms
    p.initial_wake              = 14 * H_TO_NS; /* UNCALIBRATED */
    p.initial_wake_spread_ns = 1 * H_TO_NS; /* UNCALIBRATED */

    return p;
}

// xorshift, per agent. determinism is a hard rule: same seed in, same sequence out
static u32 t12_rand(T12* t12) {
    u32 x = t12->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t12->rng = x;
    return x;
}

// PER-AGENT SKEW ON THE CADENCE PARAMS, drawn once at init and then fixed.
//
// unlike the casual tiers, this one genuinely does have a cadence - the fix is not to
// remove the clock, it is to stop every agent sharing the same one. N instances holding an
// identical timer all fire on the same tick, which makes them one agent N times larger
// rather than N agents. that is what put a 7x spike in minute :00 and a bar on the chart
// every 30 minutes: not real market structure, just arithmetic on identical constants
static u64 t12_skew(T12* t12, u64 base) {
    if (base == 0)
        return 0;
    // uniform on roughly [75%, 125%] of the tier value
    return base * 3 / 4 + (u64)(t12_rand(t12) % 1001) * (base / 2000);
}

static i64 t12_target_shares(T12* t12, u16 price) {
    if (price == 0)
        return 0;
    return (t12->aum * (i64)t12->index_weight / 10000) / (i64)price;
}

T12* t12_init() {
    T12* t12 = malloc(sizeof(T12));

    t12->p = t12_defaults();

    t12->parent_remaining = 0;
    t12->parent_is_buy = 0;
    t12->moc_remaining = 0;
    t12->moc_is_buy = 0;
    t12->moc_sent_today = 0;
    t12->last_rebalance_ns = 0;
    t12->last_reconstitution_ns = 0;
    t12->slice_id = MAX_U32;
    t12->slice_price = 0;
    t12->slice_since_ns = 0;
    t12->pending = 0;
    t12->pending_kind = T12_PEND_NONE;
    t12->pending_id = MAX_U32;

    // names go out in init order, so the roster is stable across runs
    t12->name_idx = t12_next_name % T12_NAME_COUNT;
    // each agent carries its own rng state, seeded off its slot. no shared global rng
    t12->rng = 0x45d9f3b3u * (t12_next_name + 1);

    // its own boot phase, so the tier does not start life as one agent
    t12->first_wake_ns = t12->p.initial_wake
                      + (u64)(t12_rand(t12) % 1000) * (t12->p.initial_wake_spread_ns / 1000);

    // no two of them run on the same clock
    t12->p.slice_interval_ns = t12_skew(t12, t12->p.slice_interval_ns);

    i64 span = t12->p.aum_max - t12->p.aum_min;
    t12->aum = t12->p.aum_min + (i64)(t12_rand(t12) % (u32)(span / 1000 + 1)) * 1000;
    t12->index_weight = t12->p.index_weight_bp + (u16)(t12_rand(t12) % 3);

    // seeded AT the index weight. a fund that started flat would have to buy its entire
    // tracking position on day one, which is a creation event, not a rebalance
    t12->inventory = t12_target_shares(t12, t12->p.seed_price);
    t12->cash_guess = t12->aum;

    t12_next_name++;

    return t12;
}

char* t12_get_name(T12* t12) {
    return (char*)T12_NAMES[t12->name_idx];
}

static u8 t12_sleep(T12* t12, Context* ctx, u64 delay) {
    (void)t12;
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

static u8 t12_await(T12* t12, Context* ctx, u8 kind) {
    t12->pending = 1;
    t12->pending_kind = kind;
    t12->pending_id = ctx->next_order_id;
    return 1;
}

static u8 t12_settle(T12* t12, Context* ctx) {
    u8 is_fill    = (ctx->status >> FILL_BIT) & 1;
    u8 is_reject  = (ctx->status >> REJECT_BIT) & 1;
    u8 is_partial = (ctx->status >> PARTIAL_FILL_BIT) & 1;
    u16 px = ctx->price ? ctx->price : ctx->mark;

    // a resting slice filling later, under its own id
    if (t12->slice_id != MAX_U32 && ctx->order_id == t12->slice_id && is_fill) {
        i64 q = (i64)ctx->quantity_filled;
        if (t12->parent_is_buy) {
            t12->inventory += q;
            t12->cash_guess -= q * (i64)px;
        } else {
            t12->inventory -= q;
            t12->cash_guess += q * (i64)px;
        }
        t12->parent_remaining -= q;
        if (t12->parent_remaining < 0)
            t12->parent_remaining = 0;
        if (!is_partial)
            t12->slice_id = MAX_U32;
    }

    if (t12->pending && ctx->order_id == t12->pending_id) {
        u8 kind = t12->pending_kind;
        t12->pending = 0;
        t12->pending_kind = T12_PEND_NONE;
        t12->pending_id = MAX_U32;

        if (kind == T12_PEND_MOC) {
            // an auction-only order either crosses at the cross or is cancelled - there is
            // never a residual to chase, which is the whole point of the bit
            if (is_fill && ctx->quantity_filled) {
                i64 q = (i64)ctx->quantity_filled;
                if (t12->moc_is_buy) {
                    t12->inventory += q;
                    t12->cash_guess -= q * (i64)px;
                } else {
                    t12->inventory -= q;
                    t12->cash_guess += q * (i64)px;
                }
            }
            t12->moc_remaining = 0;
            if (is_reject)
                return 1;
        } else if (kind == T12_PEND_SLICE) {
            if (is_reject)
                t12->slice_id = MAX_U32;
            else if (!is_fill || is_partial)
                t12->slice_id = ctx->order_id;
            if (is_reject)
                return 1;
        } else if (kind == T12_PEND_CANCEL) {
            t12->slice_id = MAX_U32;
            if (is_reject)
                return 1;
        }
    }

    return 0;
}

// the calendar decides, not a view. routine creation/redemption drift most days, and a
// reconstitution periodically that is a different order of magnitude
static void t12_check_calendar(T12* t12, Context* ctx) {
    if (ctx->mark == 0 || t12->aum <= 0)
        return;

    u8 reconstitution = t12->p.reconstitution_period_ns &&
        (ctx->real_time_ns - t12->last_reconstitution_ns) >= t12->p.reconstitution_period_ns;
    u8 routine = (ctx->real_time_ns - t12->last_rebalance_ns) >= t12->p.rebalance_period_ns;

    if (!reconstitution && !routine)
        return;

    i64 value = t12->inventory * (i64)ctx->mark;
    i64 weight_bp = value * 10000 / t12->aum;
    i64 drift = weight_bp - (i64)t12->index_weight;
    if (drift < 0)
        drift = -drift;

    i64 delta = 0;
    if (drift >= (i64)t12->p.tracking_error_bp)
        delta = t12_target_shares(t12, ctx->mark) - t12->inventory;

    // DAILY CREATION/REDEMPTION, on top of and independent of any drift correction. this
    // is the flow that happens every single session - the fund taking money in or paying
    // it out - and gating it on tracking error was the bug: with price sitting quietly at
    // fundamental a cap-weighted holding never drifts, so the tier sent 15 MOCs in 40 days
    // and the closing auction had essentially nothing to cross
    if (routine && t12->inventory > 0) {
        i64 flow = t12->inventory * (i64)t12->p.daily_flow_bp / 10000;
        if (flow > 0) {
            u8 creation = (t12_rand(t12) % 100) < t12->p.inflow_bias_pct;
            delta += creation ? flow : -flow;
        }
    }

    if (reconstitution) {
        // the quarterly event: the index itself changed, so this is a real position change
        // rather than a drift correction, and it is much larger
        t12->last_reconstitution_ns = ctx->real_time_ns;
        i64 base = t12_target_shares(t12, ctx->mark) / 100;
        i64 recon = base * (i64)t12->p.reconstitution_multiple;
        // direction of a reconstitution is the index committee's business, not price's
        delta += (t12_rand(t12) & 1) ? recon : -recon;
    }
    t12->last_rebalance_ns = ctx->real_time_ns;

    if (delta == 0)
        return;

    u8 is_buy = delta > 0;
    i64 total = delta > 0 ? delta : -delta;
    if (!is_buy && total > t12->inventory)
        total = t12->inventory;
    if (total <= 0)
        return;

    // NAV matching: most of it goes into the cross, the remainder is worked continuously
    i64 moc = total * (i64)t12->p.moc_fraction_pct / 100;
    t12->moc_remaining = moc;
    t12->moc_is_buy = is_buy;
    t12->parent_remaining = total - moc;
    t12->parent_is_buy = is_buy;
}

// auction-only interest into the closing cross. AUCTION_ONLY_BIT is the instruction that
// makes this a real MOC rather than a large limit order that happens to be late: cross or
// cancel, and never leak the residual into continuous trading
static u8 t12_send_moc(T12* t12, Context* ctx, Order* out) {
    i64 want = t12->moc_remaining;
    if (want > MAX_U16)
        want = MAX_U16;

    // MARKET-on-close, and the "market" half is not decoration - it is the difference
    // between crossing and never crossing. auction_walk sorts parked limits into the price
    // heaps and lets them cross only if the clearing price comes out on the right side of
    // them, but market orders "have no price, so they ride under the whole curve as base
    // demand/supply" - they take whatever the cross prints.
    //
    // this used to be a limit at ctx->mark, which is a contradiction of the comment that
    // was sitting right here: an index fund's NAV is struck AT the closing price, so any
    // price it names is a price it might miss the close over, and missing the close is the
    // one outcome its mandate cannot tolerate. over 40 days it sent 15 of these and crossed
    // exactly zero - they were parked, evaluated against a clearing price that never came
    // back in their favour, and cancelled at the bell every single session
    out->status = (1 << AUCTION_ONLY_BIT) | (1 << IS_MARKET_BIT) |
                  (t12->moc_is_buy ? (1 << BUY_DIRECTION_BIT) : 0);
    out->price = 0;
    out->quantity = (u16)want;

    t12->moc_sent_today = 1;
    return t12_await(t12, ctx, T12_PEND_MOC);
}

static u8 t12_send_slice(T12* t12, Context* ctx, Order* out) {
    i64 want = t12->parent_remaining * (i64)t12->p.impact_aversion_pct / 100;
    if (want < (i64)t12->p.min_slice)
        want = t12->p.min_slice;
    if (want > (i64)t12->p.max_slice)
        want = t12->p.max_slice;
    if (want > t12->parent_remaining)
        want = t12->parent_remaining;
    if (want <= 0) {
        t12->parent_remaining = 0;
        return t12_sleep(t12, ctx, t12->p.slice_interval_ns);
    }

    out->status = (1 << DAY_BIT) | (t12->parent_is_buy ? (1 << BUY_DIRECTION_BIT) : 0);
    out->price = ctx->mark;
    out->quantity = (u16)want;

    t12->slice_price = ctx->mark;
    t12->slice_since_ns = ctx->real_time_ns;
    return t12_await(t12, ctx, T12_PEND_SLICE);
}

static u8 t12_send_cancel(T12* t12, Context* ctx, Order* out) {
    out->status = (1 << CANCEL_BIT);
    out->other_id = t12->slice_id;
    return t12_await(t12, ctx, T12_PEND_CANCEL);
}

u8 t12_on_snapshot(T12* t12, Context* ctx) {
    Order* out = ctx->next_order_ptr;

    u8 settled = t12_settle(t12, ctx);
    if (settled == 1)
        return t12_sleep(t12, ctx, t12->p.reject_backoff_ns);

    if (t12->pending)
        return t12_sleep(t12, ctx, t12->p.slice_interval_ns);

    // the accumulation window is the one moment this tier genuinely cares about. an
    // auction-only order is accepted while the auction is being built, so this is where
    // the MOC goes - and it must go in before the freeze ends cancels and entries
    if (ctx->auctioning && !ctx->auction_frozen && t12->moc_remaining > 0 &&
        !t12->moc_sent_today && ctx->mark != 0)
        return t12_send_moc(t12, ctx, out);

    if (!ctx->is_open) {
        // outside the session the day's MOC is done with; a new one is decided tomorrow
        t12->moc_sent_today = 0;
        return t12_sleep(t12, ctx, t12->p.retry_wake_ns);
    }

    if (ctx->mark == 0)
        return t12_sleep(t12, ctx, t12->p.retry_wake_ns);

    if (t12->parent_remaining <= 0 && t12->moc_remaining <= 0) {
        t12_check_calendar(t12, ctx);
        if (t12->parent_remaining <= 0 && t12->moc_remaining <= 0)
            return t12_sleep(t12, ctx, t12->p.slice_interval_ns);
    }

    if (t12->slice_id != MAX_U32) {
        u8 stale = ctx->mark != t12->slice_price;
        if (stale)
            return t12_send_cancel(t12, ctx, out);
        return t12_sleep(t12, ctx, t12->p.slice_interval_ns);
    }

    if (t12->parent_remaining > 0)
        return t12_send_slice(t12, ctx, out);

    return t12_sleep(t12, ctx, t12->p.slice_interval_ns);
}

void t12_get_settings(T12* t12, ClientSettings* client_settings) {
    client_settings->initial_wake    = t12->first_wake_ns;
    client_settings->processing_time = t12->p.processing_time;
    client_settings->net_latency     = t12->p.net_latency;

    // cash account. a long-only index fund holds the index, it does not short it
    client_settings->is_cash_account = 1;
    client_settings->cash            = t12->aum;
    client_settings->reserved_cash   = 0;
    client_settings->shares          = (u32)t12->inventory;
    client_settings->reserved_shares = 0;

    // index composition and a closing price. no need for depth - it is not choosing when
    // to trade, the calendar already did
    client_settings->sub_tier = TIER_FREE;
    client_settings->noii     = 0;
}

void t12_free(T12* t12) {
    free(t12);
}
