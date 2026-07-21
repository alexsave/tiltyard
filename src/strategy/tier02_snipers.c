#include <stdlib.h>
#include <stdio.h>

#include "strategy/tier02_snipers.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"
#include "server.h"
#include "response.h" // rej_reason codes: an unfilled ioc is not a real reject

// T2 - Snipers. form a fair value, find a resting quote on the wrong side of it, hit it
// with a marketable limit + IOC and take back whatever doesn't fill.
//
// the sim runs 12. nasdaq's hft dataset identifies ~120 firms; the design doc puts the
// tier at 10-30 in sim, heavily overlapping T1 - the same desks often run both sides.
// 12 against 8 makers is that overlap plus a few pure takers.

// firm names, loosely parodied. one per instance, handed out in init order
static const char* T2_NAMES[] = {
    "bastion.xr",         // the maker desks wearing their taking hat
    "virtuo.strike",
    "plain.street.alpha",
    "vault.velocity",
    "tower.run",          // and the pure latency shops
    "quantlab.edge",      // (no relation to anyone real)
    "headland.trading",
    "sparrow.systems",
    "meridian.tick",
    "cutter.capital",
    "riptide.markets",
    "nine.mile.trading",
};
static const u32 T2_NAME_COUNT = sizeof(T2_NAMES) / sizeof(T2_NAMES[0]);
static u32 t2_next_name = 0;

// the tunable block. EVERY value here is a placeholder - none of it is calibrated, and
// none of it should be tuned to make the tape "look right". that is the owner's job.
// a function rather than file scope so it can use the time constants: a const u64 is not
// a constant expression in C, so S_TO_NS cannot seed a static
static T2Params t2_defaults() {
    T2Params p;

    p.fair_value_source           = T2_FV_BLEND; /* UNCALIBRATED */
    p.staleness_threshold_ticks   = 1;      /* UNCALIBRATED */
    p.min_edge_ticks              = 1;      /* UNCALIBRATED */
    // the same reference a maker seeds its quotes around, so a neutral signal agrees with
    // where the book starts rather than fighting it
    p.fundamental_anchor          = 100;    /* UNCALIBRATED */
    p.news_full_swing_ticks       = 8;      /* UNCALIBRATED */
    p.fundamental_weight          = 192;    /* UNCALIBRATED */

    p.max_take_qty                = 400;    /* UNCALIBRATED */
    p.max_position                = 1000;   /* UNCALIBRATED */
    // above max_take_qty on purpose: at or below it, every single successful shot would
    // immediately trip the flatten and the tier would never hold a position at all
    p.flatten_threshold           = 800;    /* UNCALIBRATED */
    p.flatten_urgency_ticks       = 1;      /* UNCALIBRATED */
    p.max_hold_ns                 = 30 * MIN_TO_NS; /* UNCALIBRATED */

    p.idle_wake_ns                = 1 * S_TO_NS;   /* UNCALIBRATED */
    p.retry_wake_ns               = 1 * MIN_TO_NS; /* UNCALIBRATED */
    p.reject_backoff_ns           = 1 * S_TO_NS;   /* UNCALIBRATED */
    p.missed_backoff_ns           = S_TO_NS / 1000; /* UNCALIBRATED */ // 1ms

    p.cash                        = 1000000000; /* UNCALIBRATED */
    p.margin_mult                 = 4;      /* UNCALIBRATED */
    p.maint_pct                   = 25;     /* UNCALIBRATED */

    // the race to hit a stale quote is won by nanoseconds - same or faster than a maker
    // on the take path
    p.processing_time             = 250;    /* UNCALIBRATED */
    p.net_latency                 = 200;    /* UNCALIBRATED */

    p.initial_wake                = 13 * H_TO_NS; /* UNCALIBRATED */

    return p;
}

T2* t2_init() {
    T2* t2 = malloc(sizeof(T2));

    t2->p = t2_defaults();

    t2->pending = 0;
    t2->position_since_ns = 0;
    t2->pending_buy = 0;
    t2->shot_id = MAX_U32;
    t2->inventory = 0;
    t2->cash_guess = t2->p.cash;
    t2->connected = 0;
    t2->have_book = 0;
    t2->last_bid = 0;
    t2->last_ask = 0;
    t2->last_bid_depth = 0;
    t2->last_ask_depth = 0;
    // names go out in init order, so the roster is stable across runs
    t2->name_idx = t2_next_name % T2_NAME_COUNT;
    // each agent carries its own rng state, seeded off its slot. no shared global rng
    t2->rng = 0x85ebca6bu * (t2_next_name + 1);
    t2_next_name++;

    return t2;
}

char* t2_get_name(T2* t2) {
    return (char*)T2_NAMES[t2->name_idx];
}

// what we could read off the top of the book this wake
typedef struct T2Book {
    MBO* mbo;
    u16 best_bid;
    u16 best_ask;
    u32 bid_depth;
    u32 ask_depth;
    u8 have_bid;
    u8 have_ask;
} T2Book;

// a shot we have decided to take
typedef struct T2Shot {
    u16 price;      // the limit we will not pay past. never a plain market order
    u16 quantity;
    u8 buy;
} T2Shot;

// fold this response into our own books. an ioc never rests, so there is no resting id to
// track - only whether the shot we fired is still outstanding.
// returns 1 for a hard reject to back off from, 2 for an ioc that simply found nothing
static u8 t2_settle(T2* t2, Context* ctx) {
    u8 is_ws     = (ctx->status >> WS_BIT) & 1;
    u8 is_ping   = (ctx->status >> PING_BIT) & 1;
    u8 is_fill   = (ctx->status >> FILL_BIT) & 1;
    u8 is_reject = (ctx->status >> REJECT_BIT) & 1;

    if (is_ws)
        t2->connected = 1;

    if (t2->shot_id != MAX_U32 && ctx->order_id == t2->shot_id) {
        if (is_fill) {
            // the time stop runs from when we went flat -> holding, so a position that
            // keeps being added to does not keep resetting its own clock
            i64 before = t2->inventory;

            // an ioc reports its fill and keeps nothing back, so this is the whole trade.
            // direction is ours to remember - the response does not carry it
            if (t2->pending_buy) {
                t2->inventory += (i64)ctx->quantity_filled;
                t2->cash_guess -= (i64)ctx->quantity_filled * (i64)ctx->price;
            } else {
                t2->inventory -= (i64)ctx->quantity_filled;
                t2->cash_guess += (i64)ctx->quantity_filled * (i64)ctx->price;
            }

            if (before == 0 && t2->inventory != 0)
                t2->position_since_ns = ctx->real_time_ns;
            else if (t2->inventory == 0)
                t2->position_since_ns = 0;
        }

        t2->pending = 0;
        t2->shot_id = MAX_U32;

        // an ioc that fills nothing comes back on REJECT_BIT carrying CXL_IOC_UNFILLED.
        // the engine notes this is a stretch of the bit - the order was accepted, it just
        // found no liquidity. for us it means we lost the race, not that we did anything
        // wrong, so it is a much shorter pause than a real reject
        if (is_reject)
            return ctx->rej_reason == CXL_IOC_UNFILLED ? 2 : 1;
        return 0;
    }

    if (is_reject || is_ws || is_ping)
        t2->pending = 0;

    return is_reject ? 1 : 0;
}

// maintain the local replica of the touch and hand back what we currently believe.
//
// most wakes carry no snapshot at all - a self-wake is a bare ping, and the feed only
// pushes when the book actually changes. but fair value moves without the book moving: a
// news event restates what everything is worth while every resting quote stays exactly
// where it was, and those quotes are precisely what has just gone stale. so we keep the
// last touch we saw rather than re-deriving it and giving up when it isn't in hand
static void t2_read_book(T2* t2, Context* ctx, T2Book* b) {
    MBO* mbo = (MBO*)ctx->data_snapshot;

    if (mbo) {
        u8 have_bid = mbo->hi_bid_index != MAX_U16 && mbo->hi_bid_index < mbo->level_count;
        u8 have_ask = have_bid && (u32)mbo->hi_bid_index + 1 < mbo->level_count;

        // one-sided or empty: the replica is stale in a way we can't patch, so drop it
        // rather than shoot at a level we can no longer see
        t2->have_book = have_bid && have_ask;

        if (t2->have_book) {
            MBOIndex* bid = mbo->levels + mbo->hi_bid_index;
            MBOIndex* ask = mbo->levels + mbo->hi_bid_index + 1;
            t2->last_bid = bid->price;
            t2->last_bid_depth = bid->quantity;
            t2->last_ask = ask->price;
            t2->last_ask_depth = ask->quantity;
        }
    }

    b->mbo = mbo;
    b->have_bid = t2->have_book;
    b->have_ask = t2->have_book;
    b->best_bid = t2->have_book ? t2->last_bid : 0;
    b->best_ask = t2->have_book ? t2->last_ask : 0;
    b->bid_depth = t2->have_book ? t2->last_bid_depth : 0;
    b->ask_depth = t2->have_book ? t2->last_ask_depth : 0;
}

// the microprice: the touch weighted by the size behind it. a bid showing 10x the ask's
// size says the next print is more likely at the ask, so fair value sits nearer the ask.
// scaled by 2 so it keeps half-tick resolution in integers
static u32 t2_microprice_half_ticks(T2Book* b) {
    u64 total = (u64)b->bid_depth + (u64)b->ask_depth;
    if (total == 0)
        return ((u32)b->best_bid + (u32)b->best_ask);

    // weight each side by the OPPOSITE depth - that is what leans toward the thin side
    u64 num = (u64)b->best_bid * b->ask_depth + (u64)b->best_ask * b->bid_depth;
    return (u32)((2 * num) / total);
}

// map the engine's 0-255 company-health level onto an absolute price, in half ticks.
// 128 is neutral and sits on the anchor; a full swing either way is worth
// news_full_swing_ticks. returns 0 when nothing has been published yet - with no signal
// there is no opinion to hold
static u32 t2_fundamental_half_ticks(T2* t2, Context* ctx) {
    if (ctx->last_news_ns == 0)
        return 0;

    i32 offset = (i32)ctx->news_signal - 128;
    i64 fv = 2 * (i64)t2->p.fundamental_anchor
           + ((i64)offset * 2 * (i64)t2->p.news_full_swing_ticks) / 128;

    if (fv < 2)
        fv = 2; // price 0 is reserved, so fair value never goes below one tick
    return (u32)fv;
}

// what we think the thing is actually worth, in half ticks
static u32 t2_fair_value_half_ticks(T2* t2, Context* ctx, T2Book* b) {
    u32 micro = t2_microprice_half_ticks(b);

    if (t2->p.fair_value_source == T2_FV_MICROPRICE)
        return micro;

    u32 fundamental = t2_fundamental_half_ticks(t2, ctx);

    // no news yet: nothing to be informed about, so fall back to the book's own read.
    // the microprice can never leave the spread, so on its own it finds no stale quote -
    // which is the correct answer when we have no information the market lacks
    if (fundamental == 0)
        return micro;

    if (t2->p.fair_value_source == T2_FV_NEWS)
        return fundamental;

    // blend: the microprice reads the immediate order flow, the fundamental bounds it
    u32 w = t2->p.fundamental_weight;
    if (w > 256)
        w = 256;
    return (u32)(((u64)micro * (256 - w) + (u64)fundamental * w) / 256);
}

// is there a resting quote on the wrong side of fair value, and by enough to be worth it?
// returns 1 and fills the shot if so.
//
// the ask is stale-cheap when we can buy below what it is worth; the bid is stale-rich
// when we can sell above. both tests are in half ticks so the microprice keeps its
// resolution - a half tick of edge is real money at hft size
static u8 t2_find_edge(T2* t2, T2Book* b, u32 fv, T2Shot* shot) {
    u32 threshold = 2u * (u32)t2->p.staleness_threshold_ticks;
    u32 min_edge = 2u * (u32)t2->p.min_edge_ticks;
    u32 need = threshold > min_edge ? threshold : min_edge;

    // long cap: still allowed to buy only if we are not already at it
    if (b->have_ask && t2->inventory < (i64)t2->p.max_position) {
        u32 ask_hy = 2u * (u32)b->best_ask;
        if (fv > ask_hy && (fv - ask_hy) >= need) {
            shot->buy = 1;
            shot->price = b->best_ask; // marketable limit AT the stale price, never through
            // never take more than is displayed - past it we fill at a price we never
            // judged, which is exactly the edge we thought we were capturing
            u32 room = (u32)((i64)t2->p.max_position - t2->inventory);
            u32 qty = b->ask_depth;
            if (qty > t2->p.max_take_qty) qty = t2->p.max_take_qty;
            if (qty > room) qty = room;
            shot->quantity = (u16)qty;
            return qty > 0;
        }
    }

    // short cap. this is the half of the tier that a cash account cannot express: to sell
    // a stale-rich bid while flat we have to be able to go short and cover
    if (b->have_bid && t2->inventory > -(i64)t2->p.max_position) {
        u32 bid_hy = 2u * (u32)b->best_bid;
        if (bid_hy > fv && (bid_hy - fv) >= need) {
            shot->buy = 0;
            shot->price = b->best_bid;
            u32 room = (u32)((i64)t2->p.max_position + t2->inventory);
            u32 qty = b->bid_depth;
            if (qty > t2->p.max_take_qty) qty = t2->p.max_take_qty;
            if (qty > room) qty = room;
            shot->quantity = (u16)qty;
            return qty > 0;
        }
    }

    return 0;
}

// no edge worth taking, but we are carrying more than we want. work back toward flat by
// hitting the touch, giving up to flatten_urgency_ticks to get out.
//
// the hard rule here: never exit through fair value. "flattens instantly" is about not
// warehousing risk, NOT about dumping at any price - the position exists precisely because
// we judged the thing worth more than we paid, so selling it below that estimate realises
// a loss we already decided was not there. without this gate the tier is a money pump: it
// buys the ask, immediately sells the bid a spread lower, and repeats until the book is on
// the floor. so we only flatten into a touch that has come back to fair value, and
// otherwise hold and wait for one that does
static u8 t2_find_flatten(T2* t2, Context* ctx, T2Book* b, u32 fv, T2Shot* shot) {
    // held too long: the edge we came for has not shown up, so stop waiting for it. past
    // the time stop we get out at whatever the touch is, fair value or not - carrying a
    // position for days is a different business than the one this tier is in
    u8 timed_out = t2->inventory != 0 && t2->position_since_ns != 0 &&
                   (ctx->real_time_ns - t2->position_since_ns) >= t2->p.max_hold_ns;

    if ((t2->inventory >= (i64)t2->p.flatten_threshold || (timed_out && t2->inventory > 0))
        && b->have_bid) {
        // long: sell into the bid, but only at or above what we think it is worth
        if (!timed_out && 2u * (u32)b->best_bid < fv)
            return 0;

        shot->buy = 0;
        shot->price = b->best_bid > t2->p.flatten_urgency_ticks
                    ? b->best_bid - t2->p.flatten_urgency_ticks : 1;
        i64 want = t2->inventory;
        if (want > (i64)t2->p.max_take_qty) want = t2->p.max_take_qty;
        shot->quantity = (u16)want;
        return 1;
    }

    if ((t2->inventory <= -(i64)t2->p.flatten_threshold || (timed_out && t2->inventory < 0))
        && b->have_ask) {
        // short: buy it back, but only at or below fair value
        if (!timed_out && 2u * (u32)b->best_ask > fv)
            return 0;

        shot->buy = 1;
        u32 px = (u32)b->best_ask + t2->p.flatten_urgency_ticks;
        shot->price = px > MAX_U16 ? MAX_U16 : (u16)px;
        i64 want = -t2->inventory;
        if (want > (i64)t2->p.max_take_qty) want = t2->p.max_take_qty;
        shot->quantity = (u16)want;
        return 1;
    }

    return 0;
}

// marketable limit + IOC: take what is there at the price we judged, drop the rest.
//
// deliberately NOT a plain market order. a market order has no cap, so if the liquidity we
// aimed at is gone by the time we arrive - which is exactly what happens when we lose the
// race - it keeps walking the book at any price. the limit is the price protection that
// makes losing a race cost nothing
static u8 t2_send_shot(T2* t2, Context* ctx, Order* out, T2Shot* shot) {
    out->status = (1 << IOC_BIT) | (shot->buy ? (1 << BUY_DIRECTION_BIT) : 0);
    out->price = shot->price;
    out->quantity = shot->quantity;

    t2->pending = 1;
    t2->pending_buy = shot->buy;
    t2->shot_id = ctx->next_order_id;
    return 1;
}

// "wake me in n ns" - action bit 2. the engine keeps only the earliest wake in flight and
// drops anything later, so we check what is already pending rather than ask to be dropped
static u8 t2_sleep(Context* ctx, u64 delay) {
    if (ctx->real_time_ns + delay >= ctx->next_wake_ns)
        return 0;
    ctx->wake_delay_ns = delay;
    return 2;
}

u8 t2_on_snapshot(T2* t2, Context* ctx) {
    Order* out = ctx->next_order_ptr;

    u8 settled = t2_settle(t2, ctx);
    if (settled == 1)
        return t2_sleep(ctx, t2->p.reject_backoff_ns);
    if (settled == 2)
        return t2_sleep(ctx, t2->p.missed_backoff_ns);

    // WAKE_EVERY_EVENT: the mbo stream is the wake model. we look at every book update but
    // fire on almost none of them - far fewer messages than a maker, every one aggressive
    if (!t2->connected) {
        out->status |= (1 << WS_BIT);
        t2->pending = 1;
        return 1;
    }

    // one shot in flight. firing again before we know how the last one went double-counts
    // the position, and this tier's whole posture is knowing exactly what it is carrying
    if (t2->pending)
        return t2_sleep(ctx, t2->p.idle_wake_ns);

    if (!ctx->is_open)
        return t2_sleep(ctx, t2->p.retry_wake_ns);

    T2Book book;
    t2_read_book(t2, ctx, &book);

    // no book, nothing to shoot at. unlike a maker we never act into the dark - the whole
    // strategy is a comparison against a quote we can actually see
    if (!book.have_bid && !book.have_ask)
        return t2_sleep(ctx, t2->p.idle_wake_ns);

    u32 fv = t2_fair_value_half_ticks(t2, ctx, &book);

    T2Shot shot;
    if (t2_find_edge(t2, &book, fv, &shot))
        return t2_send_shot(t2, ctx, out, &shot);

    if (t2_find_flatten(t2, ctx, &book, fv, &shot))
        return t2_send_shot(t2, ctx, out, &shot);

    return t2_sleep(ctx, t2->p.idle_wake_ns);
}

void t2_get_settings(T2* t2, ClientSettings* client_settings) {
    client_settings->initial_wake    = t2->p.initial_wake;
    client_settings->processing_time = t2->p.processing_time;
    client_settings->net_latency     = t2->p.net_latency;

    // margin, and not optionally: a pure taker that flattens instantly has to be able to
    // sell a stale-rich bid from flat and buy it back. this tier is broken in a cash-only
    // account model - it could only ever hit one side
    client_settings->is_cash_account = 0;
    client_settings->margin_mult     = t2->p.margin_mult;
    client_settings->maint_pct       = t2->p.maint_pct;
    client_settings->cash            = t2->p.cash;
    client_settings->reserved_cash   = 0;
    client_settings->shares          = 0;
    client_settings->reserved_shares = 0;

    // full depth of book: the displayed size at the touch is what sizes every shot
    client_settings->sub_tier = TIER_MBO;
    client_settings->noii     = 0;
}

void t2_free(T2* t2) {
    free(t2);
}
