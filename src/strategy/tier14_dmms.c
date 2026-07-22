#include <stdlib.h>
#include <stdio.h>

#include "strategy/tier14_dmms.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"
#include "trade.h" // Imbalance: the NOII publication this tier trades against
#include "server.h"
#include "response.h"

// T14 - Specialists. the obligated quote, and the sink for the closing imbalance.
//
// the sim runs 0 for now. the real world has ~3-5 DMM firms covering every NYSE-listed
// name with one assigned per stock, so the correct count here is exactly 1 per symbol.
// Nasdaq is pure-electronic with no DMM at all, which is worth remembering before treating
// this tier as universal.

// DMM firms, loosely parodied. there are only a handful in reality
static const char* T14_NAMES[] = {
    "gts.specialist", "bastion.dmm", "virtuo.dmm", "jane.lane.dmm", "ibex.specialist",
};
static const u32 T14_NAME_COUNT = sizeof(T14_NAMES) / sizeof(T14_NAMES[0]);
static u32 t14_next_name = 0;

// the tunable block. EVERY value here is a placeholder - none of it is calibrated, and
// none of it should be tuned to make the tape "look right". that is the owner's job.
// a function rather than file scope so it can use the time constants: a const u64 is not
// a constant expression in C, so S_TO_NS cannot seed a static
static T14Params t14_defaults() {
    T14Params p;

    // the knob that decides how hard the close is pinned. see the header
    p.imbalance_offset_pct       = 50;    /* UNCALIBRATED */
    p.max_auction_commitment     = 20000; /* UNCALIBRATED */

    p.quote_obligation_pct       = 90;    /* UNCALIBRATED */
    p.intraday_half_spread_ticks = 2;     /* UNCALIBRATED */
    p.quote_size                 = 400;   /* UNCALIBRATED */

    p.inventory_limit            = 30000; /* UNCALIBRATED */
    p.inventory_skew_ticks       = 1;     /* UNCALIBRATED */

    p.requote_interval_ns        = 5 * S_TO_NS;  /* UNCALIBRATED */
    p.idle_wake_ns               = 10 * S_TO_NS; /* UNCALIBRATED */
    p.retry_wake_ns              = 5 * MIN_TO_NS;/* UNCALIBRATED */
    p.reject_backoff_ns          = 5 * S_TO_NS;  /* UNCALIBRATED */

    p.seed_price                 = 100;   /* UNCALIBRATED */

    // the engine hook that does not exist - reported, not built
    p.last_look_enabled          = 0;

    p.cash                       = 5000000000LL; /* UNCALIBRATED */
    p.margin_mult                = 4;     /* UNCALIBRATED */
    p.maint_pct                  = 25;    /* UNCALIBRATED */

    // these firms ARE the electronic market makers - same speed as a Flickerer
    p.processing_time            = 300;   /* UNCALIBRATED */
    p.net_latency                = 250;   /* UNCALIBRATED */
    p.initial_wake               = 13 * H_TO_NS; /* UNCALIBRATED */

    return p;
}

// no per-agent rng here: there is exactly ONE DMM per symbol, so there is no population
// to decorrelate. its behaviour is an obligation, not a draw

T14* t14_init() {
    T14* t14 = malloc(sizeof(T14));

    t14->p = t14_defaults();

    t14->bid_id = MAX_U32;
    t14->ask_id = MAX_U32;
    t14->bid_price = 0;
    t14->ask_price = 0;
    t14->quoted = 0;
    t14->offset_id = MAX_U32;
    t14->offset_sent = 0;
    t14->last_imbalance = 0;
    t14->last_imbalance_buy_side = 0;
    t14->last_ref_price = 0;
    t14->inventory = 0;
    t14->cash_guess = t14->p.cash;
    t14->last_bid = 0;
    t14->last_ask = 0;
    t14->have_bid = 0;
    t14->have_ask = 0;
    t14->pending = 0;
    t14->pending_kind = T14_PEND_NONE;
    t14->pending_id = MAX_U32;
    t14->connected = 0;

    // names go out in init order, so the roster is stable across runs
    t14->name_idx = t14_next_name % T14_NAME_COUNT;
    // each agent carries its own rng state, seeded off its slot. no shared global rng
    t14->rng = 0xff51afd7u * (t14_next_name + 1);
    t14_next_name++;

    return t14;
}

char* t14_get_name(T14* t14) {
    return (char*)T14_NAMES[t14->name_idx];
}

static u8 t14_sleep(T14* t14, Context* ctx, u64 delay) {
    (void)t14;
    if (ctx->real_time_ns + delay >= ctx->next_wake_ns)
        return 0;
    ctx->wake_delay_ns = delay;
    return 2;
}

static u8 t14_await(T14* t14, Context* ctx, u8 kind) {
    t14->pending = 1;
    t14->pending_kind = kind;
    t14->pending_id = ctx->next_order_id;
    return 1;
}

// the touch, off the MBO stream
static void t14_read_book(T14* t14, Context* ctx) {
    MBO* mbo = (MBO*)ctx->data_snapshot;
    if (!mbo)
        return;
    u8 hb = mbo->hi_bid_index != MAX_U16 && mbo->hi_bid_index < mbo->level_count;
    u8 ha = hb && (u32)mbo->hi_bid_index + 1 < mbo->level_count;
    if (hb) {
        t14->last_bid = (mbo->levels + mbo->hi_bid_index)->price;
        t14->have_bid = 1;
    }
    if (ha) {
        t14->last_ask = (mbo->levels + mbo->hi_bid_index + 1)->price;
        t14->have_ask = 1;
    }
}

// the NOII publication. this is the tier's core input, not a supplementary one - the whole
// auction behaviour is a response to what this says
static void t14_read_imbalance(T14* t14, Context* ctx) {
    Imbalance* im = (Imbalance*)ctx->data_snapshot;
    if (!im)
        return;
    t14->last_imbalance = im->imbalance;
    t14->last_imbalance_buy_side = im->buy_side;
    t14->last_ref_price = im->ref_price ? im->ref_price : im->clearing;
}

static u8 t14_settle(T14* t14, Context* ctx) {
    u8 is_ws     = (ctx->status >> WS_BIT) & 1;
    u8 is_fill   = (ctx->status >> FILL_BIT) & 1;
    u8 is_reject = (ctx->status >> REJECT_BIT) & 1;
    u16 px = ctx->price ? ctx->price : ctx->mark;

    if (is_ws)
        t14->connected = 1;

    // a resting quote leg being hit, under its own id. as a maker this is the normal case
    if (is_fill && ctx->quantity_filled) {
        i64 q = (i64)ctx->quantity_filled;
        if (ctx->order_id == t14->bid_id) {
            t14->inventory += q;
            t14->cash_guess -= q * (i64)px;
        } else if (ctx->order_id == t14->ask_id) {
            t14->inventory -= q;
            t14->cash_guess += q * (i64)px;
        } else if (ctx->order_id == t14->offset_id) {
            // the auction cross. the offset was contra to the imbalance, so a buy-heavy
            // imbalance leaves the DMM SHORT and a sell-heavy one leaves it long
            if (t14->last_imbalance_buy_side) {
                t14->inventory -= q;
                t14->cash_guess += q * (i64)px;
            } else {
                t14->inventory += q;
                t14->cash_guess -= q * (i64)px;
            }
            t14->offset_id = MAX_U32;
        }
    }

    if (t14->pending && ctx->order_id == t14->pending_id) {
        u8 kind = t14->pending_kind;
        t14->pending = 0;
        t14->pending_kind = T14_PEND_NONE;
        t14->pending_id = MAX_U32;

        if (kind == T14_PEND_QUOTE) {
            if (is_reject) {
                t14->quoted = 0;
                return 1;
            }
            // the pair lands as one response: bid under order_id, ask under second_order_id
            t14->bid_id = ctx->order_id;
            t14->ask_id = ctx->second_order_id;
            t14->quoted = 1;
        } else if (kind == T14_PEND_OFFSET) {
            if (is_reject) {
                t14->offset_id = MAX_U32;
                return 1;
            }
            t14->offset_id = ctx->order_id;
        } else if (kind == T14_PEND_CANCEL) {
            t14->quoted = 0;
            t14->bid_id = MAX_U32;
            t14->ask_id = MAX_U32;
            if (is_reject)
                return 1;
        }
    }

    return 0;
}

// the continuous obligation: a two-sided quote around the reference, skewed to shed
// inventory rather than pulled. a DMM that widens out of the market when it is long is a
// DMM failing its obligation, so the lean is the only tool it has
static u8 t14_send_quote(T14* t14, Context* ctx, Order* out, u8 replacing) {
    u16 ref = ctx->mark ? ctx->mark : t14->p.seed_price;
    i64 half = (i64)t14->p.intraday_half_spread_ticks;

    i64 skew = 0;
    if (t14->p.inventory_limit > 0) {
        // long -> lean the whole quote down so the ask is more attractive than the bid
        skew = (t14->inventory * (i64)t14->p.inventory_skew_ticks)
             / (i64)t14->p.inventory_limit;
    }

    i64 bid = (i64)ref - half - skew;
    i64 ask = (i64)ref + half - skew;
    if (bid < 1) bid = 1;
    if (ask <= bid) ask = bid + 1;
    if (ask > MAX_U16) ask = MAX_U16;

    u16 bid_qty = t14->p.quote_size;
    u16 ask_qty = t14->p.quote_size;
    // at the inventory limit the obligated side shrinks but never disappears - that is the
    // difference between an obligation and a strategy
    if (t14->inventory >= (i64)t14->p.inventory_limit)
        bid_qty = bid_qty / 4 + 1;
    if (t14->inventory <= -(i64)t14->p.inventory_limit)
        ask_qty = ask_qty / 4 + 1;

    out->status = (1 << ASK_BID_PAIR_BIT) | (1 << BUY_DIRECTION_BIT) |
                  (replacing ? (1 << CAN_REP_BIT) : 0);
    if (replacing) {
        out->other_id = t14->bid_id;
        out->second_id = t14->ask_id;
    }
    out->price = (u16)bid;
    out->quantity = bid_qty;
    out->second_price = (u16)ask;
    out->second_quantity = ask_qty;

    t14->bid_price = (u16)bid;
    t14->ask_price = (u16)ask;
    return t14_await(t14, ctx, T14_PEND_QUOTE);
}

// the auction moment. the published imbalance says which side the cross is short of, and
// the DMM stands up on the OTHER side of it - which is exactly what an auction-only order
// contra to the imbalance is. the engine's offset-only cutoff already enforces the contra
// rule, so no engine change is needed for this
static u8 t14_send_offset(T14* t14, Context* ctx, Order* out) {
    i64 want = (i64)t14->last_imbalance * (i64)t14->p.imbalance_offset_pct / 100;
    if (want > (i64)t14->p.max_auction_commitment)
        want = t14->p.max_auction_commitment;
    if (want > MAX_U16)
        want = MAX_U16;
    if (want <= 0)
        return t14_sleep(t14, ctx, t14->p.idle_wake_ns);

    // contra: buy-heavy imbalance needs sell interest, and the other way round
    u8 buy = !t14->last_imbalance_buy_side;

    out->status = (1 << AUCTION_ONLY_BIT) | (buy ? (1 << BUY_DIRECTION_BIT) : 0);
    out->price = t14->last_ref_price ? t14->last_ref_price : ctx->mark;
    out->quantity = (u16)want;

    t14->offset_sent = 1;
    return t14_await(t14, ctx, T14_PEND_OFFSET);
}

u8 t14_on_snapshot(T14* t14, Context* ctx) {
    Order* out = ctx->next_order_ptr;

    u8 settled = t14_settle(t14, ctx);
    if (settled == 1)
        return t14_sleep(t14, ctx, t14->p.reject_backoff_ns);

    // the imbalance feed and the book arrive on the same channel; which one this is
    // depends on the phase, and an accumulation window is when NOII publishes
    if (ctx->auctioning)
        t14_read_imbalance(t14, ctx);
    else
        t14_read_book(t14, ctx);

    if (!t14->connected) {
        out->status |= (1 << WS_BIT);
        t14->pending = 1;
        t14->pending_id = ctx->next_order_id;
        t14->pending_kind = T14_PEND_NONE;
        return 1;
    }

    if (t14->pending)
        return t14_sleep(t14, ctx, t14->p.idle_wake_ns);

    // ACCUMULATION WINDOW. the defining moment of the tier
    if (ctx->auctioning) {
        if (!ctx->auction_frozen && !t14->offset_sent && t14->last_imbalance > 0)
            return t14_send_offset(t14, ctx, out);
        return t14_sleep(t14, ctx, t14->p.idle_wake_ns);
    }
    t14->offset_sent = 0;

    if (!ctx->is_open)
        return t14_sleep(t14, ctx, t14->p.retry_wake_ns);

    // the continuous obligation. quote, and keep quoting - the whole franchise is that it
    // does not get to stop when things are unpleasant
    u16 ref = ctx->mark ? ctx->mark : t14->p.seed_price;
    if (!t14->quoted)
        return t14_send_quote(t14, ctx, out, 0);

    i64 drift = (i64)ref - ((i64)t14->bid_price + (i64)t14->ask_price) / 2;
    if (drift < 0)
        drift = -drift;
    if (drift >= (i64)t14->p.intraday_half_spread_ticks)
        return t14_send_quote(t14, ctx, out, 1);

    return t14_sleep(t14, ctx, t14->p.idle_wake_ns);
}

void t14_get_settings(T14* t14, ClientSettings* client_settings) {
    client_settings->initial_wake    = t14->p.initial_wake;
    client_settings->processing_time = t14->p.processing_time;
    client_settings->net_latency     = t14->p.net_latency;

    // margin. an obligated two-sided quote has to be able to end up short, and absorbing a
    // buy-heavy close does exactly that
    client_settings->is_cash_account = 0;
    client_settings->margin_mult     = t14->p.margin_mult;
    client_settings->maint_pct       = t14->p.maint_pct;
    client_settings->cash            = t14->p.cash;
    client_settings->reserved_cash   = 0;
    client_settings->shares          = 0;
    client_settings->reserved_shares = 0;

    // full depth for the continuous quote, PLUS the NOII add-on. the imbalance feed is the
    // core input to the auction behaviour, so this is the one tier that pays for it
    client_settings->sub_tier = TIER_MBO;
    client_settings->noii     = 1;
}

void t14_free(T14* t14) {
    free(t14);
}
