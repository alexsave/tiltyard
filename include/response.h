#ifndef RESPONSE_H
#define RESPONSE_H

#include "types.h"

typedef struct Response {
    // client id may actually be 23 bits, so maybe we put flags into the top bits
    u32 client_id;
    // top bits of snapshot_id will actually have some interesting things in them
    // hopefully 1 millino snapshots is enough

    u32 snapshot_id;// also boot or socket
    u32 order_id;// this might be in response to an order, set to U32MAX if this is broadcast 
    u8 status;
} Response;

#endif

