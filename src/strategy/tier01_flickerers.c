#include <stdlib.h>
#include <stdio.h>

#include "strategy/tier01_flickerers.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"
#include "server.h"

// T1 - Flickerers. the apex makers: quote both sides continuously, skew away from
// inventory, requote on queue slip / a mid move / age, widen after a sweep.
//
// the sim runs 8 of these. us equities has 20-50 registered mms but only a handful
// that move the needle - citadel securities ~25% of volume, virtu ~20%, then jane
// street, jump, hrt, imc, optiver, two sigma. 8 is that handful.

// firm names, loosely parodied. one per instance, handed out in init order
static const char* T1_NAMES[] = {
    "bastion.securities",
    "virtuo.capital",
    "plain.street",
    "vault.trading",
    "hudson.creek",
    "imk.markets",
    "optivar",
    "three.sigma",
};
static const u32 T1_NAME_COUNT = sizeof(T1_NAMES) / sizeof(T1_NAMES[0]);
static u32 t1_next_name = 0;

// the tunable block. EVERY value here is a placeholder - none of it is calibrated, and
// none of it should be tuned to make the tape "look right". that is the owner's job.
// it lives in a function rather than at file scope so it can use the time constants:
// a const u64 is not a constant expression in C, so S_TO_NS cannot seed a static.
static T1Params t1_defaults() {
    T1Params p;

    p.quote_size                   = 200;   /* UNCALIBRATED */
    p.base_half_spread_ticks       = 2;     /* UNCALIBRATED */
    p.min_half_spread_ticks        = 1;     /* UNCALIBRATED */

    p.inventory_limit              = 2000;  /* UNCALIBRATED */
    p.skew_coeff                   = 1;     /* UNCALIBRATED */
    p.skew_unit                    = 400;   /* UNCALIBRATED */
    // ~10x the base half-spread: wide enough to strongly discourage growing the capped
    // side, near enough that informed flow will still lift it to move price
    p.cap_defensive_ticks          = 20;    /* UNCALIBRATED */

    p.requote_queue_slip_threshold = 1000;  /* UNCALIBRATED */
    p.max_quote_age_ns             = S_TO_NS / 2000;  /* UNCALIBRATED */ // 500us
    p.min_requote_interval_ns      = S_TO_NS / 20000; /* UNCALIBRATED */ // 50us
    p.requote_move_ticks           = 1;     /* UNCALIBRATED */

    p.idle_wake_ns                 = 1 * S_TO_NS;   /* UNCALIBRATED */
    p.retry_wake_ns                = 1 * MIN_TO_NS; /* UNCALIBRATED */
    p.reject_backoff_ns            = 1 * S_TO_NS;   /* UNCALIBRATED */

    p.sweep_pull_threshold         = 500;   /* UNCALIBRATED */
    p.sweep_cooldown_ns            = S_TO_NS / 500; /* UNCALIBRATED */ // 2ms
    p.sweep_widen_ticks            = 4;     /* UNCALIBRATED */

    p.seed_price                   = 100;   /* UNCALIBRATED */

    p.cash                         = 1000000000; /* UNCALIBRATED */
    p.margin_mult                  = 4;     /* UNCALIBRATED */
    p.maint_pct                    = 25;    /* UNCALIBRATED */

    // tick-to-trade: fpga path 10-100ns, software single-digit us, colocated cross
    // connect keeps the wire sub-us
    p.processing_time              = 400;   /* UNCALIBRATED */
    p.net_latency                  = 300;   /* UNCALIBRATED */

    // on the desk well before the bell
    p.initial_wake                 = 13 * H_TO_NS; /* UNCALIBRATED */

    return p;
}


T1* t1_init() {
    T1* t1 = malloc(sizeof(T1));

    t1->p = t1_defaults();

    t1->bid_id = MAX_U32;
    t1->ask_id = MAX_U32;
    t1->bid_price = 0;
    t1->ask_price = 0;
    t1->quoted_ns = 0;
    t1->pending = 0;
    t1->pending_kind = T1_PEND_NONE;
    t1->pending_id = MAX_U32;
    t1->inventory = 0;
    t1->cash_guess = t1->p.cash;
    t1->last_bid_depth = 0;
    t1->last_ask_depth = 0;
    t1->sweep_until_ns = 0;
    t1->connected = 0;
    // names go out in init order, so the roster is stable across runs
    t1->name_idx = t1_next_name % T1_NAME_COUNT;
    // each agent carries its own rng state, seeded off its slot. no shared global rng
    t1->rng = 0x9e3779b9u * (t1_next_name + 1);
    t1_next_name++;

    return t1;
}

char* t1_get_name(T1* t1) {
    return (char*)T1_NAMES[t1->name_idx];
}

// both quotes shift by this many ticks, signed, away from the position we carry.
// long -> negative -> both quotes drop, which invites someone to sell to us
static i32 t1_skew_ticks(T1* t1) {
    if (t1->p.skew_unit == 0)
        return 0;
    return -(i32)((t1->inventory * (i64)t1->p.skew_coeff) / (i64)t1->p.skew_unit);
}

// "wake me in n ns" - action bit 2. book events drive us, but a shut market and an
// empty book both send none, so we have to arrange our own next look.
//
// the engine keeps only the earliest wake in flight and drops anything later, so asking
// on every event can't breed wakes. we still check ctx->next_wake_ns first: if what we
// want is already covered there is no point sending a request just to have it dropped
static u8 t1_sleep(Context* ctx, u64 delay) {
    if (ctx->real_time_ns + delay >= ctx->next_wake_ns)
        return 0;
    ctx->wake_delay_ns = delay;
    return 2;
}

// what we could read off the top of the book this wake. a snapshotless wake (a ping, a
// self-wake) still produces a valid one of these with have_bid/have_ask clear
typedef struct T1Book {
    MBO* mbo;
    u16 best_bid;
    u16 best_ask;
    u32 bid_depth;
    u32 ask_depth;
    u8 have_bid;
    u8 have_ask;
} T1Book;

// the quote we would like to be showing right now
typedef struct T1Quote {
    u16 bid;
    u16 ask;
    u16 bid_qty;
    u16 ask_qty;
} T1Quote;

// fold this response into our own books. the engine tells us about fills and acks; the
// position, the resting ids and the cash are ours to keep straight.
// returns 1 if the response was a reject we should back off from
static u8 t1_settle(T1* t1, Context* ctx) {
    u8 is_ping      = (ctx->status >> PING_BIT) & 1;
    u8 is_ws        = (ctx->status >> WS_BIT) & 1;
    u8 is_fill      = (ctx->status >> FILL_BIT) & 1;
    u8 is_partial   = (ctx->status >> PARTIAL_FILL_BIT) & 1;
    u8 is_reject    = (ctx->status >> REJECT_BIT) & 1;
    u8 full_fill    = is_fill && !is_partial;

    if (is_ws)
        t1->connected = 1;

    // ---- the ack for whatever we last sent ----
    //
    // keyed on the message id we kept, not on our resting ids: a cancel's response comes
    // back under the cancel message's own id and would match neither leg, so keying off
    // the legs alone means never clearing pending and going silent for good
    if (t1->pending && ctx->order_id == t1->pending_id) {
        u8 kind = t1->pending_kind;
        t1->pending = 0;
        t1->pending_kind = T1_PEND_NONE;
        t1->pending_id = MAX_U32;

        if (kind == T1_PEND_PULL) {
            // both legs are gone whether or not the pull was accepted: if it was rejected
            // the ids were already stale, which is the only way a valid cancel fails
            t1->bid_id = MAX_U32;
            t1->ask_id = MAX_U32;
            return 0;
        }
        if (kind == T1_PEND_PULL_BID) {
            t1->bid_id = MAX_U32;
            return 0;
        }
        if (kind == T1_PEND_PULL_ASK) {
            t1->ask_id = MAX_U32;
            return 0;
        }

        if (kind == T1_PEND_QUOTE) {
            if (is_reject) {
                // nothing rested, and main.c has freed the slot
                t1->bid_id = MAX_U32;
                t1->ask_id = MAX_U32;
                return 1;
            }

            // the ask leg's id is minted server side, so this is the first moment we can
            // possibly know it
            t1->bid_id = ctx->order_id;
            t1->ask_id = ctx->second_order_id;
            t1->bid_price = ctx->price;
            t1->ask_price = ctx->second_price;
            t1->quoted_ns = ctx->real_time_ns;

            // a pair cannot cross on entry, but settle anything reported anyway
            t1->inventory += (i64)ctx->quantity_filled;
            t1->cash_guess -= (i64)ctx->quantity_filled * (i64)ctx->price;
            t1->inventory -= (i64)ctx->second_quantity_filled;
            t1->cash_guess += (i64)ctx->second_quantity_filled * (i64)ctx->second_price;
            return 0;
        }

        // a ws / ping ack. nothing of ours rested
        return is_reject;
    }

    // ---- an unsolicited report on a quote already resting: someone traded with us ----

    if (t1->bid_id != MAX_U32 && ctx->order_id == t1->bid_id) {
        if (is_fill) {
            t1->inventory += (i64)ctx->quantity_filled;
            t1->cash_guess -= (i64)ctx->quantity_filled * (i64)ctx->price;
        }
        if (full_fill || is_reject)
            t1->bid_id = MAX_U32;
        t1->pending = 0;
    } else if (t1->ask_id != MAX_U32 && ctx->order_id == t1->ask_id) {
        if (is_fill) {
            t1->inventory -= (i64)ctx->quantity_filled;
            t1->cash_guess += (i64)ctx->quantity_filled * (i64)ctx->price;
        }
        if (full_fill || is_reject)
            t1->ask_id = MAX_U32;
        t1->pending = 0;
    } else if (is_reject || is_ws || is_ping) {
        // a rejected single order, or a control ack. nothing of ours rested
        t1->pending = 0;
    }

    return is_reject;
}

// top of book, if this wake carried one.
//
// a ping carries no snapshot, and on an empty book there is nothing to broadcast either,
// so a client that insists on seeing a book before quoting waits for a book that only
// exists once somebody quotes. everything downstream tolerates mbo == 0 instead
static void t1_read_book(Context* ctx, T1Book* b) {
    MBO* mbo = (MBO*)ctx->data_snapshot;

    b->mbo = mbo;
    b->best_bid = 0;
    b->best_ask = 0;
    b->bid_depth = 0;
    b->ask_depth = 0;
    b->have_bid = mbo && mbo->hi_bid_index != MAX_U16 && mbo->hi_bid_index < mbo->level_count;
    b->have_ask = b->have_bid && (u32)mbo->hi_bid_index + 1 < mbo->level_count;

    if (b->have_bid) {
        MBOIndex* bid = mbo->levels + mbo->hi_bid_index;
        b->best_bid = bid->price;
        b->bid_depth = bid->quantity;
    }
    if (b->have_ask) {
        MBOIndex* ask = mbo->levels + mbo->hi_bid_index + 1;
        b->best_ask = ask->price;
        b->ask_depth = ask->quantity;
    }
}

// a top level draining hard in one update is someone taking through us. widen for the
// cooldown instead of re-posting into the same adverse flow - being picked off during a
// sweep is the whole thing this agent exists to avoid.
//
// only ever compare against a real book: a snapshotless wake would read as "depth went to
// zero" and fake a sweep on every heartbeat
static void t1_track_sweep(T1* t1, Context* ctx, T1Book* b) {
    if (!b->mbo)
        return;

    if ((t1->last_bid_depth > b->bid_depth &&
         t1->last_bid_depth - b->bid_depth >= t1->p.sweep_pull_threshold) ||
        (t1->last_ask_depth > b->ask_depth &&
         t1->last_ask_depth - b->ask_depth >= t1->p.sweep_pull_threshold))
        t1->sweep_until_ns = ctx->real_time_ns + t1->p.sweep_cooldown_ns;

    t1->last_bid_depth = b->bid_depth;
    t1->last_ask_depth = b->ask_depth;
}

// what we quote around: the mid if there are two sides, else the last print, else the
// seed. the book opens empty and nothing has printed, so somebody has to go first
static u32 t1_reference_price(T1* t1, Context* ctx, T1Book* b) {
    if (b->have_bid && b->have_ask)
        return ((u32)b->best_bid + (u32)b->best_ask) >> 1;
    if (ctx->mark != 0)
        return ctx->mark;
    if (b->have_bid)
        return b->best_bid;
    return t1->p.seed_price;
}

// the two-sided quote we want up right now: a half-spread either side of the reference,
// both legs shifted by the inventory skew, clamped into something the book will accept
static void t1_target_quote(T1* t1, Context* ctx, u32 ref, T1Quote* q) {
    u32 half = t1->p.base_half_spread_ticks;
    if (ctx->real_time_ns < t1->sweep_until_ns)
        half += t1->p.sweep_widen_ticks;
    if (half < t1->p.min_half_spread_ticks)
        half = t1->p.min_half_spread_ticks;

    i32 skew = t1_skew_ticks(t1);

    i64 bid = (i64)ref - (i64)half + skew;
    i64 ask = (i64)ref + (i64)half + skew;

    // inventory caps. rather than pull the whole quote and go dark - which empties the book
    // and can freeze the entire market when every maker caps the same way at once - keep a
    // two-sided market and push the CAPPED side to a defensive width. that side (mostly)
    // stops growing, but a quote still shows there, and informed flow lifting that wide
    // quote is the price discovery that walks the market back toward value after a shock
    u8 want_bid = t1->inventory < (i64)t1->p.inventory_limit;
    u8 want_ask = t1->inventory > -(i64)t1->p.inventory_limit;
    if (!want_bid)
        bid = (i64)ref - (i64)t1->p.cap_defensive_ticks; // long-capped: bid far below
    if (!want_ask)
        ask = (i64)ref + (i64)t1->p.cap_defensive_ticks; // short-capped: ask far above

    // the pair op demands bid strictly below ask, and price 0 is reserved. integer skew
    // can collapse or invert the quote, which is the locked-market landmine
    if (bid < 1)
        bid = 1;
    if (ask <= bid)
        ask = bid + 1;
    if (ask > MAX_U16) {
        ask = MAX_U16;
        if (bid >= ask)
            bid = ask - 1;
    }

    q->bid = (u16)bid;
    q->ask = (u16)ask;

    // quote size is a constant, never a function of inventory. sizing the ask off the
    // position is the classic bug - it shrinks exactly when we most need to sell
    q->bid_qty = t1->p.quote_size;
    q->ask_qty = t1->p.quote_size;
}

// remember the message we just handed over, so its ack can be matched and read
static u8 t1_await(T1* t1, Context* ctx, u8 kind) {
    t1->pending = 1;
    t1->pending_kind = kind;
    t1->pending_id = ctx->next_order_id;
    return 1;
}

// exactly one leg resting means an ack raced a fill. pull the survivor on its own, so we
// never end up running two bids or two asks
static u8 t1_send_pull_stray(T1* t1, Context* ctx, Order* out) {
    u8 pulling_bid = t1->bid_id != MAX_U32;
    out->status = (1 << CANCEL_BIT) | (pulling_bid ? (1 << BUY_DIRECTION_BIT) : 0);
    out->other_id = pulling_bid ? t1->bid_id : t1->ask_id;
    out->price = pulling_bid ? t1->bid_price : t1->ask_price;
    return t1_await(t1, ctx, pulling_bid ? T1_PEND_PULL_BID : T1_PEND_PULL_ASK);
}

// a fresh two-sided quote, or - with CAN_REP_BIT - both old quotes out and both new ones
// in atomically. as two separate orders there is a window where we are one-sided at a
// price we no longer believe
static u8 t1_send_quote(T1* t1, Context* ctx, Order* out, T1Quote* q, u8 replacing) {
    out->status = (1 << ASK_BID_PAIR_BIT) | (1 << BUY_DIRECTION_BIT) |
                  (replacing ? (1 << CAN_REP_BIT) : 0);
    if (replacing) {
        out->other_id = t1->bid_id;
        out->second_id = t1->ask_id;
    }
    out->price = q->bid;
    out->quantity = q->bid_qty;
    out->second_price = q->ask;
    out->second_quantity = q->ask_qty;
    return t1_await(t1, ctx, T1_PEND_QUOTE);
}

// our quote is resting and we have decided to replace it. before sending, look at how
// much size sits in front of us and take the more aggressive tick on any buried side.
//
// note what this deliberately does NOT do: fire a requote on slip by itself. a replace at
// the same price re-times to the BACK of that level - it makes the slip worse, then reads
// as slip again next wake, and eight makers do that to each other forever. slip is only
// ever a reason to pick a better price, never a reason to act.
// MAX_U32 is also what ob_queue_position returns for "not in the book", so a wake with no
// snapshot reads as unknown and simply doesn't adjust anything
static void t1_apply_queue_slip(T1* t1, T1Book* b, T1Quote* q) {
    u32 bid_ahead = b->mbo ? ob_queue_position(t1->bid_price, t1->bid_id, b->mbo) : MAX_U32;
    u32 ask_ahead = b->mbo ? ob_queue_position(t1->ask_price, t1->ask_id, b->mbo) : MAX_U32;
    u32 floor_width = 2u * t1->p.min_half_spread_ticks;

    if (bid_ahead != MAX_U32 && bid_ahead >= t1->p.requote_queue_slip_threshold &&
        (u32)(q->ask - q->bid) > floor_width)
        q->bid++;
    if (ask_ahead != MAX_U32 && ask_ahead >= t1->p.requote_queue_slip_threshold &&
        (u32)(q->ask - q->bid) > floor_width)
        q->ask--;
}

// our quote is resting: is it still the one we want?
static u8 t1_maintain_quote(T1* t1, Context* ctx, T1Book* b, T1Quote* q, Order* out) {
    // eight makers all reacting to each other's updates will requote forever if nothing
    // rate limits them. quote-to-trade of 100:1 is realistic, infinity is not
    if ((ctx->real_time_ns - t1->quoted_ns) < t1->p.min_requote_interval_ns)
        return t1_sleep(ctx, t1->p.idle_wake_ns);

    u16 bid_move = q->bid > t1->bid_price ? q->bid - t1->bid_price : t1->bid_price - q->bid;
    u16 ask_move = q->ask > t1->ask_price ? q->ask - t1->ask_price : t1->ask_price - q->ask;
    u8 moved = (bid_move >= t1->p.requote_move_ticks) || (ask_move >= t1->p.requote_move_ticks);

    // an aged quote is worth a fresh look, but re-posting one that already sits where we
    // want it only surrenders queue position: the book re-times any replace that isn't a
    // strict size reduction, so a no-change requote costs us the queue and buys nothing.
    // age alone therefore never sends an order - we restamp it and carry on
    if (!moved) {
        if ((ctx->real_time_ns - t1->quoted_ns) >= t1->p.max_quote_age_ns)
            t1->quoted_ns = ctx->real_time_ns;
        return t1_sleep(ctx, t1->p.idle_wake_ns);
    }

    t1_apply_queue_slip(t1, b, q);
    return t1_send_quote(t1, ctx, out, q, 1);
}

u8 t1_on_snapshot(T1* t1, Context* ctx) {
    Order* out = ctx->next_order_ptr;

    // back off rather than fire the same quote straight back - a standing reason to reject
    // (no shares, no buying power) does not clear in the time it takes us to resend
    if (t1_settle(t1, ctx))
        return t1_sleep(ctx, t1->p.reject_backoff_ns);

    // the feed IS the wake model: an mbo stream delivers one wake per book update, which
    // is WAKE_EVERY_EVENT. until we have it we are blind.
    // a ws toggle carries no quantity and clears ahead of the auction diversion, so this
    // works in any session phase - we can be on the feed well before the bell
    if (!t1->connected) {
        out->status |= (1 << WS_BIT);
        return t1_await(t1, ctx, T1_PEND_WS);
    }

    // a quote of ours is still in flight. sending another mints ids we can never cancel -
    // this is the spin the old mm had
    if (t1->pending)
        return t1_sleep(ctx, t1->p.idle_wake_ns);

    // we only make a market in continuous trading. the closing window keeps the book live
    // (is_open stays 1 through it) so we quote right through that; the pre-open window and
    // the overnight are dead to us, and neither sends events, so we sleep on a timer
    if (!ctx->is_open)
        return t1_sleep(ctx, t1->p.retry_wake_ns);

    T1Book book;
    t1_read_book(ctx, &book);
    t1_track_sweep(t1, ctx, &book);

    T1Quote quote;
    t1_target_quote(t1, ctx, t1_reference_price(t1, ctx, &book), &quote);

    u8 resting = (t1->bid_id != MAX_U32) && (t1->ask_id != MAX_U32);

    // no cap branch that pulls: t1_target_quote already widened any capped side, so we
    // always keep a two-sided market up and never withdraw liquidity entirely
    if (resting)
        return t1_maintain_quote(t1, ctx, &book, &quote, out);

    if (t1->bid_id != MAX_U32 || t1->ask_id != MAX_U32)
        return t1_send_pull_stray(t1, ctx, out);

    return t1_send_quote(t1, ctx, out, &quote, 0);
}

void t1_get_settings(T1* t1, ClientSettings* client_settings) {
    client_settings->initial_wake    = t1->p.initial_wake;
    client_settings->processing_time = t1->p.processing_time;
    client_settings->net_latency     = t1->p.net_latency;

    // a maker carries inventory both ways and flattens to hedge, so it is never a cash
    // account. risk is measured in inventory units, not dollars
    client_settings->is_cash_account = 0;
    client_settings->margin_mult     = t1->p.margin_mult;
    client_settings->maint_pct       = t1->p.maint_pct;
    client_settings->cash            = t1->p.cash;
    client_settings->reserved_cash   = 0;
    client_settings->shares          = 0;
    client_settings->reserved_shares = 0;

    // full depth of book, by order. queue position is not derivable from anything less
    client_settings->sub_tier = TIER_MBO;
    client_settings->noii     = 0;
}

void t1_free(T1* t1) {
    free(t1);
}
