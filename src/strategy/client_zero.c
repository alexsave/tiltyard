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
    cz->cash_guess = 10000000;
    cz->shares_guess = 1000;
    cz->waiting_on_id = 0;
    return cz;
}

char* cz_get_name(CZ* cz) {
    return "client.0";
}

u8 cz_on_snapshot(CZ* cz, Context* ctx){
    u8 is_ping = ((ctx->status) >> PING_BIT) & 1;
    u8 is_partial = ((ctx->status) >> PARTIAL_FILL_BIT) & 1;
    u8 is_fill = ((ctx->status) >> FILL_BIT) & 1;
    u8 is_ws = ((ctx->status) >> WS_BIT) & 1;
    u8 is_rej = ((ctx->status) >> REJECT_BIT) & 1;
    u8 is_broadcast = ctx->order_id == MAX_U32;
    if (is_partial){
        printf("MY ORDER FILLED PARTIAL %u\n", ctx->order_id, ctx->quantity_filled);
        //return 0;
    } else if (is_fill){
        printf("MY ORDER FILLED %u\n", ctx->order_id, ctx->quantity_filled);
        //return 0;
    }

    // we really shoudl check fills and update them against our cash guess and shares guess
    // but how can we know?
    // if a stock is partially filled multiple times, its tough to know it went 5 -> 3-> 1 -> 0
    // so maybe we need to encode "last amount filled by" somewhere
    // and if we get an order with quantity zero we know it was just now, filled entirely
    // gonna sleep on this

    if (cz->waiting_on_id == MAX_U32 || ctx->order_id == cz->waiting_on_id){
        // I guess this has the side effect of allowing broadcats to "wake us up"
        // go on, and wait for this next one
        cz->waiting_on_id = ctx->next_order_id;
    } else {
        //printf("got %u but nope, still waiting on order %u\n", ctx->order_id, cz->waiting_on_id);
        return 0;
    }
    //printf("got %u finally, ok lets fill out order %u and send it off\n", ctx->order_id, cz->waiting_on_id);

    // or we try something different - ignroe broadcasts
    ////if(is_broadcast)
        //return 0;

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

        // not allowed to do this
        /*Order* last = ctx->response_order_ptr;
        if ((last->status >> BUY_DIRECTION_BIT) & 1) {
            // keep it simpel for now
            cz->cash_guess += last->price * last->quantity;
        } else {
            cz->shares_guess += last->quantity;
        }*/
    }

    // every other state is impossible or try agian under constraints
    // so try agian under constraints
    
    Order* order_ptr = ctx->next_order_ptr;
    order_ptr->status = (ctx->random & 1) << BUY_DIRECTION_BIT;

    order_ptr->price = (ctx->random & MAX_U16) >> 3;
    order_ptr->quantity = ((ctx->random & MAX_U8) >> 3) + 1;

    if ((ctx->random & MAX_U8) > 200) {
        // try to cancel somestimes, specifically the one in response to this
        order_ptr->status |= (1 << CANCEL_BIT);
        order_ptr->other_id = ctx->order_id;
    }

    if (is_ping) {
        // switch to sw
        //order_ptr->status |= (1 << WS_BIT);
        // should only be like 5 of these
    }

    // ok this is great and all
    // but lets do some clinet side validation to chop down on spam
    // while we could do a more through check in main.c, that would be like cheating
    // as main.c has the data RIGHT NOW telling us how much we have
    // this is more realistic

    if ((order_ptr->status >> BUY_DIRECTION_BIT) & 1) {
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
    client_settings->cash = 10000000;
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

