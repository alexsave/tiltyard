#include <stdlib.h>
#include <stdio.h>

#include "strategy/tier05_degens.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"
#include "server.h"
#include "response.h"

// T5 - Degens. momentum retail: buy strength, ride it behind a protective sell-stop, get
// stopped out on weakness. the crash amplifier and the stop-loss fuel.
//
// the sim runs 200. there are ~450k active US day traders; the design doc puts the tier at
// 100s-1000s in sim (or AGGREGATE). 200 is a herd big enough to matter as cascade fuel
// without swamping the message rate - they are bursty, not continuous.

// WSB-flavoured handles, loosely parodied. one per instance, handed out in init order
static const char* T5_NAMES[] = {
    "diamondhandz",  "yolo_capital",   "tendieman",      "stonks_only_up",
    "theta_gang_ng", "wsb_degenerate", "moon_or_bust",   "paper_hands_pete",
    "rockets_emoji", "loss_porn_lord", "hodl_or_die",    "fomo_fred",
    "gamma_squeeze", "apes_together",  "buy_high_sell_lo","bag_holder_bob",
};
static const u32 T5_NAME_COUNT = sizeof(T5_NAMES) / sizeof(T5_NAMES[0]);
static u32 t5_next_name = 0;

// the tunable block. EVERY value here is a placeholder - none of it is calibrated, and
// none of it should be tuned to make the tape "look right". that is the owner's job.
// a function rather than file scope so it can use the time constants: a const u64 is not
// a constant expression in C, so S_TO_NS cannot seed a static
static T5Params t5_defaults() {
    T5Params p;

    p.ema_alpha_pct          = 20;    /* UNCALIBRATED */ // ~ last few prints dominate
    p.up_threshold_ticks     = 3;     /* UNCALIBRATED */
    p.position_size          = 200;   /* UNCALIBRATED */
    p.stop_loss_pct          = 6;     /* UNCALIBRATED */

    // a trigger fires a cluster, not one identical block
    p.participation_pct      = 40;    /* UNCALIBRATED */
    p.threshold_jitter_ticks = 3;     /* UNCALIBRATED */

    // human seconds-to-minutes reacting to a move: look every few minutes, bursty
    p.burst_wake_ns          = 3 * MIN_TO_NS; /* UNCALIBRATED */
    p.burst_jitter_ns        = 2 * MIN_TO_NS; /* UNCALIBRATED */

    p.seed_price             = 100;   /* UNCALIBRATED */

    p.retry_wake_ns          = 10 * MIN_TO_NS; /* UNCALIBRATED */
    p.reject_backoff_ns      = 30 * S_TO_NS;   /* UNCALIBRATED */
    p.missed_backoff_ns      = 5 * S_TO_NS;    /* UNCALIBRATED */

    p.cash                   = 50000; /* UNCALIBRATED */ // $50k retail account
    // hundreds of ms wire-to-book: human reaction + retail broadband + PFOF routing
    p.processing_time        = 200 * (S_TO_NS / 1000); /* UNCALIBRATED */ // 200ms
    p.net_latency            = 100 * (S_TO_NS / 1000); /* UNCALIBRATED */ // 100ms
    p.initial_wake           = 13 * H_TO_NS; /* UNCALIBRATED */

    return p;
}

// xorshift, per agent. determinism is a hard rule: same seed in, same sequence out
static u32 t5_rand(T5* t5) {
    u32 x = t5->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t5->rng = x;
    return x;
}

T5* t5_init() {
    T5* t5 = malloc(sizeof(T5));

    t5->p = t5_defaults();

    t5->ema_hy = 0;
    t5->ema_ready = 0;
    t5->inventory = 0;
    t5->entry_price = 0;
    t5->need_stop = 0;
    t5->shot_id = MAX_U32;
    t5->stop_id = MAX_U32;
    t5->pending = 0;
    t5->pending_kind = T5_PEND_NONE;
    t5->pending_id = MAX_U32;
    t5->cash_guess = t5->p.cash;
    // names go out in init order, so the roster is stable across runs
    t5->name_idx = t5_next_name % T5_NAME_COUNT;
    // each agent carries its own rng state, seeded off its slot. no shared global rng
    t5->rng = 0x1b56c4e9u * (t5_next_name + 1);
    t5_next_name++;

    return t5;
}

char* t5_get_name(T5* t5) {
    return (char*)T5_NAMES[t5->name_idx];
}

// bursty self-wake - action bit 2. WAKE_SIGNAL modelled as "look every so often, act only
// when the tape is moving". the engine keeps only the earliest wake in flight
static u8 t5_sleep(T5* t5, Context* ctx, u64 base) {
    // jitter so 200 degens do not all wake on the exact same tick - a herd clusters, it
    // does not march in lockstep
    u64 delay = base + (t5_rand(t5) % (t5->p.burst_jitter_ns + 1));
    if (ctx->real_time_ns + delay >= ctx->next_wake_ns)
        return 0;
    ctx->wake_delay_ns = delay;
    return 2;
}

static u8 t5_await(T5* t5, Context* ctx, u8 kind) {
    t5->pending = 1;
    t5->pending_kind = kind;
    t5->pending_id = ctx->next_order_id;
    return 1;
}

// roll the momentum EMA forward off the last-trade tape. every degen reads the same mark,
// so every degen's EMA agrees - which is why they herd
static void t5_update_ema(T5* t5, Context* ctx) {
    if (ctx->mark == 0)
        return; // nothing has printed yet
    u32 mark_hy = 2u * (u32)ctx->mark;
    if (!t5->ema_ready) {
        t5->ema_hy = mark_hy;
        t5->ema_ready = 1;
        return;
    }
    // ema += alpha * (mark - ema), integer form
    i64 diff = (i64)mark_hy - (i64)t5->ema_hy;
    t5->ema_hy = (u32)((i64)t5->ema_hy + (diff * (i64)t5->p.ema_alpha_pct) / 100);
}

// fold a response into our own books, and drive the position lifecycle
static u8 t5_settle(T5* t5, Context* ctx) {
    u8 is_fill   = (ctx->status >> FILL_BIT) & 1;
    u8 is_reject = (ctx->status >> REJECT_BIT) & 1;

    // the protective stop fired: a market sell against our long. we are flat again, ready
    // to chase the next move. THIS is the sell that feeds the cascade - when the whole herd
    // hits this at once, each fill drops price into the next band of stops
    // a market order (and a triggered stop-market) reports price 0 - it had no limit. the
    // price it actually traded at is the last print, ctx->mark, so use that for the position
    u16 fill_px = ctx->price ? ctx->price : ctx->mark;

    if (t5->stop_id != MAX_U32 && ctx->order_id == t5->stop_id && is_fill) {
        t5->inventory -= (i64)ctx->quantity_filled;
        t5->cash_guess += (i64)ctx->quantity_filled * (i64)fill_px;
        if (t5->inventory <= 0) {
            t5->inventory = 0;
            t5->stop_id = MAX_U32;
        }
        // the stop's fill is not a message we are "pending" on, so fall through
    }

    if (t5->pending && ctx->order_id == t5->pending_id) {
        u8 kind = t5->pending_kind;
        t5->pending = 0;
        t5->pending_kind = T5_PEND_NONE;
        t5->pending_id = MAX_U32;

        if (kind == T5_PEND_BUY) {
            t5->shot_id = MAX_U32;
            if (is_fill && ctx->quantity_filled) {
                t5->inventory += (i64)ctx->quantity_filled;
                t5->cash_guess -= (i64)ctx->quantity_filled * (i64)fill_px;
                t5->entry_price = fill_px; // mark for a market buy, since price is 0
                t5->need_stop = 1; // long now, park the protective stop next
            }
            // an unfilled entry IOC is a lost race, not an error - just try again later
            if (is_reject)
                return ctx->rej_reason == CXL_IOC_UNFILLED ? 2 : 1;
        } else if (kind == T5_PEND_STOP) {
            if (is_reject) {
                // could not arm the stop (e.g. bad price). leave need_stop set to retry;
                // a real reject still earns a backoff
                return 1;
            }
            t5->stop_id = ctx->order_id;
            t5->need_stop = 0;
        } else if (is_reject) {
            return 1;
        }
    }

    return 0;
}

// the momentum entry: a market buy, IOC. taker by construction - a degen chasing a move
// takes what is there, it does not sit on a passive limit. a market order must carry a
// time-in-force, and IOC is the taker one (drop whatever the touch cannot fill)
static u8 t5_send_buy(T5* t5, Context* ctx, Order* out) {
    out->status = (1 << IS_MARKET_BIT) | (1 << IOC_BIT) | (1 << BUY_DIRECTION_BIT);
    out->quantity = t5->p.position_size;
    out->price = 0; // market
    t5->shot_id = ctx->next_order_id;
    return t5_await(t5, ctx, T5_PEND_BUY);
}

// park the protective sell-stop: a stop-only order (no NOW half) that converts to a market
// sell when the print falls to the trigger. the trigger is a percent below the entry, so it
// scales with price. this is the sell-weakness exit AND the fuel for the downside cascade
static u8 t5_send_stop(T5* t5, Context* ctx, Order* out) {
    u32 trig = (u32)t5->entry_price * (100u - t5->p.stop_loss_pct) / 100u;
    if (trig < 1)
        trig = 1; // price 0 is reserved

    out->status = (1 << HAS_STOP_BIT);   // no STOP_LIMIT_BIT -> converts to market on trigger
    out->stop_price = (u16)trig;
    out->second_direction = 0;           // sell
    out->second_quantity = (u16)t5->inventory;
    out->second_price = 0;               // market on trigger
    return t5_await(t5, ctx, T5_PEND_STOP);
}

u8 t5_on_snapshot(T5* t5, Context* ctx) {
    Order* out = ctx->next_order_ptr;

    u8 settled = t5_settle(t5, ctx);
    if (settled == 1)
        return t5_sleep(t5, ctx, t5->p.reject_backoff_ns);
    if (settled == 2)
        return t5_sleep(t5, ctx, t5->p.missed_backoff_ns);

    // no live stream: a degen is bursty, not tick-by-tick, so it must NOT subscribe - a ws
    // feed would wake it on every trade (millions during a crash), which is the apex wake
    // model, not this one. the last-trade price it needs rides on ctx->mark every wake, and
    // the burst self-wake paces it. it boots on the initial ping and drives itself from there
    t5_update_ema(t5, ctx);

    if (t5->pending)
        return t5_sleep(t5, ctx, t5->p.burst_wake_ns);

    if (!ctx->is_open)
        return t5_sleep(t5, ctx, t5->p.retry_wake_ns);

    // just filled an entry: park the protective stop before doing anything else
    if (t5->need_stop && t5->inventory > 0)
        return t5_send_stop(t5, ctx, out);

    // holding a long behind an armed stop: ride it. the exit is the stop firing on weakness -
    // we do not actively sell, we let the stop be the sell (that is what clusters the cascade)
    if (t5->inventory > 0)
        return t5_sleep(t5, ctx, t5->p.burst_wake_ns);

    // flat: look for strength to chase. need a seeded trend and a printed price
    if (!t5->ema_ready || ctx->mark == 0)
        return t5_sleep(t5, ctx, t5->p.burst_wake_ns);

    // momentum = how far the last print sits above the trend, in half ticks. a per-agent
    // threshold jitter means the herd crosses over a spread of ticks, not all at one instant
    i64 momentum_hy = (i64)(2u * (u32)ctx->mark) - (i64)t5->ema_hy;
    u32 jitter = t5_rand(t5) % (t5->p.threshold_jitter_ticks + 1);
    i64 threshold_hy = 2 * ((i64)t5->p.up_threshold_ticks + (i64)jitter);

    if (momentum_hy < threshold_hy)
        return t5_sleep(t5, ctx, t5->p.burst_wake_ns); // no move worth chasing

    // eligible - but only a fraction of the herd actually pulls the trigger on any given
    // move, so it fires as a cluster rather than one monolithic block
    if ((t5_rand(t5) % 100) >= t5->p.participation_pct)
        return t5_sleep(t5, ctx, t5->p.burst_wake_ns);

    return t5_send_buy(t5, ctx, out);
}

void t5_get_settings(T5* t5, ClientSettings* client_settings) {
    client_settings->initial_wake    = t5->p.initial_wake;
    client_settings->processing_time = t5->p.processing_time;
    client_settings->net_latency     = t5->p.net_latency;

    // cash account: retail, and long-only by construction - can't short. that is what makes
    // the stop field lopsided to the sell side, the whole crash-fuel point of this tier
    client_settings->is_cash_account = 1;
    client_settings->cash            = t5->p.cash;
    client_settings->reserved_cash   = 0;
    client_settings->shares          = 0;
    client_settings->reserved_shares = 0;

    // L1: the BBO and last trade a broker app shows. momentum runs off the last-trade tape
    client_settings->sub_tier = TIER_MBP1;
    client_settings->noii     = 0;
}

void t5_free(T5* t5) {
    free(t5);
}
