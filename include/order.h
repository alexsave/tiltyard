#ifndef ORDER_H
#define ORDER_H

#include "types.h"


// ok so this is going to have a BUNCH of stuff in it, everything we need to specify an order
// really these are client->server network events, but wel
// ah that's the term
// for now just type and clientid

// when you hit "buy" on Robinhood, it makes what is called a market order
// just get me stocks at the current price as fast as possible
// but you still need to provide a quantity
// thus is our first needed argument
// what shoudl the size of quantity be?
// 16 bits allows for 65K stocks
// i thik that's enough

// what's the other thing yuo do on robin hood?
// sell
// a market sell is pretty much identical
// sell a certain amount as fast as possible
// we'll fold these into a single u8 later but ...

// ok so that's like level one of trading
// amateur stuff
// if everyone ever did market orders, nothing would actually move really
// if the server is single threaded, and always processed market orders, it would reject all of them
// and beacuse all the market orders were rejected, it would reject the other ones
// there are no "patient" sellers or buyers

// so
// limit orders
// this is liek "I would like N quantity of stock at price P"
// if it's "Deep in the book", it might not get filled for a while
// if it's "outside the book" (dont remember exact terminology), it will be filled imemdiately
// ie - if a stock is at $100 and you want to buy it at $95, you'll wait
// if a stock is at $100 and you want to by at $105, you'll get filled as much as the existing asks can sell
// much more interestig in the actual server impl but yeah
// i've decided to keep prices as ints representing cents
// so Max $ is $655.36 with u16

// the other thing are stop orders
// so a basic strategy thing you can do is buy at a price, set a take profit some amount 2*X above the current price, set a stop loss 1*X below the current price
// and if one triggers, the other gets cancelled
// the best part is that now Order needs to save room for TWO prices
// because ideally these go in atomically as a single transaction
// the "take profit" is just a limit order as discusssed above
// the "stop loss" is actually more intresting
// it doesn't go into the order book like the take profit limit order
// the server kinda remembers it
// then turns it into a marker order when that price is hit

// and of cours ws

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
static const u8 BUY_DIRECTION_BIT = 7;
static const u8 IS_LIMIT_BIT = 6; // otherwise market
static const u8 HAS_STOP_BIT = 5;
static const u8 OCO_BIT = 4;
static const u8 WS_BIT = 3;
static const u8 REJECT_BIT = 0;

// wait til you hear about cancel-replace orders you will shit brix

// also includes websocket connections, but here we go
typedef struct Order {
    u8 flags;
    u16 quantity; // up to 65K stocks at a time, update if we have whales
    u16 price;
    u16 stop_price;
    u32 client_id;

    //smh trust me you want to align to 8 bytes
    u8 paddingA;
    u64 paddingB;
} Order;

#endif

