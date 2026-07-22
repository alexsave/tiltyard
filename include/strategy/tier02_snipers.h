#ifndef TIER02_SNIPERS_H
#define TIER02_SNIPERS_H

#include "types.h"
#include "client_settings.h"
#include "order.h"
#include "context.h"

// T2 - Snipers. hft takers / latency arbs: form a fair value, watch for a resting quote
// on the wrong side of it, race to hit it. see TILTYARD_AGENTS.md T2.
//
// the natural predator of T1. pairing the two is what makes the book churn instead of
// freeze - makers rest liquidity, takers cross it, both get feedback.
//
// every behavioural knob lives in T2Params so a sweep harness can vary it without
// touching this file. ALL DEFAULTS ARE /* UNCALIBRATED */ PLACEHOLDERS.

// why we are getting out, which decides whether we are willing to cross fair value to do it
#define T2_FLAT_NONE 0
#define T2_FLAT_EDGE 1  // hit the position cap: still price-disciplined
#define T2_FLAT_TIME 2  // hit the time stop: out at any price

// where fair value comes from. the doc calls this fair_value_source
typedef enum {
    T2_FV_MICROPRICE,  // size-weighted touch. leans toward the thinner side
    T2_FV_NEWS,        // the engine's 0-255 company-health signal, mapped to a price
    T2_FV_BLEND        // microprice, tilted by the news signal
} t2_fv_source_t;

typedef struct T2Params {
    t2_fv_source_t fair_value_source;

    // a resting quote this far the wrong side of fair value is stale, and worth racing for.
    // min_edge is what we insist on capturing after the fee - below it there is no trade
    u16 staleness_threshold_ticks;
    u16 min_edge_ticks;

    // the news signal is an abstract 0-255 health level, not a price. mapping it is the
    // client's job, and this is ours: an ABSOLUTE level, not an offset from wherever the
    // book happens to be. neutral (128) means the anchor is fair, and a full swing to 0 or
    // 255 moves fair value news_full_swing_ticks either side of it.
    //
    // absolute is load-bearing. anchoring the signal on the current mid makes fair value
    // self-referential - it sits a fixed distance above the book forever, so a taker sees
    // the same edge at any price and ratchets the market up until something else stops it.
    // an absolute level is what lets the tier say "far enough" and turn around
    u16 fundamental_anchor;      // price the health scale is centred on
    u16 news_full_swing_ticks;

    // how much of fair value is that absolute fundamental vs the book's own microprice,
    // 0-256. microprice is the better short-horizon read but cannot bound a move; the
    // fundamental is what anchors it
    u16 fundamental_weight;

    // sizing. we take what is displayed, capped - never more than the quote is showing,
    // because anything past it fills at a worse price than the one we judged stale
    u16 max_take_qty;
    i32 max_position;

    // near-zero inventory is the whole posture. past this we stop adding and work back
    i32 flatten_threshold;
    u16 flatten_urgency_ticks;  // ticks through the touch we will pay to get flat

    // how long we will carry a position before getting out REGARDLESS of fair value.
    //
    // the fair-value gate on the flatten stops the money pump, but on its own it also means
    // a position the market has moved away from is held forever - and this tier's whole
    // premise is a short-horizon edge, not a view worth warehousing. without a time stop
    // every sniper ends up pinned at max_position, unable to add and unwilling to exit,
    // which removes the arresting force that pulls price back toward fair value
    u64 max_hold_ns;

    // once a flatten starts it runs to FLAT and nothing else trades until it is done, then
    // nothing trades at all for this long.
    //
    // both halves are load-bearing, and the run that taught us cost 1.3 billion shares in
    // eight seconds. the flatten used to fire per-wake off "am i over the threshold right
    // now", which meant one shot took inventory from the cap back under it, the edge test
    // saw a clean book on the very next wake and re-entered, and the position was back at
    // the cap. exit crossing up to the ask, re-entry crossing down to the bid, spread paid
    // twice a round trip, at wake speed. the latch makes an exit a decision about the whole
    // position rather than about one shot, and the cooldown stops the signal that put us
    // there from firing again the instant we are out of it
    u64 reentry_cooldown_ns;

    // no book, no signal, nothing to do - come back later
    u64 idle_wake_ns;
    u64 retry_wake_ns;
    u64 reject_backoff_ns;

    // an ioc that finds nothing comes back on REJECT_BIT. that is not an error, just a
    // race we lost, so it gets its own much shorter backoff
    u64 missed_backoff_ns;

    // account / connection shape
    i64 cash;
    u8  margin_mult;
    u8  maint_pct;
    u64 processing_time;
    u64 net_latency;
    u64 initial_wake;
} T2Params;

typedef struct T2 {
    T2Params p;

    // one shot in flight at a time. an ioc either fills or dies on arrival, so there is
    // never anything of ours resting to track - only the shot we are waiting to hear about.
    // the fill response carries no direction, so we have to remember which way we fired
    u8 pending;

    // when we last went from flat to holding something, for the time stop
    u64 position_since_ns;

    // the flatten latch. 0 = trading normally. set when we decide to get out, and it stays
    // set until inventory is actually 0 - so the exit cannot be interrupted and handed back
    // to the entry signal half way through. we keep WHY, because it decides the price
    // discipline: T2_FLAT_EDGE came from the position cap and still refuses to sell below
    // fair value, T2_FLAT_TIME came from the time stop and takes whatever the touch is
    u8 flattening;

    // no new shots until this. set when a flatten completes
    u64 cooldown_until_ns;
    u8 pending_buy;
    u32 shot_id;

    // our own books. the engine will not tell us, so we track what we sent and got
    i64 inventory;
    i64 cash_guess;

    // local replica of the touch, maintained off the feed. most wakes carry no snapshot -
    // a self-wake is a bare ping - and fair value can move without the book moving at all
    // (a news event does exactly that), so re-deriving the book each wake and giving up
    // when it isn't there means never acting on anything but a book update
    u8  have_book;
    u16 last_bid;
    u16 last_ask;
    u32 last_bid_depth;
    u32 last_ask_depth;

    u8 connected;
    u32 name_idx;
    u32 rng;
} T2;

T2* t2_init();
char* t2_get_name(T2* t2);
u8 t2_on_snapshot(T2* t2, Context* ctx);
void t2_get_settings(T2* t2, ClientSettings* client_settings);
void t2_free(T2* t2);

#endif
