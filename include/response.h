#ifndef RESPONSE_H
#define RESPONSE_H

#include "types.h"

typedef struct Response {
    // client id may actually be 23 bits, so maybe we put flags into the top bits
    u32 client_id;
    // top bits of snapshot_id will actually have some interesting things in them
    // hopefully 1 millino snapshots is enough

    u32 snapshot_id;// also boot or socket

    // all about the order this response is about
    u32 order_id;// this might be in response to an order, set to U32MAX if this is broadcast
    u16 status;
    u32 quantity_filled;
    u16 price;

    // for atomic bid+ask pairs: the ask leg, delivered in this same response
    // (MAX_U32 order id when there is no second leg; filled stays 0 while pairs are non-crossing)
    u32 second_order_id;
    u16 second_price;
    u32 second_quantity_filled;

    u8 rej_reason;
} Response;

#endif

