#ifndef TIER14_DMMS_H
#define TIER14_DMMS_H

#include "types.h"
#include "client_settings.h"
#include "order.h"
#include "context.h"

// T14 - Specialists. the NYSE designated market maker. one per symbol, so one here.
// see TILTYARD_AGENTS.md T14.
//
// orthogonal to the twelve directional tiers: this is not an investor with a view, it is
// an obligated liquidity provider. it quotes both sides continuously because it is
// required to, and at the cross it supplies whichever side the imbalance is short of.
//
// THE PAIRING IS THE POINT. T12 Tides is an imbalance SOURCE - predictable, one-sided MOC
// flow slammed into the closing cross. this tier is the imbalance SINK. a Tides MOC with
// no DMM opposite it clears against whatever offset interest happens to exist and can
// print far from the reference price; a DMM sized against the published imbalance pins the
// close near reference instead. that source/sink pairing is the closing-auction analog of
// the T1/T2 maker/taker pairing that keeps the continuous book from freezing, and the tier
// is much less interesting without T12 running.
//
// it trades AGAINST the published NOII feed, which is why TIER_IMBALANCE is not optional
// decoration here - the imbalance is its core input, not a nice-to-have. it is the only
// tier in the sim that pays for the add-on.
//
// ENGINE GAP, REPORTED NOT BUILT: the formal DMM last-look - the exchange handing the DMM
// the exact unpaired residual to fill at the clearing price against its obligation - has
// no engine hook. per the hard rules that is reported rather than implemented, and gated
// behind last_look_enabled, which stays 0. what IS implementable, and is what this does,
// is offsetting the published imbalance with ordinary auction-only orders contra to it -
// no engine change required.
//
// every behavioural knob lives in T14Params so a sweep harness can vary it without
// touching this file. ALL DEFAULTS ARE /* UNCALIBRATED */ PLACEHOLDERS.

#define T14_PEND_NONE   0
#define T14_PEND_QUOTE  2  // the continuous two-sided obligation quote
#define T14_PEND_OFFSET 3  // auction-only interest contra to the published imbalance
#define T14_PEND_CANCEL 4

typedef struct T14Params {
    // IMBALANCE_OFFSET_FRACTION. what share of the published unpaired interest this DMM
    // stands up against. this is the knob that decides how hard the close is pinned: at 0
    // the cross gaps to wherever the imbalance drags it, at 100 the DMM absorbs all of it
    u16 imbalance_offset_pct;

    // and a ceiling on that, because an obligation is not a blank cheque
    u32 max_auction_commitment;

    // QUOTE_OBLIGATION. the affirmative duty to sit at the NBBO a set fraction of the time.
    // modelled as how aggressively it quotes rather than as a measured percentage - a real
    // obligation is audited over a month, which is not a thing a client can enforce on
    // itself tick by tick
    u16 quote_obligation_pct;
    u16 intraday_half_spread_ticks;
    u16 quote_size;

    // inventory taken on at the cross has to be worked back out, and the obligation is
    // still binding while that happens - so the limit is what keeps the two compatible
    i32 inventory_limit;
    u16 inventory_skew_ticks;   // lean the quote to shed inventory rather than pull it

    u64 requote_interval_ns;
    u64 idle_wake_ns;
    u64 retry_wake_ns;
    u64 reject_backoff_ns;

    u16 seed_price;

    // the engine hook that does not exist. see the header
    u8 last_look_enabled;

    i64 cash;
    u8  margin_mult;
    u8  maint_pct;

    // HFT-grade. a DMM is an electronic market maker wearing a franchise obligation
    u64 processing_time;
    u64 net_latency;
    u64 initial_wake;
} T14Params;

typedef struct T14 {
    T14Params p;

    // the resting two-sided quote
    u32 bid_id;
    u32 ask_id;
    u16 bid_price;
    u16 ask_price;
    u8  quoted;

    // the auction offset order standing against the published imbalance
    u32 offset_id;
    u8  offset_sent;

    // last NOII publication we acted on
    u32 last_imbalance;
    u8  last_imbalance_buy_side;
    u16 last_ref_price;

    i64 inventory;
    i64 cash_guess;

    u16 last_bid;
    u16 last_ask;
    u8  have_bid;
    u8  have_ask;

    u8  pending;
    u8  pending_kind;
    u32 pending_id;

    u8  connected;
    u32 name_idx;
    u32 rng;
} T14;

T14* t14_init();
char* t14_get_name(T14* t14);
u8 t14_on_snapshot(T14* t14, Context* ctx);
void t14_get_settings(T14* t14, ClientSettings* client_settings);
void t14_free(T14* t14);

#endif
