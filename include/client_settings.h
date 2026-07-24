#ifndef CLIENT_SETTINGS_H
#define CLIENT_SETTINGS_H

#include "types.h"
// let me be clear
// in a cash account - your buying power == (cash-reserved_cash)
// BIDS will change reserved_cash, but not cash unless they are filled instantly
// and your "selling ability" is == shares - reserved_shares
// ASKS wil chagned reserved_shares but not shares

// also equity here is cash - reserved_cash + price * (shares - reserved_shares)
// more on that later
//price is debatable and changes every tick

// $$, initial wake, processing time, latency
typedef struct ClientSettings {
    // used in ob modification to signify a special response will be sent
    u8 will_notify; 

    u8 ws; 
    u8 is_cash_account;
    // 2 for .5 margin requirement, 4 for .25 margin requirement
    // keeping as an int not a float
    u8 margin_mult;
    // maintenance %, house call. separate number from margin_mult, and per client:
    // 25 is the reg floor, houses run 30-35 on whoever they trust less
    u8 maint_pct;
    i64 cash; // negative = margin loan
    u32 reserved_cash; // bids in market
    //u32 buying_power;
    i64 shares; // negative = short
    u32 reserved_shares;

    // backstop: earliest self-wake in flight, MAX_U64 for none. a later one is dropped, so pacing
    // is still the client's job - this only stops a tier from breeding a wake per event
    u64 next_wake_ns;

    u64 initial_wake;
    u64 processing_time;
    u64 net_latency;

    // MBO, MBP, MBP10, MBP1, trade later
    u8 sub_tier;
    // NOII imbalance feed, an add-on billed on top of the base sub_tier
    u8 noii;

    // conflated book stream (blob tiers only): at most one delivery event in the air per
    // client. while one is in flight, newer snapshots only overwrite the pending slot (the
    // pin swaps with them), and the fire path in main.c chains the newest one at its own
    // arrival time. calloc's zero-init reads as idle with nothing pending, which is right
    u8 stream_in_flight;
    u8 stream_pending_valid;
    u8 stream_pending_tier;
    u32 stream_pending_snapshot;
    u64 stream_pending_arrival;

} ClientSettings;

#endif

