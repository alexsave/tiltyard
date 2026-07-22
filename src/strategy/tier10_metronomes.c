#include <stdlib.h>
#include <stdio.h>

#include "strategy/tier10_metronomes.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"
#include "server.h"
#include "response.h"

// T10 - Metronomes. payday arrives, money goes in, no questions asked.
//
// the sim runs 0 for now. the real population is 100M+; the doc recommends AGGREGATE and
// warns that POPULATED is a scaling stress test of the slow scheduler rather than a
// realism win. whatever count this is given, remember each agent wakes ~24 times a YEAR.

// plan / provider names, loosely parodied. one per instance, handed out in init order
static const char* T10_NAMES[] = {
    "vanguardian.2055",  "fidelius.freedom",  "schwabb.target",   "troweprince.rmt",
    "blackstone.lifepath","statestrand.glide", "empowered.401k",  "principality.plan",
    "voyager.retire",    "nationswide.plan",  "transamerika.tgt", "johnhandcock.plan",
};
static const u32 T10_NAME_COUNT = sizeof(T10_NAMES) / sizeof(T10_NAMES[0]);
static u32 t10_next_name = 0;

// the tunable block. EVERY value here is a placeholder - none of it is calibrated, and
// none of it should be tuned to make the tape "look right". that is the owner's job.
// a function rather than file scope so it can use the time constants: a const u64 is not
// a constant expression in C, so S_TO_NS cannot seed a static
static T10Params t10_defaults() {
    T10Params p;

    // per-person contribution is tiny; it is the headcount that makes the tier matter
    p.contribution_min       = 200;   /* UNCALIBRATED */
    p.contribution_max       = 3000;  /* UNCALIBRATED */

    // biweekly payroll, spread across the period so the tier does not arrive in one block
    p.contribution_period_ns = 14 * DAY_TO_NS; /* UNCALIBRATED */
    p.payday_phase_spread_ns = 14 * DAY_TO_NS; /* UNCALIBRATED */

    p.min_lot                = 1;     /* UNCALIBRATED */

    // the doc's model (b). needs routing, which is not shipped - see the header
    p.route_via_tides        = 0;

    p.cash                   = 1000000; /* UNCALIBRATED */
    // batched overnight through a fund administrator. this tier could not care less
    p.processing_time        = 1 * H_TO_NS;  /* UNCALIBRATED */
    p.net_latency            = 1 * S_TO_NS;  /* UNCALIBRATED */
    p.initial_wake           = 15 * H_TO_NS; /* UNCALIBRATED */

    return p;
}

// xorshift, per agent. determinism is a hard rule: same seed in, same sequence out
static u32 t10_rand(T10* t10) {
    u32 x = t10->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t10->rng = x;
    return x;
}

T10* t10_init() {
    T10* t10 = malloc(sizeof(T10));

    t10->p = t10_defaults();

    t10->accrued = 0;
    t10->inventory = 0;
    t10->pending = 0;
    t10->pending_id = MAX_U32;

    // names go out in init order, so the roster is stable across runs
    t10->name_idx = t10_next_name % T10_NAME_COUNT;
    // each agent carries its own rng state, seeded off its slot. no shared global rng
    t10->rng = 0x9e3779b9u * (t10_next_name + 1);

    i64 span = t10->p.contribution_max - t10->p.contribution_min;
    t10->contribution = t10->p.contribution_min + (i64)(t10_rand(t10) % (u32)(span + 1));
    t10->cash_guess = t10->p.cash;

    t10_next_name++;

    return t10;
}

char* t10_get_name(T10* t10) {
    return (char*)T10_NAMES[t10->name_idx];
}

// WAKE_CALENDAR - action bit 2. payday, and nothing in between
static u8 t10_sleep(T10* t10, Context* ctx, u64 base) {
    (void)t10;
    // clamp inside a pending wake rather than deferring to it - see tier13_glaciers.c for
    // the failure this avoids: a wake delivered a hair before its own fire_at leaves
    // next_wake_ns advertising one that has already been spent, and every later re-arm is
    // then dropped against that stale value.
    //
    // ONLY the feed-less calendar tiers do this, and the distinction is load-bearing. a
    // tier on a live stream is woken by the tape whatever happens, so deferring costs it
    // nothing - and clamping would defeat the wake dedup that stops a client arming a wake
    // per event. applying this to the makers reintroduced exactly that storm and crashed
    // the sim inside one second. this tier has no feed, so a lost wake is the end of it,
    // and its cadence is minutes-to-days so a clamp cannot spin
    if (ctx->real_time_ns + base >= ctx->next_wake_ns) {
        if (ctx->next_wake_ns <= ctx->real_time_ns + 1)
            return 0;
        base = ctx->next_wake_ns - ctx->real_time_ns - 1;
    }
    ctx->wake_delay_ns = base;
    return 2;
}

// the next payday, phase-spread so the population does not all contribute at once
static u64 t10_next_payday(T10* t10) {
    u64 phase = t10->p.payday_phase_spread_ns
              ? (u64)(t10_rand(t10) % 1000) * (t10->p.payday_phase_spread_ns / 1000)
              : 0;
    // the phase is a one-off offset on top of the period, not a new period
    return t10->p.contribution_period_ns / 2 + phase / 2;
}

u8 t10_on_snapshot(T10* t10, Context* ctx) {
    Order* out = ctx->next_order_ptr;

    u8 is_fill = (ctx->status >> FILL_BIT) & 1;

    if (t10->pending && ctx->order_id == t10->pending_id) {
        t10->pending = 0;
        t10->pending_id = MAX_U32;
        if (is_fill && ctx->quantity_filled) {
            u16 px = ctx->price ? ctx->price : ctx->mark;
            t10->inventory += (i64)ctx->quantity_filled;
            t10->cash_guess -= (i64)ctx->quantity_filled * (i64)px;
        }
        // a contribution that could not be invested this payday is not refunded to the
        // participant - it sits in the plan and goes in next time
    }

    if (t10->pending)
        return t10_sleep(t10, ctx, t10_next_payday(t10));

    if (!ctx->is_open)
        return t10_sleep(t10, ctx, t10_next_payday(t10));

    // PAYDAY. the money is here and it is going in. there is deliberately no test of
    // price, trend, valuation or anything else on this path - see the header
    t10->accrued += t10->contribution;

    if (ctx->mark == 0)
        return t10_sleep(t10, ctx, t10_next_payday(t10)); // nothing has printed to buy at

    i64 shares = t10->accrued / (i64)ctx->mark;
    if (shares < (i64)t10->p.min_lot)
        return t10_sleep(t10, ctx, t10_next_payday(t10)); // carry it, buy a lot next time
    if (shares > MAX_U16)
        shares = MAX_U16;

    // whole shares leave, the remainder rides along to the next payday. this tier is the
    // one place fractional-share reality actually bites, and dropping the remainder would
    // silently shrink every contribution
    t10->accrued -= shares * (i64)ctx->mark;

    out->status = (1 << IS_MARKET_BIT) | (1 << IOC_BIT) | (1 << BUY_DIRECTION_BIT);
    out->quantity = (u16)shares;
    out->price = 0;

    t10->pending = 1;
    t10->pending_id = ctx->next_order_id;
    return 1;
}

void t10_get_settings(T10* t10, ClientSettings* client_settings) {
    client_settings->initial_wake    = t10->p.initial_wake;
    client_settings->processing_time = t10->p.processing_time;
    client_settings->net_latency     = t10->p.net_latency;

    // cash account. a retirement contribution is not levered and never sells
    client_settings->is_cash_account = 1;
    client_settings->cash            = t10->p.cash;
    client_settings->reserved_cash   = 0;
    client_settings->shares          = 0;
    client_settings->reserved_shares = 0;

    // DATA_NONE. they watch nothing, which is the point - a feed would be an input to a
    // decision, and there is no decision here
    client_settings->sub_tier = TIER_FREE;
    client_settings->noii     = 0;
}

void t10_free(T10* t10) {
    free(t10);
}
