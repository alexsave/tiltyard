#include <stdlib.h>
#include <stdio.h>

#include "strategy/tier04_suits.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"
#include "mbp.h"
#include "server.h"
#include "response.h"

// T4 - Suits. read the signal, form a fair value, size a position by conviction, work it
// in toward that value, and hold. the informed flow.
//
// the sim runs 60. there are ~32k hedge funds worldwide but ~550 firms manage 86% of the
// capital, so the handful that move a name is in the low hundreds; the design doc puts the
// tier at 50-100 in sim. 60 informed funds against 8 makers is enough adverse selection to
// matter without instantly overwhelming the book on a catalyst.

// fund names, loosely parodied. one per instance, handed out in init order
static const char* T4_NAMES[] = {
    "bridgewater.assoc",   // the macro / multi-strat giants, GTA-style
    "renascence.tech",
    "citreon.global",
    "millbrook.capital",
    "point82",
    "two.sigma.echo",
    "ballyasny",
    "marshall.wace.ish",
    "davidson.kegner",
    "elliot.mgmt.ish",
    "third.point.five",
    "pershing.square.root",
    "tiger.cub.global",
    "coatue.adjacent",
    "lone.pinecone",
};
static const u32 T4_NAME_COUNT = sizeof(T4_NAMES) / sizeof(T4_NAMES[0]);
static u32 t4_next_name = 0;

// the tunable block. EVERY value here is a placeholder - none of it is calibrated, and
// none of it should be tuned to make the tape "look right". that is the owner's job.
// a function rather than file scope so it can use the time constants: a const u64 is not
// a constant expression in C, so S_TO_NS cannot seed a static
static T4Params t4_defaults() {
    T4Params p;

    // the doc's default is VIA_SLICER, but routing one client's order to another needs the
    // T11 layer, which does not exist. DIRECT is the only path that works today
    p.routing                   = T4_DIRECT;   /* UNCALIBRATED */

    // the same anchor a maker seeds around and T2 reads, so a neutral signal agrees with
    // where the book starts
    p.fundamental_anchor        = 100;    /* UNCALIBRATED */
    p.news_full_swing_ticks     = 8;      /* UNCALIBRATED */
    p.fair_value_bias_ticks     = 4;      /* UNCALIBRATED */

    p.conviction_per_tick       = 250;    /* UNCALIBRATED */
    p.position_limit            = 5000;   /* UNCALIBRATED */
    p.conviction_deadzone_ticks = 1;      /* UNCALIBRATED */

    p.child_size                = 300;    /* UNCALIBRATED */
    p.participation_cap_pct     = 40;     /* UNCALIBRATED */

    // a few looks a day. the session is 6.5h, so ~2h between looks is ~3 per session
    p.rebalance_interval_ns     = 2 * H_TO_NS; /* UNCALIBRATED */
    // a PM decides, then execution moves. seconds to minutes, and latency is irrelevant at
    // this cadence - this is the whole reason a suit is not a latency player
    p.decision_delay_ns         = 5 * S_TO_NS; /* UNCALIBRATED */

    p.seed_price                = 100;    /* UNCALIBRATED */

    p.retry_wake_ns             = 30 * MIN_TO_NS; /* UNCALIBRATED */
    p.reject_backoff_ns         = 1 * MIN_TO_NS;  /* UNCALIBRATED */

    p.aum                       = 5000000000; /* UNCALIBRATED */ // $5bn
    p.margin_mult               = 2;      /* UNCALIBRATED */
    p.maint_pct                 = 30;     /* UNCALIBRATED */

    p.processing_time           = 2 * S_TO_NS; /* UNCALIBRATED */
    p.net_latency               = 1 * S_TO_NS; /* UNCALIBRATED */

    p.initial_wake              = 13 * H_TO_NS; /* UNCALIBRATED */
    p.initial_wake_spread_ns = 1 * H_TO_NS; /* UNCALIBRATED */

    return p;
}

// xorshift, per agent. determinism is a hard rule: same seed in, same sequence out
static u32 t4_rand(T4* t4) {
    u32 x = t4->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t4->rng = x;
    return x;
}

// PER-AGENT SKEW ON THE CADENCE PARAMS, drawn once at init and then fixed.
//
// unlike the casual tiers, this one genuinely does have a cadence - the fix is not to
// remove the clock, it is to stop every agent sharing the same one. N instances holding an
// identical timer all fire on the same tick, which makes them one agent N times larger
// rather than N agents. that is what put a 7x spike in minute :00 and a bar on the chart
// every 30 minutes: not real market structure, just arithmetic on identical constants
static u64 t4_skew(T4* t4, u64 base) {
    if (base == 0)
        return 0;
    // uniform on roughly [75%, 125%] of the tier value
    return base * 3 / 4 + (u64)(t4_rand(t4) % 1001) * (base / 2000);
}

T4* t4_init() {
    T4* t4 = malloc(sizeof(T4));

    t4->p = t4_defaults();

    t4->have_book = 0;
    t4->book_have_bid = 0;
    t4->book_have_ask = 0;
    t4->last_bid = 0;
    t4->last_ask = 0;
    t4->last_bid_depth = 0;
    t4->last_ask_depth = 0;

    t4->pending = 0;
    t4->pending_kind = T4_PEND_NONE;
    t4->pending_buy = 0;
    t4->pending_id = MAX_U32;

    t4->inventory = 0;
    t4->cash_guess = t4->p.aum;

    // names go out in init order, so the roster is stable across runs
    t4->name_idx = t4_next_name % T4_NAME_COUNT;
    // each agent carries its own rng state, seeded off its slot. no shared global rng
    t4->rng = 0x27d4eb2fu * (t4_next_name + 1);

    // its own boot phase, so the tier does not start life as one agent
    t4->first_wake_ns = t4->p.initial_wake
                      + (u64)(t4_rand(t4) % 1000) * (t4->p.initial_wake_spread_ns / 1000);

    // no two of them run on the same clock
    t4->p.rebalance_interval_ns = t4_skew(t4, t4->p.rebalance_interval_ns);

    // this fund's persistent disagreement with the true fundamental, drawn once and fixed:
    // a symmetric offset in [-bias, +bias]. mean-zero across the population, so there is no
    // net drift baked in - only a spread of opinions
    u16 span = 2 * t4->p.fair_value_bias_ticks + 1;
    t4->fair_value_bias = (i32)(t4_rand(t4) % span) - (i32)t4->p.fair_value_bias_ticks;

    t4_next_name++;
    return t4;
}

char* t4_get_name(T4* t4) {
    return (char*)T4_NAMES[t4->name_idx];
}

// "wake me in n ns" - action bit 2. a suit is slow: this is its whole wake model, a look
// every rebalance_interval. the engine keeps only the earliest wake in flight
static u8 t4_sleep(Context* ctx, u64 delay) {
    if (ctx->real_time_ns + delay >= ctx->next_wake_ns)
        return 0;
    ctx->wake_delay_ns = delay;
    return 2;
}

// this fund's fair value in half ticks: the true fundamental from the signal, plus its own
// fixed bias. before any news the signal is neutral, so everyone sits near the anchor,
// spread only by their biases
static u32 t4_fair_value_half_ticks(T4* t4, Context* ctx) {
    i32 level = ctx->last_news_ns ? (i32)ctx->news_signal : 128; // neutral until news prints
    i32 offset = level - 128;

    i64 fv = 2 * (i64)t4->p.fundamental_anchor
           + ((i64)offset * 2 * (i64)t4->p.news_full_swing_ticks) / 128
           + 2 * (i64)t4->fair_value_bias;

    if (fv < 2)
        fv = 2; // price 0 is reserved, so fair value never drops below one tick
    return (u32)fv;
}

// maintain the local replica of the touch off this response. the snapshot carries whatever
// this client's sub_tier subscribes to, on an order ack the same as on a broadcast, so at
// TIER_MBP1 it is the BBO - the touch and its sizes, which is the whole data budget of this
// tier. we never set WS_BIT, so it only ever arrives on the ack to one of our own orders
static void t4_read_book(T4* t4, Context* ctx) {
    MBP1* bbo = (MBP1*)ctx->data_snapshot;
    if (!bbo)
        return;

    // price 0 is the engine's "no such level" - price 0 is reserved, so it is unambiguous
    u8 hb = bbo->hi_bid.price != 0;
    u8 ha = bbo->lo_ask.price != 0;

    if (hb) {
        t4->last_bid = bbo->hi_bid.price;
        t4->last_bid_depth = bbo->hi_bid.quantity;
    }
    if (ha) {
        t4->last_ask = bbo->lo_ask.price;
        t4->last_ask_depth = bbo->lo_ask.quantity;
    }

    t4->book_have_bid = hb;
    t4->book_have_ask = ha;
    t4->have_book = hb || ha;
}

// fold a fill into the position. a suit tracks its book by fills like everyone else
static u8 t4_settle(T4* t4, Context* ctx) {
    u8 is_fill   = (ctx->status >> FILL_BIT) & 1;
    u8 is_reject = (ctx->status >> REJECT_BIT) & 1;

    if (is_fill && ctx->quantity_filled) {
        if (t4->pending_buy) {
            t4->inventory += (i64)ctx->quantity_filled;
            t4->cash_guess -= (i64)ctx->quantity_filled * (i64)ctx->price;
        } else {
            t4->inventory -= (i64)ctx->quantity_filled;
            t4->cash_guess += (i64)ctx->quantity_filled * (i64)ctx->price;
        }
    }

    if (t4->pending && ctx->order_id == t4->pending_id) {
        u8 kind = t4->pending_kind;
        t4->pending = 0;
        t4->pending_kind = T4_PEND_NONE;
        t4->pending_id = MAX_U32;

        // a probe or a conviction IOC that found nothing is not an error - just a look that
        // did not trade. only a real reject (bad funding, closed, etc) needs a backoff
        if (is_reject && kind == T4_PEND_TRADE)
            return ctx->rej_reason == CXL_IOC_UNFILLED ? 0 : 1;
        if (is_reject)
            return 1;
    }

    return 0;
}

static u8 t4_await(T4* t4, Context* ctx, u8 kind, u8 buy) {
    t4->pending = 1;
    t4->pending_kind = kind;
    t4->pending_buy = buy;
    t4->pending_id = ctx->next_order_id;
    return 1;
}

// nothing seen yet: one tiny resting child gets a response, which carries the book. priced
// away from the touch so it does not accidentally trade before we have formed a view
static u8 t4_send_probe(T4* t4, Context* ctx, Order* out) {
    u16 ref = ctx->mark ? ctx->mark : t4->p.seed_price;
    // rest a buy well below / a sell well above, so it just sits and reports the book
    u8 buy = t4->inventory <= 0;
    u16 px = buy ? (ref > 4 ? ref - 4 : 1)
                 : (ref < MAX_U16 - 4 ? ref + 4 : MAX_U16);

    out->status = (1 << DAY_BIT) | (buy ? (1 << BUY_DIRECTION_BIT) : 0);
    out->price = px;
    out->quantity = 1; // minimal - this is a feeler, not a position
    return t4_await(t4, ctx, T4_PEND_PROBE, buy);
}

// the conviction position we want to be holding, given fair value and the current price.
// signed shares: positive long, negative short. capped at position_limit
static i64 t4_target_position(T4* t4, u32 fv_hy, u32 ref_hy) {
    // mispricing in whole ticks (half-tick math rounds toward zero, fine for sizing)
    i32 mis_ticks = ((i32)fv_hy - (i32)ref_hy) / 2;

    u32 dead = t4->p.conviction_deadzone_ticks;
    if ((u32)(mis_ticks < 0 ? -mis_ticks : mis_ticks) <= dead)
        return 0; // close enough to fair value - hold flat, do not chase noise

    i64 target = (i64)mis_ticks * (i64)t4->p.conviction_per_tick;
    if (target > t4->p.position_limit) target = t4->p.position_limit;
    if (target < -(i64)t4->p.position_limit) target = -(i64)t4->p.position_limit;
    return target;
}

// work one child toward the target. taker-leaning - we cross to build the position - but
// NEVER through our own fair value: we lift offers that are at or below what we think it is
// worth and stop there. that stop is the price-discovery force, and it is the same
// discipline that keeps every trading tier here from running the book away
static u8 t4_work_toward(T4* t4, Context* ctx, Order* out, i64 target, u32 fv_hy) {
    i64 delta = target - t4->inventory;
    u16 fv = (u16)(fv_hy / 2);

    u8 buy = delta > 0;
    u32 want = (u32)(buy ? delta : -delta);
    if (want > t4->p.child_size)
        want = t4->p.child_size;

    if (buy) {
        // need an ask at or below fair value to lift; otherwise the market is already past
        // where we would pay, so hold
        if (!t4->book_have_ask || t4->last_ask > fv)
            return t4_sleep(ctx, t4->p.rebalance_interval_ns);

        u32 cap = (t4->last_ask_depth * t4->p.participation_cap_pct) / 100;
        if (cap == 0) cap = 1;
        if (want > cap) want = cap;
        if (want == 0)
            return t4_sleep(ctx, t4->p.rebalance_interval_ns);

        out->status = (1 << IOC_BIT) | (1 << BUY_DIRECTION_BIT);
        out->price = t4->last_ask;  // marketable, capped at the ask (<= fair value)
        out->quantity = (u16)want;
        return t4_await(t4, ctx, T4_PEND_TRADE, 1);
    }

    // sell: need a bid at or above fair value to hit
    if (!t4->book_have_bid || t4->last_bid < fv)
        return t4_sleep(ctx, t4->p.rebalance_interval_ns);

    u32 cap = (t4->last_bid_depth * t4->p.participation_cap_pct) / 100;
    if (cap == 0) cap = 1;
    if (want > cap) want = cap;
    if (want == 0)
        return t4_sleep(ctx, t4->p.rebalance_interval_ns);

    out->status = (1 << IOC_BIT);
    out->price = t4->last_bid;
    out->quantity = (u16)want;
    return t4_await(t4, ctx, T4_PEND_TRADE, 0);
}

u8 t4_on_snapshot(T4* t4, Context* ctx) {
    Order* out = ctx->next_order_ptr;

    // VIA_SLICER needs the routing layer; without it there is nothing to emit a parent to.
    // gate it here rather than pretend, per the hard rules
    if (t4->p.routing == T4_VIA_SLICER)
        return 0;

    t4_read_book(t4, ctx);

    if (t4_settle(t4, ctx))
        return t4_sleep(ctx, t4->p.reject_backoff_ns);

    if (t4->pending)
        return t4_sleep(ctx, t4->p.rebalance_interval_ns);

    if (!ctx->is_open)
        return t4_sleep(ctx, t4->p.retry_wake_ns);

    // no book yet: one feeler to get one. the makers quote from the open, so this fills in
    // on the first response after the bell
    if (!t4->have_book)
        return t4_send_probe(t4, ctx, out);

    // reference price: the mid if we have both sides, else the last print, else the anchor
    u32 ref_hy;
    if (t4->book_have_bid && t4->book_have_ask)
        ref_hy = (u32)t4->last_bid + (u32)t4->last_ask; // (bid+ask)/2, in half ticks
    else if (ctx->mark)
        ref_hy = 2u * (u32)ctx->mark;
    else
        ref_hy = 2u * (u32)t4->p.seed_price;

    u32 fv_hy = t4_fair_value_half_ticks(t4, ctx);
    i64 target = t4_target_position(t4, fv_hy, ref_hy);

    // at (or close enough to) the position our view calls for - hold and look again later.
    // holding is most of what this tier does; it acts only around catalysts and drift
    i64 delta = target - t4->inventory;
    i64 abs_delta = delta < 0 ? -delta : delta;
    if (abs_delta < (i64)t4->p.child_size / 4)
        return t4_sleep(ctx, t4->p.rebalance_interval_ns);

    return t4_work_toward(t4, ctx, out, target, fv_hy);
}

void t4_get_settings(T4* t4, ClientSettings* client_settings) {
    client_settings->initial_wake    = t4->first_wake_ns;
    client_settings->processing_time = t4->p.processing_time;
    client_settings->net_latency     = t4->p.net_latency;

    // margin: a fund takes both directions on conviction, long when cheap and short when
    // dear, so it must be able to run the position negative
    client_settings->is_cash_account = 0;
    client_settings->margin_mult     = t4->p.margin_mult;
    client_settings->maint_pct       = t4->p.maint_pct;
    client_settings->cash            = t4->p.aum;
    client_settings->reserved_cash   = 0;
    client_settings->shares          = 0;
    client_settings->reserved_shares = 0;

    // L1 + the signal. the doc gives this tier L1/L0 - it does not model queues or read
    // depth for edge, it needs the touch to execute against and the signal to form a view
    client_settings->sub_tier = TIER_MBP1;
    client_settings->noii     = 0;
}

void t4_free(T4* t4) {
    if (t4->p.routing == T4_VIA_SLICER)
        printf("T4 %s: routing VIA_SLICER needs the T11 routing layer (one client handing "
               "an order to another), which does not exist - ran inert\n",
               T4_NAMES[t4->name_idx]);
    free(t4);
}
