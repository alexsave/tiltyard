#ifndef CLIENT_ONE_H
#define CLIENT_ONE_H

#include "types.h"
#include "client_settings.h"
#include "order.h"
#include "context.h"


// this will eventually be C's equivalent of an interface but for now-


typedef struct CO {
    uint64_t idk;
    u32 cash_guess;
    u32 shares_guess;
    u32 bid_order_id;
    u32 ask_order_id;
} CO;

CO* co_init();

char* co_get_name(CO* co);

u8 co_on_snapshot(CO* co, Context* ctx);

void co_get_settings(CO* co, ClientSettings* client_settings);

void co_free(CO* co);

#endif
