#ifndef TRADE_H
#define TRADE_H

#include "types.h"

static const UNIT_SEC = 0;
static const UNIT_MIN = 1;
static const UNIT_HR = 2;
static const UNIT_DAY = 3;

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

void update_trade(ServerContext* sc, u32 quantity, u16 price, u8 direction);

#endif

