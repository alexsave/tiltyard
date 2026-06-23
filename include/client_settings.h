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
    u8 ws; 
    u8 is_cash_account;
    u32 cash;
    u32 reserved_cash; // bids in market
    u32 buying_power;
    u32 shares;
    u32 reserved_shares;

    u64 initial_wake;
    u64 processing_time;
    u64 net_latency;
    
} ClientSettings;

#endif

