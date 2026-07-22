#ifndef TIER10_METRONOMES_H
#define TIER10_METRONOMES_H

#include "types.h"
#include "client_settings.h"
#include "order.h"
#include "context.h"

// T10 - Metronomes. 401k / DCA contributions. the largest tier in the world by headcount
// (100M+ individuals) and the smallest by flow per agent. see TILTYARD_AGENTS.md T10.
//
// THERE IS NO PRICE LOGIC IN THIS FILE, AND THERE MUST NOT BE. the doc is explicit: they
// buy regardless of level. that is not a simplification, it is the defining property -
// payroll deduction arrives on payday and gets invested whether the market is at an all
// time high or halfway through a crash. any "wait for a better price" behaviour here would
// be a different tier wearing this one's name.
//
// what it collapses to is a small, steady, entirely predictable BUY-PRESSURE RATE. it is a
// permanent structural bid under the market. worth being clear-eyed about what that means
// for the sim: this tier can only ever push price up, it never sells, and it does not care
// how high price already is - so it is the one population that makes an unbounded melt-up
// MORE likely, not less. pair it with something that can supply stock.
//
// the doc offers two models: (a) direct small buys, (b) a demand stream that feeds Tides,
// which is the more correct one - real 401k money buys index funds, and it is the fund that
// touches the book. (a) is implemented; (b) needs cross-client routing, which is not being
// shipped, so it is a param that reports rather than a path that runs.
//
// every behavioural knob lives in T10Params so a sweep harness can vary it without
// touching this file. ALL DEFAULTS ARE /* UNCALIBRATED */ PLACEHOLDERS.

#define T10_PEND_NONE 0
#define T10_PEND_BUY  2

typedef struct T10Params {
    // the contribution. a payroll deduction is an amount of MONEY, not a share count -
    // which is the whole reason this tier keeps buying as price rises: the same $500 just
    // buys fewer shares. sizing in shares would quietly make it price-sensitive
    i64 contribution_min;
    i64 contribution_max;

    // payday. period is how often it lands, phase spreads the population across the month
    // so the whole tier does not contribute on the same instant
    u64 contribution_period_ns;
    u64 payday_phase_spread_ns;

    // a contribution too small to buy a single share at the current price accrues instead
    // of being thrown away - fractional shares are real for this tier, whole shares are
    // what the book takes, and skipping the remainder would leak the difference
    u16 min_lot;

    // ROUTE_VIA_TIDES: the doc's model (b), where contributions are demand handed to T12
    // rather than orders sent to the book. it needs client-to-client routing, which is not
    // shipped - so this stays 0 and the tier buys directly. left here so the choice is
    // visible in the params rather than buried as an assumption
    u8 route_via_tides;

    i64 cash;
    u64 processing_time;   // batched through a fund administrator overnight. irrelevant
    u64 net_latency;
    u64 initial_wake;
    // spread the boot phase across the tier. a whole-hour initial_wake shared by every
    // instance starts the entire population on one tick, and no amount of per-agent period
    // skew can pull them apart afterwards - they just march in step at slightly different
    // rates. this is the master phase reference and it has to be per-agent
    u64 initial_wake_spread_ns;
} T10Params;

typedef struct T10 {
    T10Params p;

    i64 contribution;      // this participant's per-period amount, drawn once
    i64 accrued;           // money carried forward that could not buy a whole share yet

    i64 inventory;
    i64 cash_guess;

    u8  pending;
    u32 pending_id;

    u64 first_wake_ns;     // this agent's own boot phase, drawn once
    u32 name_idx;
    u32 rng;
} T10;

T10* t10_init();
char* t10_get_name(T10* t10);
u8 t10_on_snapshot(T10* t10, Context* ctx);
void t10_get_settings(T10* t10, ClientSettings* client_settings);
void t10_free(T10* t10);

#endif
