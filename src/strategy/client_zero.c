#include <stdlib.h>
#include <stdio.h>

#include "strategy/client_zero.h"
#include "sch.h"
#include "types.h"
#include "order.h"
#include "context.h"
#include "constants.h"
#include "client_settings.h"

CZ* cz_init() {
    printf("client Z is init!\n");
    return malloc(sizeof(CZ));
}

char* cz_get_name(CZ* cz) {
    return "client.0";
}

u8 cz_on_snapshot(CZ* cz, Context* ctx){
    //printf("order id %u, status %u\n", ctx->order_id, ctx->status);
    if ((ctx->order_id != MAX_U32) && ((((ctx->status) >> WS_BIT) & 1)==0)){
        //it's your echo, just stfu
        //printf("shutting the fuck up because echo\n");
        //return 0;
    }
    
    Order* order_ptr = ctx->next_order_ptr;
    order_ptr->flags = (ctx->random & 1) << BUY_DIRECTION_BIT;

    order_ptr->price = ctx->random & MAX_U16;
    order_ptr->quantity = ((ctx->random & MAX_U8) >> 2) + 1;

    return 1;
}

// i propse folding all these into a single one

// I think only this one can modify the settings
// everything else only reads so as to not mess with the server
void cz_get_settings(CZ* cz, ClientSettings* client_settings){
    // these are like fixed state. you can only change these by 
    // making your computer faster, colocating, booting faster
    client_settings->initial_wake = (u64)25*60*60*S_TO_NS;
    client_settings->processing_time = (u64)1 * S_TO_NS;
    client_settings->net_latency = (u64)30000000000;

    // could be interesting to randomize these...
    client_settings->is_cash_account = 1;
    client_settings->cash = 100000;
    client_settings->reserved_cash = 0;
    client_settings->buying_power = 100000;
    client_settings->shares = 100;
    client_settings->reserved_shares = 0;

    cz->cs = client_settings;
}

void cz_free(CZ* cz) {
    free(cz);
}

