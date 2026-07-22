#include <stdlib.h>
#include <stdio.h>

#include "strategy/tier03_slicers.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"
#include "mbp.h" // MBP1: the L1 touch, this tier's data feed
#include "server.h"
#include "response.h"

// T3 - Slicers. work a parent order into the book as a stream of children, comparing
// filled against a schedule each wake: post passive when on track, cross when behind.
//
// the sim runs 40. the real count is ~100s of algo providers and ~1000s of systematic
// funds, but the unit that matters is PARENT ORDERS IN FLIGHT, not firms - one desk runs
// many at once. the design doc puts the tier at 50-150 in sim; 40 against 8 makers and 12
// snipers keeps the book from being swamped by a single tier while still producing a
// continuous, persistent, same-side child stream.

// broker / algo-desk names, loosely parodied. one per instance, handed out in init order
static const char* T3_NAMES[] = {
    "goldstein.exec",    // the bulge-bracket algo desks
    "morgan.stanhope",
    "barcloud.algo",
    "credit.suess",
    "deutsche.banc",
    "nomora.exec",
    "jefferys.electronic",
    "cowan.algo",
    "instinet.prime",    // the agency brokers and vendors
    "itg.triton",
    "liquidnet.dark",
    "virtu.exec",
};
static const u32 T3_NAME_COUNT = sizeof(T3_NAMES) / sizeof(T3_NAMES[0]);
static u32 t3_next_name = 0;

// VWAP and POV both need the REALIZED VOLUME of the market - what everyone else traded -
// to size the next child. a client subscribes to exactly one stream (server.c builds the
// roster by a single sub_tier), so an algo can watch the book or the tape, not both, and
// these two cannot be built correctly today. per the design doc's gating rule they are
// defined and refuse to run rather than being faked off a depth proxy.
static u8 t3_algo_supported(t3_algo_t algo) {
    return algo == T3_TWAP || algo == T3_IS;
}

// the tunable block. EVERY value here is a placeholder - none of it is calibrated, and
// none of it should be tuned to make the tape "look right". that is the owner's job.
// a function rather than file scope so it can use the time constants: a const u64 is not
// a constant expression in C, so S_TO_NS cannot seed a static
static T3Params t3_defaults() {
    T3Params p;

    p.algo_type              = T3_TWAP;  /* UNCALIBRATED */

    // the doc puts real parents at 100k-1M+ shares. this is far smaller on purpose: the
    // whole book is currently ~1600 shares deep, so a real-sized parent would never finish
    // and the tier would be indistinguishable from one that does nothing
    p.parent_qty             = 4000;     /* UNCALIBRATED */
    p.horizon_ns             = 4 * H_TO_NS; /* UNCALIBRATED */

    p.child_size             = 200;      /* UNCALIBRATED */
    p.child_interval_ns      = 30 * S_TO_NS; /* UNCALIBRATED */

    p.participation_cap_pct  = 25;       /* UNCALIBRATED */
    p.catch_up_threshold     = 400;      /* UNCALIBRATED */
    p.max_slippage_ticks     = 10;       /* UNCALIBRATED */
    p.aggression_half_pct    = 65;       /* UNCALIBRATED */

    p.passive_offset_ticks   = 0;        /* UNCALIBRATED */
    p.passive_chase_ticks    = 2;        /* UNCALIBRATED */

    p.seed_price             = 100;      /* UNCALIBRATED */

    p.slice_disabled         = 0;        /* UNCALIBRATED */

    p.auto_parent            = 1;        /* UNCALIBRATED */
    p.restart_delay_ns       = 30 * MIN_TO_NS; /* UNCALIBRATED */
    // a couple of parents worth of drift before the generated flow leans back
    p.inventory_balance_qty  = 8000;     /* UNCALIBRATED */

    p.idle_wake_ns           = 30 * S_TO_NS; /* UNCALIBRATED */
    p.retry_wake_ns          = 5 * MIN_TO_NS; /* UNCALIBRATED */
    p.reject_backoff_ns      = 5 * S_TO_NS;  /* UNCALIBRATED */

    p.cash                   = 1000000000; /* UNCALIBRATED */
    p.margin_mult            = 2;        /* UNCALIBRATED */
    p.maint_pct              = 30;       /* UNCALIBRATED */

    // milliseconds. not racing anyone - near-colo, but a nanosecond edge buys an algo
    // nothing when its schedule is measured in hours
    p.processing_time        = 2 * (S_TO_NS / 1000);  /* UNCALIBRATED */ // 2ms
    p.net_latency            = 3 * (S_TO_NS / 1000);  /* UNCALIBRATED */ // 3ms

    p.initial_wake           = 13 * H_TO_NS; /* UNCALIBRATED */

    return p;
}

T3* t3_init() {
    T3* t3 = malloc(sizeof(T3));

    t3->p = t3_defaults();

    t3->have_parent = 0;
    t3->parent_buy = 0;
    t3->parent_qty = 0;
    t3->parent_filled = 0;
    t3->parent_start_ns = 0;

    t3->passive_id = MAX_U32;
    t3->passive_price = 0;
    t3->passive_qty = 0;

    t3->pending = 0;
    t3->pending_kind = T3_PEND_NONE;
    t3->pending_id = MAX_U32;
    t3->pending_qty = 0;

    t3->cash_guess = t3->p.cash;
    t3->inventory = 0;

    t3->have_book = 0;
    t3->booted = 0;
    t3->book_have_bid = 0;
    t3->book_have_ask = 0;
    t3->last_bid = 0;
    t3->last_ask = 0;
    t3->last_bid_depth = 0;
    t3->last_ask_depth = 0;
    // names go out in init order, so the roster is stable across runs
    t3->name_idx = t3_next_name % T3_NAME_COUNT;
    // each agent carries its own rng state, seeded off its slot. no shared global rng
    t3->rng = 0xc2b2ae35u * (t3_next_name + 1);
    t3_next_name++;

    return t3;
}

char* t3_get_name(T3* t3) {
    return (char*)T3_NAMES[t3->name_idx];
}

// xorshift, per agent. determinism is a hard rule: same seed in, same sequence out
static u32 t3_rand(T3* t3) {
    u32 x = t3->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t3->rng = x;
    return x;
}

// what we could read off the top of the book this wake
typedef struct T3Book {
    u16 best_bid;
    u16 best_ask;
    u32 bid_depth;
    u32 ask_depth;
    u8 have_bid;
    u8 have_ask;
} T3Book;

// "wake me in n ns" - action bit 2. WAKE_INTERVAL is this tier's whole wake model: it is
// not reacting to ticks, it is checking a clock against a schedule. the engine keeps only
// the earliest wake in flight, so asking when one is already sooner is just dropped
static u8 t3_sleep(Context* ctx, u64 delay) {
    if (ctx->real_time_ns + delay >= ctx->next_wake_ns)
        return 0;
    ctx->wake_delay_ns = delay;
    return 2;
}

// take on a new parent. side is drawn per agent so the population carries both directions -
// a tier that is uniformly one way is a demand shock, not an execution population
static void t3_new_parent(T3* t3, Context* ctx) {
    t3->have_parent = 1;

    // mix the engine's per-event draw into our own state. ctx->random advances with the
    // whole sim, so two agents waking at different moments get decorrelated draws without
    // either of them depending on a shared stream alone - the per-agent state is still what
    // makes a single agent reproducible
    t3->rng ^= ctx->random;
    t3->parent_buy = t3_rand(t3) & 1;

    // carrying more than we should: take the side that works it back down. with no routing
    // there is no client to deliver a finished parent to, so this is how the desk stays a
    // conduit instead of an unbounded directional book - see inventory_balance_qty
    if (t3->inventory > (i64)t3->p.inventory_balance_qty)
        t3->parent_buy = 0;
    else if (t3->inventory < -(i64)t3->p.inventory_balance_qty)
        t3->parent_buy = 1;
    t3->parent_qty = t3->p.parent_qty;
    t3->parent_filled = 0;
    t3->parent_start_ns = ctx->real_time_ns;

    // arrival price: what the market was when the client handed this over. every price we
    // are willing to trade at is measured from here, and shortfall is measured against it
    if (t3->have_book && t3->book_have_bid && t3->book_have_ask)
        t3->arrival_price = (u16)(((u32)t3->last_bid + (u32)t3->last_ask) >> 1);
    else if (ctx->mark)
        t3->arrival_price = ctx->mark;
    else
        t3->arrival_price = t3->p.seed_price;
}

// the worst price this parent may ever trade at. beyond it we do not trade - we under-fill
// and hand back the shortfall, which is the honest outcome when the market runs away
static u16 t3_limit_price(T3* t3) {
    if (t3->parent_buy) {
        u32 cap = (u32)t3->arrival_price + t3->p.max_slippage_ticks;
        return cap > MAX_U16 ? MAX_U16 : (u16)cap;
    }
    return t3->arrival_price > t3->p.max_slippage_ticks
         ? t3->arrival_price - t3->p.max_slippage_ticks : 1;
}

// is a price inside the band?
static u8 t3_within_band(T3* t3, u16 price) {
    u16 limit = t3_limit_price(t3);
    return t3->parent_buy ? price <= limit : price >= limit;
}

// fold this response into the parent's progress. fills are what this tier runs on: the
// schedule is tracked BY fills, so a missed one silently stalls the whole parent
static u8 t3_settle(T3* t3, Context* ctx) {
    u8 is_fill   = (ctx->status >> FILL_BIT) & 1;
    u8 is_partial = (ctx->status >> PARTIAL_FILL_BIT) & 1;
    u8 is_reject = (ctx->status >> REJECT_BIT) & 1;

    // credit any fill to the parent, whoever it belonged to - a resting child can fill
    // long after we stopped waiting on its ack
    if (is_fill && ctx->quantity_filled) {
        t3->parent_filled += ctx->quantity_filled;
        if (t3->parent_buy) {
            t3->inventory += (i64)ctx->quantity_filled;
            t3->cash_guess -= (i64)ctx->quantity_filled * (i64)ctx->price;
        } else {
            t3->inventory -= (i64)ctx->quantity_filled;
            t3->cash_guess += (i64)ctx->quantity_filled * (i64)ctx->price;
        }
    }

    // the resting child is gone: filled out, cancelled at the close, or rejected
    if (t3->passive_id != MAX_U32 && ctx->order_id == t3->passive_id) {
        if ((is_fill && !is_partial) || is_reject)
            t3->passive_id = MAX_U32;
    }

    if (t3->pending && ctx->order_id == t3->pending_id) {
        u8 kind = t3->pending_kind;
        t3->pending = 0;
        t3->pending_kind = T3_PEND_NONE;
        t3->pending_id = MAX_U32;

        if (kind == T3_PEND_PASSIVE) {
            if (is_reject) {
                t3->passive_id = MAX_U32;
                return 1;
            }
            // it rested (or partially filled and rested). remember it so we can chase it
            if (!(is_fill && !is_partial)) {
                t3->passive_id = ctx->order_id;
                t3->passive_price = ctx->price;
            }
        } else if (kind == T3_PEND_CANCEL) {
            t3->passive_id = MAX_U32;
        } else if (kind == T3_PEND_CROSS) {
            // an ioc that found nothing is a miss, not an error - we simply stay behind
            // schedule and try again next wake
            if (is_reject)
                return ctx->rej_reason == CXL_IOC_UNFILLED ? 0 : 1;
        } else if (is_reject) {
            return 1;
        }
    }

    return 0;
}

static u8 t3_await(T3* t3, Context* ctx, u8 kind, u16 qty) {
    t3->pending = 1;
    t3->pending_kind = kind;
    t3->pending_id = ctx->next_order_id;
    t3->pending_qty = qty;
    return 1;
}

// maintain the local replica of the touch.
//
// with no live stream the only book we ever see is the one riding on the response to one
// of our own orders - and a self-wake is a bare ping carrying nothing. so this caches what
// it last saw. note the snapshot on an order response is always the MBO, whatever a client
// is subscribed to; the sub_tier only picks the format of a *broadcast*, which we do not
// take. reading it as anything else is reading the wrong struct
static void t3_read_book(T3* t3, Context* ctx, T3Book* b) {
    MBO* mbo = (MBO*)ctx->data_snapshot;

    if (mbo) {
        u8 hb = mbo->hi_bid_index != MAX_U16 && mbo->hi_bid_index < mbo->level_count;
        u8 ha = hb && (u32)mbo->hi_bid_index + 1 < mbo->level_count;

        if (hb) {
            MBOIndex* bid = mbo->levels + mbo->hi_bid_index;
            t3->last_bid = bid->price;
            t3->last_bid_depth = bid->quantity;
        }
        if (ha) {
            MBOIndex* ask = mbo->levels + mbo->hi_bid_index + 1;
            t3->last_ask = ask->price;
            t3->last_ask_depth = ask->quantity;
        }

        t3->book_have_bid = hb;
        t3->book_have_ask = ha;
        t3->have_book = hb || ha;
        if (t3->have_book)
            t3->booted = 1;
    }

    b->have_bid = t3->book_have_bid;
    b->have_ask = t3->book_have_ask;
    b->best_bid = t3->last_bid;
    b->best_ask = t3->last_ask;
    b->bid_depth = t3->last_bid_depth;
    b->ask_depth = t3->last_ask_depth;
}

// we have never seen a book: no stream, and no order of ours has come back yet. post one
// child somewhere plausible purely to get a response, which carries the book with it.
// after this the replica is live and the normal schedule logic takes over
static u8 t3_send_probe(T3* t3, Context* ctx, Order* out) {
    u16 ref = ctx->mark ? ctx->mark : t3->p.seed_price;
    u16 px;
    if (t3->parent_buy)
        px = ref > 1 ? ref - 1 : 1;
    else
        px = ref < MAX_U16 ? ref + 1 : MAX_U16;

    out->status = (1 << DAY_BIT) | (t3->parent_buy ? (1 << BUY_DIRECTION_BIT) : 0);
    out->price = px;
    out->quantity = t3->p.child_size;
    return t3_await(t3, ctx, T3_PEND_PASSIVE, t3->p.child_size);
}

// how much of the parent the schedule wants done by now.
//
// TWAP is linear in elapsed time. IS front-loads: same endpoints, but it wants
// aggression_half_pct of the parent done by the halfway mark, trading spread now against
// the risk that the price drifts away while we are still working - which is the
// implementation-shortfall tradeoff the doc names
static u32 t3_scheduled(T3* t3, Context* ctx) {
    if (t3->p.horizon_ns == 0)
        return t3->parent_qty;

    u64 elapsed = ctx->real_time_ns - t3->parent_start_ns;
    if (elapsed >= t3->p.horizon_ns)
        return t3->parent_qty;

    // fraction of the horizon gone, in 1/1024ths - integer throughout
    u64 f = (elapsed * 1024) / t3->p.horizon_ns;

    if (t3->p.algo_type == T3_IS) {
        // two straight segments through (0,0), (1/2, aggression_half), (1,1). steeper
        // early, shallower late, and it still lands exactly on the parent at the horizon
        u64 half = t3->p.aggression_half_pct;
        if (f <= 512)
            f = (f * half * 2) / 100;
        else
            f = (half * 1024) / 100 + ((f - 512) * (100 - half) * 2) / 100;
    }

    u64 want = ((u64)t3->parent_qty * f) / 1024;
    return want > t3->parent_qty ? t3->parent_qty : (u32)want;
}

// never take more than our share of what is showing. a real participation cap is a share
// of market VOLUME; with one data subscription per client we can see the book or the tape
// but not both, so this caps against displayed depth instead. the effect is the same in
// spirit - do not be the whole print - but it is a genuinely different measure, and VWAP
// and POV are gated off rather than pretending otherwise
static u16 t3_cap_child(T3* t3, u32 want, u32 depth) {
    u32 remaining = t3->parent_qty - t3->parent_filled;
    if (want > remaining)
        want = remaining;

    u32 cap = (depth * t3->p.participation_cap_pct) / 100;
    if (cap == 0)
        cap = 1; // a cap that rounds to nothing would stall the parent forever
    if (want > cap)
        want = cap;

    if (want > MAX_U16)
        want = MAX_U16;
    return (u16)want;
}

// cross the spread to catch up: marketable limit + IOC, capped at the touch price. never a
// plain market order - an algo that is behind schedule is not so desperate that it will
// pay any price, and the residual coming back lets the next wake re-decide
static u8 t3_send_cross(T3* t3, Context* ctx, Order* out, T3Book* b, u16 qty) {
    // the touch has run outside the client's band - do not chase it
    u16 touch = t3->parent_buy ? b->best_ask : b->best_bid;
    if (!t3_within_band(t3, touch))
        return 0;

    out->status = (1 << IOC_BIT) | (t3->parent_buy ? (1 << BUY_DIRECTION_BIT) : 0);
    out->price = t3->parent_buy ? b->best_ask : b->best_bid;
    out->quantity = qty;
    return t3_await(t3, ctx, T3_PEND_CROSS, qty);
}

// post passively at (or just behind) the touch and wait to be hit. DAY, because an
// intraday schedule has no meaning once the session ends - the parent is abandoned at the
// close rather than left resting into tomorrow
static u8 t3_send_passive(T3* t3, Context* ctx, Order* out, T3Book* b, u16 qty, u8 replacing) {
    u16 px;
    if (t3->parent_buy) {
        px = b->best_bid > t3->p.passive_offset_ticks
           ? b->best_bid - t3->p.passive_offset_ticks : 1;
    } else {
        u32 raised = (u32)b->best_ask + t3->p.passive_offset_ticks;
        px = raised > MAX_U16 ? MAX_U16 : (u16)raised;
    }

    // a passive child never posts outside the band either - resting there is a standing
    // offer to trade at a price the client said no to
    if (!t3_within_band(t3, px))
        return 0;

    out->status = (1 << DAY_BIT) | (t3->parent_buy ? (1 << BUY_DIRECTION_BIT) : 0);
    if (replacing) {
        out->status |= (1 << CAN_REP_BIT);
        out->other_id = t3->passive_id;
    }
    out->price = px;
    out->quantity = qty;
    return t3_await(t3, ctx, T3_PEND_PASSIVE, qty);
}

static u8 t3_send_cancel(T3* t3, Context* ctx, Order* out) {
    out->status = (1 << CANCEL_BIT) | (t3->parent_buy ? (1 << BUY_DIRECTION_BIT) : 0);
    out->other_id = t3->passive_id;
    out->price = t3->passive_price;
    return t3_await(t3, ctx, T3_PEND_CANCEL, 0);
}

// the fat-finger path: the whole parent at once, no slicing, no schedule. this is the
// experiment the doc asks for as a config flag rather than a separate agent type
static u8 t3_send_dump(T3* t3, Context* ctx, Order* out, T3Book* b) {
    u16 dump_touch = t3->parent_buy ? b->best_ask : b->best_bid;
    if (!t3_within_band(t3, dump_touch))
        return 0;

    u32 remaining = t3->parent_qty - t3->parent_filled;
    if (remaining > MAX_U16)
        remaining = MAX_U16;

    out->status = (1 << IOC_BIT) | (t3->parent_buy ? (1 << BUY_DIRECTION_BIT) : 0);
    // still price-protected at the touch: unbounded is a different experiment. what makes
    // this the fat finger is the SIZE arriving at once, not the absence of a limit
    out->price = t3->parent_buy ? b->best_ask : b->best_bid;
    out->quantity = (u16)remaining;
    return t3_await(t3, ctx, T3_PEND_CROSS, (u16)remaining);
}

u8 t3_on_snapshot(T3* t3, Context* ctx) {
    Order* out = ctx->next_order_ptr;

    if (t3_settle(t3, ctx))
        return t3_sleep(ctx, t3->p.reject_backoff_ns);

    if (t3->pending)
        return t3_sleep(ctx, t3->p.child_interval_ns);

    if (!ctx->is_open)
        return t3_sleep(ctx, t3->p.retry_wake_ns);

    // parent done, or the horizon ran out with it unfinished. a real algo hands the
    // residual back to the client rather than chasing it forever
    if (t3->have_parent && (t3->parent_filled >= t3->parent_qty ||
        ctx->real_time_ns - t3->parent_start_ns > t3->p.horizon_ns)) {
        t3->have_parent = 0;
        if (t3->passive_id != MAX_U32)
            return t3_send_cancel(t3, ctx, out);
    }

    if (!t3->have_parent) {
        if (!t3->p.auto_parent)
            return t3_sleep(ctx, t3->p.retry_wake_ns);
        t3_new_parent(t3, ctx);
        return t3_sleep(ctx, t3->p.restart_delay_ns);
    }

    T3Book book;
    t3_read_book(t3, ctx, &book);

    // no book right now. probe ONLY to bootstrap the very first one - a single resting
    // feeler to get an mbo back. once we have ever seen a book, an empty book (makers all
    // pulled in a crash) means WAIT for liquidity to return, not fire another resting probe:
    // re-probing on every empty tick leaks a resting order and its share reserve each time,
    // which is the runaway that pinned a slicer at tens of millions of reserved shares
    if (!t3->have_book) {
        if (t3->booted)
            return t3_sleep(ctx, t3->p.retry_wake_ns);
        return t3_send_probe(t3, ctx, out);
    }

    if (t3->p.slice_disabled) {
        u8 can_cross = t3->parent_buy ? book.have_ask : book.have_bid;
        if (!can_cross)
            return t3_sleep(ctx, t3->p.idle_wake_ns);
        return t3_send_dump(t3, ctx, out, &book) ? 1 : t3_sleep(ctx, t3->p.idle_wake_ns);
    }

    u32 scheduled = t3_scheduled(t3, ctx);
    u32 behind = scheduled > t3->parent_filled ? scheduled - t3->parent_filled : 0;

    // ---- behind schedule: stop waiting to be hit, go get it ----
    if (behind >= t3->p.catch_up_threshold) {
        // the resting child has to come off first - crossing while it rests would leave us
        // quoting one side and taking the other, and double-count against the parent
        if (t3->passive_id != MAX_U32)
            return t3_send_cancel(t3, ctx, out);

        u8 can_cross = t3->parent_buy ? book.have_ask : book.have_bid;
        if (!can_cross)
            return t3_sleep(ctx, t3->p.idle_wake_ns);

        u32 depth = t3->parent_buy ? book.ask_depth : book.bid_depth;
        u16 qty = t3_cap_child(t3, behind, depth);
        if (qty == 0)
            return t3_sleep(ctx, t3->p.child_interval_ns);
        return t3_send_cross(t3, ctx, out, &book, qty) ? 1 : t3_sleep(ctx, t3->p.child_interval_ns);
    }

    // ---- on schedule or ahead: rest and let the market come to us ----

    u8 can_post = t3->parent_buy ? book.have_bid : book.have_ask;
    if (!can_post)
        return t3_sleep(ctx, t3->p.idle_wake_ns);

    u32 touch = t3->parent_buy ? book.best_bid : book.best_ask;
    u32 depth = t3->parent_buy ? book.bid_depth : book.ask_depth;

    if (t3->passive_id != MAX_U32) {
        // the market walked away from where we are resting: follow it, or we sit out of
        // reach for the rest of the horizon and fall behind by default
        u16 drift = touch > t3->passive_price ? touch - t3->passive_price
                                              : t3->passive_price - touch;
        if (drift < t3->p.passive_chase_ticks)
            return t3_sleep(ctx, t3->p.child_interval_ns);

        u16 qty = t3_cap_child(t3, t3->p.child_size, depth);
        if (qty == 0)
            return t3_sleep(ctx, t3->p.child_interval_ns);
        return t3_send_passive(t3, ctx, out, &book, qty, 1) ? 1 : t3_sleep(ctx, t3->p.child_interval_ns);
    }

    u16 qty = t3_cap_child(t3, t3->p.child_size, depth);
    if (qty == 0)
        return t3_sleep(ctx, t3->p.child_interval_ns);
    return t3_send_passive(t3, ctx, out, &book, qty, 0) ? 1 : t3_sleep(ctx, t3->p.child_interval_ns);
}

void t3_get_settings(T3* t3, ClientSettings* client_settings) {
    client_settings->initial_wake    = t3->p.initial_wake;
    client_settings->processing_time = t3->p.processing_time;
    client_settings->net_latency     = t3->p.net_latency;

    // an algo works the CLIENT's order, not its own book - it can be handed a sell parent
    // while holding nothing, so it has to be able to run the position negative
    client_settings->is_cash_account = 0;
    client_settings->margin_mult     = t3->p.margin_mult;
    client_settings->maint_pct       = t3->p.maint_pct;
    client_settings->cash            = t3->p.cash;
    client_settings->reserved_cash   = 0;
    client_settings->shares          = 0;
    client_settings->reserved_shares = 0;

    // L1 + own fills, exactly what the doc gives this tier. it is not modelling queues or
    // reading depth for signal - it needs the touch to post at and the fills to track
    client_settings->sub_tier = TIER_MBP1;
    client_settings->noii     = 0;
}

void t3_free(T3* t3) {
    if (!t3_algo_supported(t3->p.algo_type))
        printf("T3 %s: algo_type %d needs realized market volume, which one data "
               "subscription cannot deliver alongside the book - see t3_algo_supported\n",
               T3_NAMES[t3->name_idx], (int)t3->p.algo_type);
    free(t3);
}
