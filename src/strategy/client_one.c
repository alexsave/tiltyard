#include <stdlib.h>
#include <stdio.h>

#include "strategy/client_one.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"

CO* co_init() {
    CO* a = malloc(sizeof(CO));
    printf("client O is init at %p!\n", (void*)a);
    return a;
}

char* co_get_name(CO* co){
    return "client.1";
}

u8 co_on_snapshot(CO* cz, Context* ctx){

    if (ctx->order_id == MAX_U32){
        printf("order id %u\n", ctx->order_id);
        printf("ok order_id can be treated as broadcast\n");
        exit(1);

    }

    Order* order_ptr = ctx->next_order_ptr;
    // randomly choose between buy and sell
    order_ptr->flags = ((ctx->random >>25) & 1) << BUY_DIRECTION_BIT;
    //order_ptr->flags = (ctx->random & 1) << BUY_DIRECTION_BIT;


    order_ptr->price = (ctx->random >> 8 )& MAX_U16;
    //order_ptr->price = (ctx->random << 8 )& MAX_U16;
    order_ptr->quantity = (ctx->random & MAX_U8) + 1;

    return 1;
}

// i mean we could set these elsehwere, 
// but it seems cleaner to keep client properties in the client
void co_get_settings(CO* co, ClientSettings* client_settings) {
       // these are like fixed state. you can only change these by  
    // making your computer faster, colocating, booting faster
    client_settings->initial_wake = (u64)24*60*60*S_TO_NS;
    client_settings->processing_time = (u64)25 * S_TO_NS;
    client_settings->net_latency = (u64)50000000; 
}

void co_free(CO* co) {
    // whats wrong with this one
    printf("client O is free at %p!\n", (void*)co);
    free(co);
}

