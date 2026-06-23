#include <stdlib.h>
#include <stdio.h>

#include "strategy/client_zero.h"
#include "sch.h"
#include "types.h"
#include "order.h"
#include "client_settings.h"

CZ* cz_init() {
    printf("client Z is init!\n");
    return malloc(sizeof(CZ));
}

char* cz_get_name(CZ* cz) {
    return "client.0";
}

u8 cz_on_snapshot(CZ* cz, Context* ctx){
    Order* order_ptr = ctx->order_ptr;
    order_ptr->flags = (ctx->random & 1) << BUY_DIRECTION_BIT;

    order_ptr->price = ctx->random & MAX_U16;
    order_ptr->quantity = (ctx->random & MAX_U8) >> 2 + 1;

    return 1;
}

// i propse folding all these into a single one

void cz_get_settings(CZ* cz, ClientSettings* client_settings){
    // these are like fixed state. you can only change these by 
    // making your computer faster, colocating, booting faster
    client_settings->initial_wake = (u64)25*60*60*S_TO_NS;
    client_settings->processing_time = (u64)15 * S_TO_NS;
    client_settings->net_latency = (u64)300000000;
}

void cz_free(CZ* cz) {
    free(cz);
}

