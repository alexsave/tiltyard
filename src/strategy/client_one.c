#include <stdlib.h>
#include <stdio.h>

#include "strategy/client_one.h"
#include "sch.h"
#include "types.h"
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
    Order* order_ptr = ctx->order_ptr;
    // randomly choose between buy and sell
    order_ptr->flags = (ctx->random & 1) << BUY_DIRECTION_BIT;


    order_ptr->price = ctx->random & MAX_U16;
    order_ptr->quantity = ctx->random & MAX_U8;

    return 1;
}

u64 co_initial_wakeup(CO* co) {
    return (uint64_t)2*24*60*60*S_TO_NS;
}

// you turn on the comptuer, then the trading software launches in like 15 seconds?
u64 co_processing_time(CO* co) {
    return (uint64_t)25 * S_TO_NS;
}

u64 co_net_latency(CO* co) {
    return (uint64_t)300000000;// 300 ms?
}

void co_free(CO* co) {
    // whats wrong with this one
    printf("client O is free at %p!\n", (void*)co);
    free(co);
}

