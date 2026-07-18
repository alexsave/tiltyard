#include <stdlib.h>
#include <stdio.h>

#include "strategy/client_one.h"
#include "client_settings.h"
#include "sch.h"
#include "types.h"
#include "constants.h"
#include "order.h"
#include "ob.h"

CO* co_init() {
    CO* a = malloc(sizeof(CO));
    a->cash_guess = 10000000;
    a->shares_guess = 1000;
    a->bid_order_id = MAX_U16;
    a->ask_order_id = MAX_U16;
    return a;
}

char* co_get_name(CO* co){
    return "client.1";
}

u8 co_on_snapshot(CO* co, Context* ctx){

    //printf("co got snapshot\n");
    // now then - let's make some market
    // we also need to connect to WS
    u8 is_ping = ((ctx->status) >> PING_BIT) & 1;
    if (is_ping) {
        printf("co requesting websocket\n");
        ctx->next_order_ptr->status |= (1 << WS_BIT);
        return 1;
    }

    if (ctx->data_snapshot == 0) {
        // it's a ping bit, just try to connect to sw
        // idk
        ctx->next_order_ptr->status |= (1 << PING_BIT);
        return 1;
    }

    u8 is_fill = ((ctx->status) >> FILL_BIT) & 1;
    u8 is_partial = ((ctx->status) >> PARTIAL_FILL_BIT) & 1;
    //if (is_fill) {
    if (ctx->order_id == co->bid_order_id){
        co->shares_guess += ctx->quantity_filled;
        co->cash_guess -= ctx->quantity_filled * ctx->price;
        
        if (!is_partial && is_fill)
            co->bid_order_id = MAX_U16;
        // update guess
    } else if (ctx->order_id == co->ask_order_id) {
        co->shares_guess -= ctx->quantity_filled;
        co->cash_guess += ctx->quantity_filled * ctx->price;

        if (!is_partial && is_fill)
            co->ask_order_id = MAX_U16;
    }
    //}
    // lets see which was filled

    

    MBO * mbo = (MBO*)ctx->data_snapshot;


    // let make sure we have both bid and ask
    if (mbo->hi_bid_index == MAX_U16 || mbo->hi_bid_index == mbo->level_count){
        return 0;
    }

    //u8 is_broadcast = ctx->order_id == MAX_U32;
    
    // 

    // we have both bid and ask
    MBOIndex * bid = mbo->levels + mbo->hi_bid_index;
    MBOIndex * ask = bid + 1;

    u16 mid = ((u32)(bid->price) + (u32)(ask->price)) >> 1;
    u16 distance = ((u32)(ask->price) - (u32)(bid->price)) * 3 / 4;

    // now we're talking

    //printf("mid is %u\n", mid);

    // let's try to get our bid in first

    Order* order_ptr = ctx->next_order_ptr;

    if (co->bid_order_id == MAX_U16) {
        co->bid_order_id = ctx->next_order_id;
        
        order_ptr->price = mid - distance;
        order_ptr->quantity = co->shares_guess >> 2;
        order_ptr->status |= (1 << BUY_DIRECTION_BIT);

    } else if (co->ask_order_id == MAX_U16) {
        co->ask_order_id = ctx->next_order_id;

        order_ptr->price = mid + distance;
        order_ptr->quantity = co->shares_guess >> 2;
        order_ptr->status &= ~(1 << BUY_DIRECTION_BIT);
    }

    return 1;
}

// i mean we could set these elsehwere, 
// but it seems cleaner to keep client properties in the client
void co_get_settings(CO* co, ClientSettings* client_settings) {
       // these are like fixed state. you can only change these by  
    // making your computer faster, colocating, booting faster
    client_settings->initial_wake = (u64)2*60*60*S_TO_NS;
    client_settings->processing_time = (u64) 10000000;
    client_settings->net_latency = (u64)5000000; 

    // could be interesting to randomize these...
    client_settings->is_cash_account = 1;
    client_settings->cash = 10000000;
    client_settings->reserved_cash = 0;
    client_settings->shares = 1000;
    client_settings->reserved_shares = 0;
}

void co_free(CO* co) {
    // whats wrong with this one
    printf("client O is free at %p!\n", (void*)co);
    free(co);
}

