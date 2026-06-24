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
    //printf("client Z is init!\n");
    CZ* cz = malloc(sizeof(CZ));
    cz->cash_guess = 1000000;
    cz->shares_guess = 1000;
    return cz;
}

char* cz_get_name(CZ* cz) {
    return "client.0";
}

u8 cz_on_snapshot(CZ* cz, Context* ctx){

    u8 is_ws = ((ctx->status) >> WS_BIT) & 1;
    u8 is_rej = ((ctx->status) >> REJECT_BIT) & 1;
    u8 is_broadcast = ctx->order_id == MAX_U32;

    // or we try something different - ignroe broadcasts
    if(is_broadcast)
        return 0;

    // broadcast and is ws and is rej impossible
    // broadcast and is ws and not rej - impossible
    // broadcast and not ws and not rej - someone elses trade went through, try under constraints
    // broadcast and not ws and is rej - impossible
    // not broadcast and is ws and is rej impossible
    // not broadcast and is ws and not rej - web socket connected, try agian under constraints
    // not broadcast and not ws and not rej - trade went through, do nothing
    // not broadcast and not ws and is rej - rejection, undo effect of the trade, try again under constriaints

    //if (!is_ws && !is_rej){
        // Trade went through, do nothing
        //return 0;
    //}

    if (!is_ws && is_rej) {
        // undo effect of the trade
        // OH hold on we need the Order itself
        // luckily we dont need to change teh ENTIRE signature, we can just update context

        Order* last = ctx->response_order_ptr;
        if ((last->flags >> BUY_DIRECTION_BIT) & 1) {
            // keep it simpel for now
            cz->cash_guess += last->price * last->quantity;
        } else {
            cz->shares_guess += last->quantity;
        }
    }

    // every other state is impossible or try agian under constraints
    // so try agian under constraints
    
    Order* order_ptr = ctx->next_order_ptr;
    order_ptr->flags = (ctx->random & 1) << BUY_DIRECTION_BIT;

    order_ptr->price = (ctx->random & MAX_U16) >> 3;
    order_ptr->quantity = ((ctx->random & MAX_U8) >> 3) + 1;

    // ok this is great and all
    // but lets do some clinet side validation to chop down on spam
    // while we could do a more through check in main.c, that would be like cheating
    // as main.c has the data RIGHT NOW telling us how much we have
    // this is more realistic

    if ((order_ptr->flags >> BUY_DIRECTION_BIT) & 1) {
        // keep it simpel for now 
        //if (cz->cash_guess < order_ptr->price * order_ptr->quantity)
            //return is_ws;
        cz->cash_guess -= order_ptr->price * order_ptr->quantity;
    } else {
        //if (cz->shares_guess < order_ptr->quantity)
            //return is_ws;
        cz->shares_guess -= order_ptr->quantity;
    }  

    return 1;
}

// I think only this one can modify the settings
// everything else only reads so as to not mess with the server
void cz_get_settings(CZ* cz, ClientSettings* client_settings){
    // these are like fixed state. you can only change these by 
    // making your computer faster, colocating, booting faster
    client_settings->initial_wake = (u64)25*60*60*S_TO_NS;
    client_settings->processing_time = (u64)1 * S_TO_NS;
    client_settings->net_latency = (u64)3 * S_TO_NS;

    // could be interesting to randomize these...
    client_settings->is_cash_account = 1;
    client_settings->cash = 1000000;
    client_settings->reserved_cash = 0;
    //client_settings->buying_power = 100000;
    client_settings->shares = 1000;
    client_settings->reserved_shares = 0;

    // since we cant techincally use it anyways 
    //cz->cs = client_settings;
}

void cz_free(CZ* cz) {
    free(cz);
}

