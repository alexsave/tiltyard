#include <stdlib.h>
#include <stdio.h>

#include "strategy/client_zero.h"
#include "sch.h"
#include "types.h"
#include "order.h"

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


uint64_t cz_initial_wakeup(CZ* cz) {
    return (uint64_t)24*60*60*S_TO_NS;
}

// you turn on the comptuer, then the trading software launches in like 15 seconds?
uint64_t cz_processing_time(CZ* cz) {
    return (uint64_t)15 * S_TO_NS;
}

uint64_t cz_net_latency(CZ* cz) {
    return (uint64_t)300000000;// 300 ms?
}




void cz_free(CZ* cz) {
    free(cz);
}
