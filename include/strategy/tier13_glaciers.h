#ifndef TIER13_GLACIERS_H
#define TIER13_GLACIERS_H

#include "types.h"
#include "client_settings.h"
#include "order.h"
#include "context.h"

// T13 - Glaciers. pensions, sovereign wealth funds, endowments. the slowest tier in the
// sim and the largest single pool of capital in it. see TILTYARD_AGENTS.md T13.
//
// THIS IS THE CEILING. every other tier that sells is selling something it chose to buy,
// so its supply runs out exactly when the move gets big - a floor can be defended with
// cash, which is unbounded, but a ceiling needs shares, which are not. a Glacier is
// different: it holds a TARGET WEIGHT of a portfolio. it does not have a view on price at
// all. when this name runs 10x, the position's weight in the portfolio runs 10x with it,
// blows through rebalance_threshold, and the fund is mechanically obliged to sell the
// excess back to target - selling MORE the further price runs, precisely because it is
// not trying to be clever. that is how real markets cap a runaway, and it is why this
// tier and not T9 is the thing that stops a melt-up.
//
// the same mechanism is a floor on the way down: a crash shrinks the weight below target
// and the fund buys. one knob, both barriers, no shorting required.
//
// SELF-SLICING. the doc models this tier as a Slicer client - it hands a huge parent
// order to T3 and lets someone else work it. T11 routing is not being shipped, so clients
// cannot route through each other, and a Glacier that traded its whole parent in one
// print would be a fat-finger event rather than a pension. so it works the order itself:
// same patient, impact-averse slicing, in-tier.
//
// DATA_NONE: it has no opinion about the touch and no feed to form one. the doc puts its
// eyes at its executing brokers, which we do not have, so it works off the last print on
// the Context like T9 does. a fund rebalancing over weeks does not need the BBO.
//
// every behavioural knob lives in T13Params so a sweep harness can vary it without
// touching this file. ALL DEFAULTS ARE /* UNCALIBRATED */ PLACEHOLDERS.

// what we last sent and have not heard back about, keyed by the MESSAGE id
#define T13_PEND_NONE   0
#define T13_PEND_SLICE  2  // a working slice of the parent, resting in the book
#define T13_PEND_CANCEL 3  // pulling a slice that has gone stale against the print

typedef struct T13Params {
    // AUM. the largest single pool in the sim, and a distribution rather than a number -
    // a $500bn public pension and a $1.7tn SWF are not the same participant
    i64 aum_min;
    i64 aum_max;

    // the mandate. what fraction of the portfolio is meant to sit in this one name, in
    // basis points - a single equity in a diversified institutional book is a small slice
    // of it. this is the whole strategy: there is no view here, only a weight
    u16 target_weight_bp;

    // how far the weight is allowed to drift before the committee acts. wide = a tolerant
    // mandate that lets winners run; narrow = a tight tracking band that sells into every
    // rally. this is the knob that decides how hard the ceiling is
    u16 rebalance_threshold_bp;

    // EXECUTION. a parent this size cannot be shown to the market at once - it is worked
    // in slices over a horizon, which is what impact aversion actually means in practice
    u64 execution_horizon_ns;
    u64 slice_interval_ns;
    u16 impact_aversion_pct;   // most of the remaining parent any one slice may show
    u16 max_slice;             // and a hard cap on top, in shares
    u16 min_slice;

    // WAKE_CALENDAR, the slowest in the sim. between rebalances it does nothing at all -
    // it wakes, looks at the weight, and almost always goes straight back to sleep
    u64 rebalance_frequency_ns;

    // a resting slice that the print has walked away from is repriced, not left to rot
    u64 slice_patience_ns;

    // nothing has printed yet - the initial position has to be sized off something
    u16 seed_price;

    u64 retry_wake_ns;
    u64 reject_backoff_ns;

    // committee-slow. latency is irrelevant to a fund that rebalances a few times a year
    u64 processing_time;
    u64 net_latency;
    u64 initial_wake;
} T13Params;

typedef struct T13 {
    T13Params p;

    // this fund's own size and mandate, drawn once
    i64 aum;
    u16 target_weight;         // in bp, with a per-fund tilt off the tier default

    // the position. seeded AT target, so the fund starts where its mandate says it should
    // be and only ever trades because price moved it off that - never because it arrived
    i64 inventory;
    i64 cash_guess;

    // the parent order being worked. signed: negative is a sell. 0 = nothing to do
    i64 parent_remaining;
    u8  parent_is_buy;
    u64 work_deadline_ns;      // the horizon runs out and we stop, done or not

    // the single working slice resting in the book (MAX_U32 = none)
    u32 slice_id;
    u16 slice_price;
    u64 slice_since_ns;

    // in-flight message
    u8  pending;
    u8  pending_kind;
    u32 pending_id;

    u32 name_idx;
    u32 rng;
} T13;

T13* t13_init();
char* t13_get_name(T13* t13);
u8 t13_on_snapshot(T13* t13, Context* ctx);
void t13_get_settings(T13* t13, ClientSettings* client_settings);
void t13_free(T13* t13);

#endif
