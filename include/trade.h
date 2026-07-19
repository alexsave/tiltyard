#ifndef TRADE_H
#define TRADE_H

#include "types.h"

// ServerContext is defined in server.h; we only need the name for the prototype
typedef struct ServerContext ServerContext;

typedef struct Candle {
    // maybe duration makes mroe senes
    u64 duration; // in ns, like 60 billion for a minute
    u64 time; // nah just ns since epoch? or should we do how many durations have gone by?
    u16 open;
    u16 hi;
    u16 lo;
    u16 close;
    u32 volume;
} Candle;

// pretty simple
typedef struct Trade {
    u64 ns;
    u32 quantity;
    u16 price;
    u8 direction; // of taker
} Trade;

// one NOII publication during an accumulation window: how much closing interest pairs off, how
// much is left unpaired, and which side that imbalance sits on
typedef struct Imbalance {
    u64 ns;
    u16 ref_price;  // reference (last continuous) price
    u32 paired;     // min(buy, sell) closing interest
    u32 imbalance;  // |buy - sell| unpaired closing interest
    u8 buy_side;    // 1 if buy-heavy, 0 if sell-heavy
} Imbalance;

void update_trade(ServerContext* sc, u32 quantity, u16 price, u8 direction);

#endif

