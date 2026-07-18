#ifndef RESPONSE_H
#define RESPONSE_H

#include "types.h"

// why an order was rejected. treat unknown values as REJ_OTHER
static const u8 REASON_NONE = 0;

// malformed, never reached the book
static const u8 REJ_INVALID_QUANTITY = 1;
static const u8 REJ_INVALID_PRICE = 2;
static const u8 REJ_BAD_QUALIFIER = 3; // bits that contradict each other
static const u8 REJ_CROSSED_PAIR = 4; // pair's bid isn't below its ask

// can't be funded
static const u8 REJ_NO_BUYING_POWER = 5;
static const u8 REJ_NO_SHARES = 6;

// the cancel/replace target isn't usable
static const u8 REJ_UNKNOWN_ORDER = 7; // not resting in the book
static const u8 REJ_NOT_YOUR_ORDER = 8;
static const u8 REJ_ORDER_ALREADY_DONE = 9; // already filled or rejected

// accepted, then went away. a real venue calls these cancels, not rejects, so they
// ride on REJECT_BIT until there's a CANCELLED_BIT - and status is a full u16 now,
// so that bit has to come from somewhere
static const u8 CXL_IOC_UNFILLED = 10;
static const u8 CXL_FOK_KILLED = 11;
// a resting day/gtd order pulled at the close, same "cancel on the reject bit" convention
static const u8 CXL_SESSION_CLOSE = 12;

// arrived while the market was closed, never reached the book
static const u8 REJ_MARKET_CLOSED = 13;

static const u8 REJ_OTHER = 99;

typedef struct Response {
    // client id may actually be 23 bits, so maybe we put flags into the top bits
    u32 client_id;
    // top bits of snapshot_id will actually have some interesting things in them
    // hopefully 1 millino snapshots is enough

    u32 snapshot_id;// blob id for a blob broadcast, buffer offset for a trade/candle broadcast

    // a broadcast's tier - indexes sc->tier_source to the BS* (blob, bs_get) or CB* (buffer,
    // cb_at) it came from, resolved at delivery so a store that moves mid-flight stays safe
    u8 tier;

    // all about the order this response is about
    u32 order_id;// this might be in response to an order, set to U32MAX if this is broadcast
    u32 status;
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

