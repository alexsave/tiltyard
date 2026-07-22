#include <stdlib.h>
#include <stdio.h>

#include "strategy/tier05_degens.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"
#include "mbp.h" // MBP1: the BBO a broker app shows, this tier's whole data budget
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
    p.up_threshold_bp        = 300;   /* UNCALIBRATED */ // 3% above trend
    p.stop_loss_pct          = 6;     /* UNCALIBRATED */

    // concentrated: a quarter of the account into one name, which is retail-plausible and
    // still leaves room to be wrong three times
    p.position_concentration_pct = 25;  /* UNCALIBRATED */
    p.min_position_size          = 10;  /* UNCALIBRATED */

    // ~0.8 taker per the doc
    p.taker_probability_pct  = 80;    /* UNCALIBRATED */
    p.marketable_limit_ticks = 2;     /* UNCALIBRATED */
    p.limit_patience_ns      = 2 * MIN_TO_NS; /* UNCALIBRATED */

    // mostly-shared signal, with real private disagreement on top
    p.herd_correlation_pct   = 75;    /* UNCALIBRATED */
    p.idiosyncratic_bp       = 600;   /* UNCALIBRATED */

    // a trigger fires a cluster, not one identical block
    p.participation_pct      = 40;    /* UNCALIBRATED */
    p.threshold_jitter_bp    = 300;   /* UNCALIBRATED */

    // human seconds-to-minutes reacting to a move: look every few minutes, bursty
    p.burst_wake_ns          = 3 * MIN_TO_NS; /* UNCALIBRATED */
    p.burst_jitter_ns        = 2 * MIN_TO_NS; /* UNCALIBRATED */
    p.initial_wake_spread_ns = 6 * H_TO_NS;   /* UNCALIBRATED */

    p.seed_price             = 100;   /* UNCALIBRATED */

    p.retry_wake_ns          = 10 * MIN_TO_NS; /* UNCALIBRATED */
    p.reject_backoff_ns      = 30 * S_TO_NS;   /* UNCALIBRATED */
    p.missed_backoff_ns      = 5 * S_TO_NS;    /* UNCALIBRATED */

    // thousands to low-hundreds-of-thousands, per the doc. drawn per agent
    p.capital_min            = 5000;   /* UNCALIBRATED */
    p.capital_max            = 250000; /* UNCALIBRATED */

    // a day trade is closed the same day. one session is the outer bound
    p.max_hold_ns            = 4 * H_TO_NS; /* UNCALIBRATED */

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
    t5->position_since_ns = 0;
    t5->exiting = 0;
    t5->shot_id = MAX_U32;
    t5->stop_id = MAX_U32;
    t5->limit_id = MAX_U32;
    t5->limit_since_ns = 0;
    t5->pending = 0;
    t5->pending_kind = T5_PEND_NONE;
    t5->pending_id = MAX_U32;

    // names go out in init order, so the roster is stable across runs
    t5->name_idx = t5_next_name % T5_NAME_COUNT;
    // each agent carries its own rng state, seeded off its slot. no shared global rng
    t5->rng = 0x1b56c4e9u * (t5_next_name + 1);

    // and its own arrival time. spreading the first wake is what breaks the population out
    // of lockstep at the start; the scatter above is what keeps it out
    t5->first_wake_ns = t5->p.initial_wake
                      + (u64)(t5_rand(t5) % 1000) * (t5->p.initial_wake_spread_ns / 1000);

    // this agent's account, drawn once. every degen brings a different amount of money to
    // the same idea - which is most of what turns one signal into a spread of order sizes
    i64 span = t5->p.capital_max - t5->p.capital_min;
    t5->capital = t5->p.capital_min + (i64)(t5_rand(t5) % (u32)(span + 1));
    t5->cash_guess = t5->capital;

    // 200 agents, 16 handles: the slot number keeps them distinguishable in the tape
    snprintf(t5->name, sizeof(t5->name), "%s_%u", T5_NAMES[t5->name_idx], t5_next_name);
    t5_next_name++;

    return t5;
}

char* t5_get_name(T5* t5) {
    return t5->name;
}

// bursty self-wake - action bit 2. WAKE_SIGNAL modelled as "look every so often, act only
// when the tape is moving". the engine keeps only the earliest wake in flight
// NOTHING THIS TIER DOES RUNS ON A CLOCK. a day trader has no cadence - they look when
// they look. so every delay the tier asks for gets stretched by a random factor rather
// than used as given: the burst cadence, the overnight wait, the backoff after a reject,
// all of it.
//
// exact constants are the trap, and they are easy to miss because each one looks harmless
// on its own. 10 minutes, 30 seconds, 3 minutes - every one a divisor of an hour, sitting
// on top of an initial_wake that was 13:00:00.000000000 on the dot for all 200 agents. a
// fixed period can never drift off the boundary it started on, so the tier stays welded to
// the top of the hour for the whole run. the tape showed it plainly: 13,479 trades in
// minute :00 against a ~1,900 baseline, and a spike on the chart every 30 minutes
static u64 t5_scatter(T5* t5, u64 base) {
    if (base == 0)
        return 0;
    // uniform on roughly [50%, 150%] of what was asked for
    return base / 2 + (u64)(t5_rand(t5) % 1001) * (base / 1000);
}

static u8 t5_sleep(T5* t5, Context* ctx, u64 base) {
    u64 delay = t5_scatter(t5, base);
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

// TIER_MBP1 is the BBO - exactly the two prices a broker app shows, which is the whole
// data budget of this tier. no live stream at this cadence, so it only ever arrives on the
// ack to one of our own orders
static void t5_read_book(T5* t5, Context* ctx) {
    MBP1* bbo = (MBP1*)ctx->data_snapshot;
    if (!bbo)
        return;
    // price 0 is the engine's "no such level"
    if (bbo->hi_bid.price) {
        t5->last_bid = bbo->hi_bid.price;
        t5->have_bid = 1;
    }
    if (bbo->lo_ask.price) {
        t5->last_ask = bbo->lo_ask.price;
        t5->have_ask = 1;
    }
}

// POSITION_CONCENTRATION, in money rather than shares. a fixed share count stops being
// affordable the moment price runs, and a retail account does not think in shares anyway
static u16 t5_position_size(T5* t5, u16 price) {
    if (price == 0)
        return 0;
    i64 budget = t5->cash_guess * (i64)t5->p.position_concentration_pct / 100;
    if (budget > t5->cash_guess)
        budget = t5->cash_guess;
    i64 shares = budget / (i64)price;
    if (shares < (i64)t5->p.min_position_size)
        return 0;             // account too small at this price to bother
    if (shares > MAX_U16)
        shares = MAX_U16;
    return (u16)shares;
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

        if (kind == T5_PEND_BUY || kind == T5_PEND_LIMIT) {
            if (kind == T5_PEND_BUY)
                t5->shot_id = MAX_U32;
            if (is_fill && ctx->quantity_filled) {
                t5->inventory += (i64)ctx->quantity_filled;
                t5->cash_guess -= (i64)ctx->quantity_filled * (i64)fill_px;
                t5->entry_price = fill_px; // mark for a market buy, since price is 0
                t5->need_stop = 1; // long now, park the protective stop next
                t5->position_since_ns = ctx->real_time_ns;
            }
            // a perched limit that did not fully fill on arrival is now RESTING, and we
            // need its id to pull it later. one that filled outright never rests at all
            if (kind == T5_PEND_LIMIT) {
                u8 partial = (ctx->status >> PARTIAL_FILL_BIT) & 1;
                t5->limit_id = (!is_reject && (!is_fill || partial)) ? ctx->order_id
                                                                    : MAX_U32;
            }
            if (is_reject) {
                // an unfilled entry IOC is a lost race, not an error - try again later
                return ctx->rej_reason == CXL_IOC_UNFILLED ? 2 : 1;
            }
        } else if (kind == T5_PEND_PULL) {
            t5->limit_id = MAX_U32;
            if (is_reject)
                return 1;
        } else if (kind == T5_PEND_UNSTOP) {
            // the stop is off; the exit sell can go now
            t5->stop_id = MAX_U32;
            if (is_reject) {
                // could not pull it - it most likely already fired, which flattens us anyway
                t5->exiting = 0;
                return 1;
            }
        } else if (kind == T5_PEND_EXIT) {
            if (is_fill && ctx->quantity_filled) {
                t5->inventory -= (i64)ctx->quantity_filled;
                t5->cash_guess += (i64)ctx->quantity_filled * (i64)fill_px;
            }
            if (t5->inventory <= 0) {
                t5->inventory = 0;
                t5->exiting = 0;
                t5->position_since_ns = 0;
            }
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
static u8 t5_send_buy(T5* t5, Context* ctx, Order* out, u16 qty) {
    out->status = (1 << IS_MARKET_BIT) | (1 << IOC_BIT) | (1 << BUY_DIRECTION_BIT);
    out->quantity = qty;
    out->price = 0; // market
    t5->shot_id = ctx->next_order_id;
    return t5_await(t5, ctx, T5_PEND_BUY);
}

// a marketable limit: crosses the spread like a market order but with a cap on how far it
// will chase. same impatience, one shred of price protection - the retail order type that
// isn't a naked market order
static u8 t5_send_marketable(T5* t5, Context* ctx, Order* out, u16 qty) {
    u32 px = (u32)t5->last_ask + t5->p.marketable_limit_ticks;
    out->status = (1 << IOC_BIT) | (1 << BUY_DIRECTION_BIT);
    out->price = px > MAX_U16 ? MAX_U16 : (u16)px;
    out->quantity = qty;
    t5->shot_id = ctx->next_order_id;
    return t5_await(t5, ctx, T5_PEND_BUY);
}

// the impatient LIMIT: perch at the bid rather than pay the offer, then pull it if it has
// not filled by limit_patience_ns. this is the one in five that does not cross, and the
// only thing this tier ever contributes to the resting book
static u8 t5_send_limit(T5* t5, Context* ctx, Order* out, u16 qty) {
    out->status = (1 << BUY_DIRECTION_BIT) | (1 << DAY_BIT);
    out->price = t5->last_bid;
    out->quantity = qty;
    t5->limit_since_ns = ctx->real_time_ns;
    return t5_await(t5, ctx, T5_PEND_LIMIT);
}

// pull an order by id - the perched limit that never filled, or the protective stop that
// has to come off before a time exit can sell the shares it is guarding
static u8 t5_send_cancel(T5* t5, Context* ctx, Order* out, u32 id, u8 kind) {
    out->status = (1 << CANCEL_BIT);
    out->other_id = id;
    return t5_await(t5, ctx, kind);
}

// the intraday time exit. a day trader closes the same day, so a position that has neither
// been stopped out nor run its course gets sold at the market
static u8 t5_send_exit(T5* t5, Context* ctx, Order* out) {
    out->status = (1 << IS_MARKET_BIT) | (1 << IOC_BIT); // sell
    out->quantity = (u16)t5->inventory;
    out->price = 0;
    return t5_await(t5, ctx, T5_PEND_EXIT);
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
    t5_read_book(t5, ctx);

    if (t5->pending)
        return t5_sleep(t5, ctx, t5->p.burst_wake_ns);

    if (!ctx->is_open)
        return t5_sleep(t5, ctx, t5->p.retry_wake_ns);

    // just filled an entry: park the protective stop before doing anything else
    if (t5->need_stop && t5->inventory > 0)
        return t5_send_stop(t5, ctx, out);

    // a time exit already under way: the stop is off, sell the shares
    if (t5->exiting && t5->inventory > 0)
        return t5_send_exit(t5, ctx, out);

    if (t5->inventory > 0) {
        // held it long enough. a DAY trader closes the same day - the stop is the exit we
        // hope for, this is the one we take when the move simply never resolved. the stop
        // has to come off first or it is guarding shares we are about to sell
        u8 timed_out = t5->position_since_ns != 0 &&
                       (ctx->real_time_ns - t5->position_since_ns) >= t5->p.max_hold_ns;
        if (timed_out) {
            t5->exiting = 1;
            if (t5->stop_id != MAX_U32)
                return t5_send_cancel(t5, ctx, out, t5->stop_id, T5_PEND_UNSTOP);
            return t5_send_exit(t5, ctx, out);
        }
        // otherwise ride it. the exit we WANT is the stop firing on weakness - we do not
        // actively sell, we let the stop be the sell. that is what clusters the cascade
        return t5_sleep(t5, ctx, t5->p.burst_wake_ns);
    }

    // flat, but a perched limit is still out there. give it limit_patience_ns and then pull
    // it - the whole point of this tier is that it does not wait around
    if (t5->limit_id != MAX_U32) {
        if (ctx->real_time_ns - t5->limit_since_ns >= t5->p.limit_patience_ns)
            return t5_send_cancel(t5, ctx, out, t5->limit_id, T5_PEND_PULL);
        return t5_sleep(t5, ctx, t5->p.burst_wake_ns);
    }

    // flat: look for strength to chase. need a seeded trend and a printed price
    if (!t5->ema_ready || ctx->mark == 0)
        return t5_sleep(t5, ctx, t5->p.burst_wake_ns);

    // momentum = how far the last print sits above the trend, in half ticks. a per-agent
    // threshold jitter means the herd crosses over a spread of ticks, not all at one instant
    i64 shared_hy = (i64)(2u * (u32)ctx->mark) - (i64)t5->ema_hy;

    // HERD_CORRELATION. the shared half is the common tape - identical for every degen, and
    // the reason they act together at all. the private half is this agent's own read, which
    // is what stops them being one indivisible block. at 100 the tier is a single organism;
    // at 0 it is 200 unrelated people who happen to share a ticker
    // measured as a FRACTION of the trend, not a distance from it. this is the whole
    // difference between a momentum trader and an unconditional buyer: a fixed tick gap
    // is a 3% move at $100 and a rounding error at $10,000, so an absolute threshold
    // decays into always-true exactly when a trend is running - which is precisely when
    // a momentum tier most needs to still be able to say no
    i64 shared_bp = t5->ema_hy ? (shared_hy * 10000) / (i64)t5->ema_hy : 0;

    i64 span = (i64)t5->p.idiosyncratic_bp;
    i64 own_bp = span ? (i64)(t5_rand(t5) % (u32)(2 * span + 1)) - span : 0;
    i64 momentum_bp = (shared_bp * (i64)t5->p.herd_correlation_pct +
                       own_bp * (100 - (i64)t5->p.herd_correlation_pct)) / 100;

    u32 jitter = t5_rand(t5) % (t5->p.threshold_jitter_bp + 1);
    i64 threshold_bp = (i64)t5->p.up_threshold_bp + (i64)jitter;

    if (momentum_bp < threshold_bp)
        return t5_sleep(t5, ctx, t5->p.burst_wake_ns); // no move worth chasing

    // eligible - but only a fraction of the herd actually pulls the trigger on any given
    // move, so it fires as a cluster rather than one monolithic block
    if ((t5_rand(t5) % 100) >= t5->p.participation_pct)
        return t5_sleep(t5, ctx, t5->p.burst_wake_ns);

    // size it in money, against the price we would actually pay
    u16 ref = t5->have_ask ? t5->last_ask : ctx->mark;
    u16 qty = t5_position_size(t5, ref);
    if (qty == 0)
        return t5_sleep(t5, ctx, t5->p.burst_wake_ns); // blown up, or too small to bother

    // THE LEAN. taker_probability of the time we cross to get in now; the rest of the time
    // we perch a limit and dare the market to come to us. without a book we cannot price a
    // limit at all, and an impatient buyer does not wait for one - so that falls to market
    u8 cross = (t5_rand(t5) % 100) < t5->p.taker_probability_pct;
    if (!cross && t5->have_bid)
        return t5_send_limit(t5, ctx, out, qty);
    if (cross && t5->have_ask)
        return t5_send_marketable(t5, ctx, out, qty);
    return t5_send_buy(t5, ctx, out, qty);
}

void t5_get_settings(T5* t5, ClientSettings* client_settings) {
    client_settings->initial_wake    = t5->first_wake_ns;
    client_settings->processing_time = t5->p.processing_time;
    client_settings->net_latency     = t5->p.net_latency;

    // cash account: retail, and long-only by construction - can't short. that is what makes
    // the stop field lopsided to the sell side, the whole crash-fuel point of this tier
    client_settings->is_cash_account = 1;
    client_settings->cash            = t5->capital;
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
