#ifndef ORDER_H
#define ORDER_H

#include "types.h"

// so let's look at all the possible orders we can have
/*
MARKET BUY
MARKET SELL
LIMIT BUY
LIMIT SELL
STOP BUY
STOP SELL
LIMIT BUY + STOP BUY (tp + sl for sell)
LIMIT SELL + STOP SELL (tp + sl for buy)
*/

// if you take out the direciton, its 4 types
/* MARKET, LIMIT, STOP, LIMIT + STOP */
// direction + types = 3 bits
// nice
// one more bit for websocket
// one more bit for oco 
// still at 5 bits which fits in u8
// thsu

// dont get me started on trailing stops
// we'll come back to that later

// ok we can work with this
// could probably all be put in it's own class tbh 
// yeah why not 
static const u8 CAN_REP_BIT = 10; // posisbly fold into cancel bit and use quantity check
static const u8 CANCEL_BIT = 9;
static const u8 PARTIAL_FILL_BIT = 8;
static const u8 BUY_DIRECTION_BIT = 7;
static const u8 IS_LIMIT_BIT = 6; // otherwise market
static const u8 HAS_STOP_BIT = 5;
static const u8 OCO_BIT = 4;
static const u8 WS_BIT = 3;
static const u8 PING_BIT = 2;
static const u8 FILL_BIT = 1;
static const u8 REJECT_BIT = 0;

// wait til you hear about cancel-replace orders you will shit brix

// also includes websocket connections, but here we go
typedef struct Order {
    u16 status;
    u16 quantity; // up to 65K stocks at a time, update if we have whales
    u16 price;
    u16 stop_price;

    u32 client_id;

    // order id for replace or cancel 
    u32 other_id;
    //smh trust me you want to align to 8 bytes, but it'll do it automatically
} Order;

#endif

