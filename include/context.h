#ifndef CONTEXT_H
#define CONTEXT_H

#include "order.h"
#include "types.h"

typedef struct Context {
    // or how about just give them the damn reponse
    void* data_snapshot;
    // same thing here, both needed
    u32 order_id; // the order this response is for. MAX_U32 for broadcasts?
    // this is problematic, as the order is always 100% up to date
    // by the time it gets to the client, it could've had another fill on it
    //Order* response_order_ptr;
    u32 quantity_filled; //quantity of that order
    u16 price; //price of that order
    u32 status; // status of ^THIS order

    // the ask leg of an atomic pair, delivered alongside the bid in the same wake-up
    // only meaningful when the pair bit is set in status
    u32 second_order_id;
    u16 second_price;
    u32 second_quantity_filled;

    // next order info
    u32 next_order_id;
    Order* next_order_ptr;
    u32 random;
    u64 real_time_ns;
    u64 wake_delay_ns;
    // the self-wake already in flight, MAX_U64 for none. asking for anything later than this is
    // dropped, so a client that wants to know whether it's covered can read it instead of guessing
    u64 next_wake_ns;

    // last trade price. every tier can read this, the free tier gets nothing else
    u16 mark;

    // session phase, mirrored from the server at each bell/auction transition. lets a client
    // tell continuous trading from an accumulation window or the freeze
    u8 is_open;
    u8 auctioning;
    u8 auction_frozen;

    // abstract 0-255 scale of how well the $TYD company is doing
    // clients can react to it however they want
    u8 news_signal;
    u64 last_news_ns;

    u8 rej_reason;
} Context;

#endif

