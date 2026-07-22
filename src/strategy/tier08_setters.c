#include <stdlib.h>
#include <stdio.h>

#include "strategy/tier08_setters.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"
#include "mbp.h" // MBP1: an L1 quote and an EOD chart is the whole data budget
#include "server.h"
#include "response.h"

// T8 - Setters. patient limits at support, stops below, hold days to weeks.
//
// the sim runs 0 for now. the real population is low-to-mid millions - a fuzzy behavioural
// slice rather than a registry count; the doc puts the tier at 1000s in sim or AGGREGATE.

// swing-trader handles, loosely parodied. one per instance, handed out in init order
static const char* T8_NAMES[] = {
    "fib_retracer",    "cup_and_handle",  "rsi_divergent",   "macd_crossover",
    "trendline_tom",   "support_sam",     "breakout_betty",  "golden_crosser",
    "elliott_waver",   "bollinger_bill",  "swing_sally",     "candlestick_carl",
};
static const u32 T8_NAME_COUNT = sizeof(T8_NAMES) / sizeof(T8_NAMES[0]);
static u32 t8_next_name = 0;

// the tunable block. EVERY value here is a placeholder - none of it is calibrated, and
// none of it should be tuned to make the tape "look right". that is the owner's job.
// a function rather than file scope so it can use the time constants: a const u64 is not
// a constant expression in C, so S_TO_NS cannot seed a static
static T8Params t8_defaults() {
    T8Params p;

    // after work: ~19:30 UTC, spread over a couple of hours across the population
    p.check_time_of_day_s   = 70200; /* UNCALIBRATED */
    p.check_spread_s        = 7200;  /* UNCALIBRATED */

    p.entry_offset_bp       = 200;   /* UNCALIBRATED */ // 2% below, "at support"
    p.stop_loss_pct         = 8;     /* UNCALIBRATED */
    p.stop_loss_spread_pct   = 4;     /* UNCALIBRATED */

    p.hold_horizon_ns       = 10 * DAY_TO_NS; /* UNCALIBRATED */
    p.order_ttl_ns          = 5 * DAY_TO_NS;  /* UNCALIBRATED */

    p.taker_probability_pct = 30;    /* UNCALIBRATED */ // per the doc

    p.position_concentration_pct = 20; /* UNCALIBRATED */
    p.min_position_size          = 10; /* UNCALIBRATED */

    p.retry_wake_ns         = 4 * H_TO_NS;   /* UNCALIBRATED */
    p.reject_backoff_ns     = 1 * H_TO_NS;   /* UNCALIBRATED */

    p.capital_min           = 5000;   /* UNCALIBRATED */
    p.capital_max           = 200000; /* UNCALIBRATED */

    // it is placing resting orders, not reacting. latency here is a rounding error on a
    // decision that was made hours ago in front of a chart
    p.processing_time       = 2 * S_TO_NS;   /* UNCALIBRATED */
    p.net_latency           = 150 * (S_TO_NS / 1000); /* UNCALIBRATED */ // 150ms
    p.initial_wake          = 15 * H_TO_NS;  /* UNCALIBRATED */
    p.initial_wake_spread_ns = 2 * H_TO_NS; /* UNCALIBRATED */

    return p;
}

// xorshift, per agent. determinism is a hard rule: same seed in, same sequence out
static u32 t8_rand(T8* t8) {
    u32 x = t8->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t8->rng = x;
    return x;
}

T8* t8_init() {
    T8* t8 = malloc(sizeof(T8));

    t8->p = t8_defaults();

    t8->inventory = 0;
    t8->entry_price = 0;
    t8->entry_id = MAX_U32;
    t8->entry_since_ns = 0;
    t8->stop_id = MAX_U32;
    t8->need_stop = 0;
    t8->position_since_ns = 0;
    t8->exiting = 0;
    t8->last_bid = 0;
    t8->last_ask = 0;
    t8->have_bid = 0;
    t8->have_ask = 0;
    t8->pending = 0;
    t8->pending_kind = T8_PEND_NONE;
    t8->pending_id = MAX_U32;

    // names go out in init order, so the roster is stable across runs
    t8->name_idx = t8_next_name % T8_NAME_COUNT;
    // each agent carries its own rng state, seeded off its slot. no shared global rng
    t8->rng = 0x7feb352du * (t8_next_name + 1);

    // and its own stop distance, so the field is smeared rather than stacked
    {
        u16 sp = t8->p.stop_loss_spread_pct;
        if (sp) {
            i32 off = (i32)(t8_rand(t8) % (2u * sp + 1u)) - (i32)sp;
            i32 v = (i32)t8->p.stop_loss_pct + off;
            t8->p.stop_loss_pct = (u16)(v < 1 ? 1 : v);
        }
    }

    // its own boot phase, so the tier does not start life as one agent
    t8->first_wake_ns = t8->p.initial_wake
                      + (u64)(t8_rand(t8) % 1000) * (t8->p.initial_wake_spread_ns / 1000);

    i64 span = t8->p.capital_max - t8->p.capital_min;
    t8->capital = t8->p.capital_min + (i64)(t8_rand(t8) % (u32)(span + 1));
    t8->cash_guess = t8->capital;

    // this person's own after-work hour. without the spread the whole tier checks the
    // chart on the same second and places identical orders at the same level
    t8->check_offset_s = t8->p.check_spread_s
                       ? t8_rand(t8) % t8->p.check_spread_s : 0;

    t8_next_name++;

    return t8;
}

char* t8_get_name(T8* t8) {
    return (char*)T8_NAMES[t8->name_idx];
}

// WAKE_CALENDAR: come back at this person's check time tomorrow (or later today)
static u8 t8_sleep_until_check(T8* t8, Context* ctx) {
    u64 want_s = (u64)t8->p.check_time_of_day_s + t8->check_offset_s;
    u64 tod_s = (ctx->real_time_ns % DAY_TO_NS) / S_TO_NS;
    u64 delay = want_s > tod_s ? (want_s - tod_s) * S_TO_NS
                               : (DAY_TO_NS - (tod_s - want_s) * S_TO_NS);
    if (ctx->real_time_ns + delay >= ctx->next_wake_ns)
        return 0;
    ctx->wake_delay_ns = delay;
    return 2;
}

static u8 t8_sleep(T8* t8, Context* ctx, u64 delay) {
    (void)t8;
    if (ctx->real_time_ns + delay >= ctx->next_wake_ns)
        return 0;
    ctx->wake_delay_ns = delay;
    return 2;
}

static u8 t8_await(T8* t8, Context* ctx, u8 kind) {
    t8->pending = 1;
    t8->pending_kind = kind;
    t8->pending_id = ctx->next_order_id;
    return 1;
}

static void t8_read_book(T8* t8, Context* ctx) {
    MBP1* bbo = (MBP1*)ctx->data_snapshot;
    if (!bbo)
        return;
    if (bbo->hi_bid.price) { t8->last_bid = bbo->hi_bid.price; t8->have_bid = 1; }
    if (bbo->lo_ask.price) { t8->last_ask = bbo->lo_ask.price; t8->have_ask = 1; }
}

static u16 t8_size(T8* t8, u16 price) {
    if (price == 0)
        return 0;
    i64 budget = t8->cash_guess * (i64)t8->p.position_concentration_pct / 100;
    i64 shares = budget / (i64)price;
    if (shares < (i64)t8->p.min_position_size)
        return 0;
    if (shares > MAX_U16)
        shares = MAX_U16;
    return (u16)shares;
}

static u8 t8_settle(T8* t8, Context* ctx) {
    u8 is_fill    = (ctx->status >> FILL_BIT) & 1;
    u8 is_reject  = (ctx->status >> REJECT_BIT) & 1;
    u8 is_partial = (ctx->status >> PARTIAL_FILL_BIT) & 1;
    u16 fill_px = ctx->price ? ctx->price : ctx->mark;

    // the entry was resting and got hit while this person was asleep. that is the entire
    // design of the tier, so it arrives under the ENTRY's id, not under a pending message
    if (t8->entry_id != MAX_U32 && ctx->order_id == t8->entry_id && is_fill) {
        t8->inventory += (i64)ctx->quantity_filled;
        t8->cash_guess -= (i64)ctx->quantity_filled * (i64)fill_px;
        t8->entry_price = fill_px;
        t8->need_stop = 1;
        if (t8->position_since_ns == 0)
            t8->position_since_ns = ctx->real_time_ns;
        if (!is_partial)
            t8->entry_id = MAX_U32;
    }

    // the stop fired. this is the sell that stacks under the Degens' stop field
    if (t8->stop_id != MAX_U32 && ctx->order_id == t8->stop_id && is_fill) {
        t8->inventory -= (i64)ctx->quantity_filled;
        t8->cash_guess += (i64)ctx->quantity_filled * (i64)fill_px;
        if (t8->inventory <= 0) {
            t8->inventory = 0;
            t8->stop_id = MAX_U32;
            t8->position_since_ns = 0;
        }
    }

    if (t8->pending && ctx->order_id == t8->pending_id) {
        u8 kind = t8->pending_kind;
        t8->pending = 0;
        t8->pending_kind = T8_PEND_NONE;
        t8->pending_id = MAX_U32;

        if (kind == T8_PEND_ENTRY) {
            if (is_fill && ctx->quantity_filled) {
                t8->inventory += (i64)ctx->quantity_filled;
                t8->cash_guess -= (i64)ctx->quantity_filled * (i64)fill_px;
                t8->entry_price = fill_px;
                t8->need_stop = 1;
                t8->position_since_ns = ctx->real_time_ns;
            }
            // whatever did not fill on arrival is now resting, and it is the resting that
            // this tier is FOR - keep the id so the ttl can pull it later
            if (is_reject)
                t8->entry_id = MAX_U32;
            else if (!is_fill || is_partial)
                t8->entry_id = ctx->order_id;
            if (is_reject)
                return 1;
        } else if (kind == T8_PEND_STOP) {
            if (is_reject)
                return 1;
            t8->stop_id = ctx->order_id;
            t8->need_stop = 0;
        } else if (kind == T8_PEND_PULL) {
            t8->entry_id = MAX_U32;
            if (is_reject)
                return 1;
        } else if (kind == T8_PEND_UNSTOP) {
            t8->stop_id = MAX_U32;
            if (is_reject) {
                t8->exiting = 0;
                return 1;
            }
        } else if (kind == T8_PEND_EXIT) {
            if (is_fill && ctx->quantity_filled) {
                t8->inventory -= (i64)ctx->quantity_filled;
                t8->cash_guess += (i64)ctx->quantity_filled * (i64)fill_px;
            }
            if (t8->inventory <= 0) {
                t8->inventory = 0;
                t8->exiting = 0;
                t8->position_since_ns = 0;
            }
            if (is_reject)
                return 1;
        }
    }

    return 0;
}

// the patient entry at "support" - a limit a fixed fraction below the market, carrying GTD
// so it survives the close. this is the order the whole tier exists to place
static u8 t8_send_entry(T8* t8, Context* ctx, Order* out, u8 cross) {
    u16 ref = t8->have_ask ? t8->last_ask : ctx->mark;
    u32 px;
    if (cross) {
        // sometimes the move is already going and waiting for a retest means missing it
        px = ref;
        out->status = (1 << IOC_BIT) | (1 << BUY_DIRECTION_BIT);
    } else {
        px = (u32)ctx->mark * (10000u - t8->p.entry_offset_bp) / 10000u;
        if (px < 1)
            px = 1;
        // GTD: the defining order type of this tier. it has to outlive the session, because
        // the person who placed it will not be awake when the market comes down to meet it
        out->status = (1 << BUY_DIRECTION_BIT) | (1 << GTD_BIT);
        out->second_id = (u32)((ctx->real_time_ns + t8->p.order_ttl_ns) / DAY_TO_NS);
    }

    u16 qty = t8_size(t8, (u16)(px > MAX_U16 ? MAX_U16 : px));
    if (qty == 0)
        return t8_sleep_until_check(t8, ctx);

    out->price = px > MAX_U16 ? MAX_U16 : (u16)px;
    out->quantity = qty;
    t8->entry_since_ns = ctx->real_time_ns;
    return t8_await(t8, ctx, T8_PEND_ENTRY);
}

static u8 t8_send_stop(T8* t8, Context* ctx, Order* out) {
    u32 trig = (u32)t8->entry_price * (100u - t8->p.stop_loss_pct) / 100u;
    if (trig < 1)
        trig = 1;
    out->status = (1 << HAS_STOP_BIT);  // market on trigger
    out->stop_price = (u16)trig;
    out->second_direction = 0;
    out->second_quantity = (u16)t8->inventory;
    out->second_price = 0;
    return t8_await(t8, ctx, T8_PEND_STOP);
}

static u8 t8_send_cancel(T8* t8, Context* ctx, Order* out, u32 id, u8 kind) {
    out->status = (1 << CANCEL_BIT);
    out->other_id = id;
    return t8_await(t8, ctx, kind);
}

static u8 t8_send_exit(T8* t8, Context* ctx, Order* out) {
    out->status = (1 << IS_MARKET_BIT) | (1 << IOC_BIT); // sell
    out->quantity = (u16)t8->inventory;
    out->price = 0;
    return t8_await(t8, ctx, T8_PEND_EXIT);
}

u8 t8_on_snapshot(T8* t8, Context* ctx) {
    Order* out = ctx->next_order_ptr;

    u8 settled = t8_settle(t8, ctx);
    if (settled == 1)
        return t8_sleep(t8, ctx, t8->p.reject_backoff_ns);

    t8_read_book(t8, ctx);

    if (t8->pending)
        return t8_sleep_until_check(t8, ctx);

    if (!ctx->is_open)
        return t8_sleep(t8, ctx, t8->p.retry_wake_ns);

    // a filled entry needs its stop parked before anything else happens
    if (t8->need_stop && t8->inventory > 0)
        return t8_send_stop(t8, ctx, out);

    if (t8->exiting && t8->inventory > 0)
        return t8_send_exit(t8, ctx, out);

    if (t8->inventory > 0) {
        // the horizon ran out. a swing trade that has not worked in weeks is closed and
        // the capital goes somewhere else - the stop comes off first
        u8 timed_out = t8->position_since_ns != 0 &&
                       (ctx->real_time_ns - t8->position_since_ns) >= t8->p.hold_horizon_ns;
        if (timed_out) {
            t8->exiting = 1;
            if (t8->stop_id != MAX_U32)
                return t8_send_cancel(t8, ctx, out, t8->stop_id, T8_PEND_UNSTOP);
            return t8_send_exit(t8, ctx, out);
        }
        // otherwise leave it alone. the stop is the exit, and checking daily is the most
        // attention this tier ever pays
        return t8_sleep_until_check(t8, ctx);
    }

    // an entry still resting. give it its ttl, then give up on that level
    if (t8->entry_id != MAX_U32) {
        if (ctx->real_time_ns - t8->entry_since_ns >= t8->p.order_ttl_ns)
            return t8_send_cancel(t8, ctx, out, t8->entry_id, T8_PEND_PULL);
        return t8_sleep_until_check(t8, ctx);
    }

    if (ctx->mark == 0)
        return t8_sleep(t8, ctx, t8->p.retry_wake_ns);

    // flat, nothing resting: set a new one and go to bed
    u8 cross = (t8_rand(t8) % 100) < t8->p.taker_probability_pct;
    return t8_send_entry(t8, ctx, out, cross);
}

void t8_get_settings(T8* t8, ClientSettings* client_settings) {
    client_settings->initial_wake    = t8->first_wake_ns;
    client_settings->processing_time = t8->p.processing_time;
    client_settings->net_latency     = t8->p.net_latency;

    // cash account. disposable-income retail, long-only, which is what puts its stops on
    // the sell side alongside the Degens'
    client_settings->is_cash_account = 1;
    client_settings->cash            = t8->capital;
    client_settings->reserved_cash   = 0;
    client_settings->shares          = 0;
    client_settings->reserved_shares = 0;

    // L1 and an EOD chart. it prices one order a day off the touch and then stops looking
    client_settings->sub_tier = TIER_MBP1;
    client_settings->noii     = 0;
}

void t8_free(T8* t8) {
    free(t8);
}
