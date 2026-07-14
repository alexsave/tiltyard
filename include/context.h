#ifndef CONTEXT_H
#define CONTEXT_H

#include "order.h"
#include "types.h"

typedef struct Context {
    // or how about just give them the damn reponse
    void* mbo_snapshot;
    // same thing here, both needed
    u32 order_id; // the order this response is for. MAX_U32 for broadcasts?
    // this is problematic, as the order is always 100% up to date
    // by the time it gets to the client, it could've had another fill on it
    //Order* response_order_ptr;
    u32 quantity_filled; //quantity of that order
    u16 price; //price of that order
    u16 status; // status of ^THIS order

    // the ask leg of an atomic pair, delivered alongside the bid in the same wake-up
    // only meaningful when the pair bit is set in status
    u32 second_order_id;
    u16 second_price;
    u32 second_quantity_filled;

    // next order info
    u32 next_order_id;
    Order* next_order_ptr;
    u32 random;
} Context;

#endif

