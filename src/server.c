#include <stdio.h>
#include <stdlib.h>

#include "server.h"
#include "types.h"
#include "constants.h"

#include "order.h"
#include "utils.h"
#include "client_settings.h"
#include "bs.h"
#include "fl.h"
#include "cb.h"
#include "sch.h"
#include "response.h"
#include "holder.h"
#include "ob.h"
#include "rand.h"

// yeah you need these two to initialize the server, cuz really it initalizes everything
ServerContext* server_init(TypeMetadata* tm, u32 * client_allocations, u64 seed){
    ServerContext* sc = malloc(sizeof(ServerContext));

    /// ahhhh catch 22
    // we cant know how many clients to reserve BEFORE going through this
    //sc->client_settings = calloc(sc->ho->num_clients, sizeof(ClientSettings));
    sc->ho = holder_init(tm, client_allocations, &(sc->client_settings));

    sc->client_allocations = client_allocations;

    sc->mbo_bs = bs_init(32768);

    void* mbo_address = 0;
    sc->last_mbo = bs_reserve(sc->mbo_bs, sizeof(MBO), 1, &mbo_address);
    ((MBO*)mbo_address)->level_count = 0;
    ((MBO*)mbo_address)->hi_bid_index = MAX_U16;

    sc->executing = 0;
    sc->sw_queue = cb_init();
    sc->hw_queue = cb_init();
    sc->convert_holder = cb_init();

    sc->orders = fl_init(sizeof(Order), MIN_RESERVED_PACKET);
    sc->fills = cb_init();
    sc->responses = fl_init(sizeof(Response), MAX_U32);

    sc->rand = rand_init(seed);
    rand_next(sc->rand);
    sc->sch = sch_init(sc->rand);

    return sc;
}


// much better
// the big driver of all market book stuff
void server_exec_end(ServerContext* sc) {

    // maybe I rename at least the struct names
    //u32 last_mbo = sc->last_mbo;
    ClientSettings* client_settings = sc->client_settings;
    BS* mbo_bs = sc->mbo_bs;
    SCH* sch = sc->sch;
    Holder* ho = sc->ho;
    FL* responses = sc->responses;
    FL* orders = sc->orders;
    CB* sw_queue = sc->sw_queue;
    CB* fills = sc->fills;
    u64 now_ns = sch_now_ns(sch);

    if (cb_is_empty(sw_queue)) {
        //really weird
        sc->executing = 0;
        return;
    }


    u32 exec_order_id = cb_deque(sw_queue);
    //printf("exec finished on order %u\n", exec_order_id);

    Order* in = (Order*)fl_get(orders, exec_order_id);

    u32 agro = in->client_id;

    //printf("argro is %u btw\n" , agro);

    u8 will_modify_ob = 0;

    u32 before_quantity = in->quantity;
    u32 in_cost = before_quantity * in->price;
    u8 is_buy = (in->flags >> BUY_DIRECTION_BIT) & 1;

    ClientSettings* cs = (client_settings + agro);//?

    u8 has_bp = (cs->cash - cs->reserved_cash) >= in_cost;
    u8 has_shares = (cs->shares - cs->reserved_shares) >= before_quantity;
    u8 is_valid_quantity = in->quantity > 0;
    u8 is_valid_price = in->price > 0;
    u8 is_toggle_ws = (in->flags & (1 << WS_BIT));

    u8 will_modify;
    if(is_buy)
        will_modify = has_bp & is_valid_quantity & is_valid_price;
    else 
        will_modify = has_shares & is_valid_quantity & is_valid_price;

    if (!will_modify)
        bs_bump_refs(mbo_bs, sc->last_mbo);

    // for now we'll just handle socket connections
    if (is_toggle_ws) 
        client_settings[agro].ws = !(client_settings[agro].ws);

    /*
    printf("[%us] order #%u buy %u quantity %u price %u client #%u [$%u/$%u/%uq/%uq] ", now_ns/S_TO_NS, exec_order_id, is_buy, in->quantity, in->price, agro, client_settings[agro].cash, client_settings[agro].reserved_cash, client_settings[agro].shares, client_settings[agro].reserved_shares);

    if(is_buy && !has_bp) 
        printf("REJ $%u > $%u\n", in_cost, cs->cash - cs->reserved_cash);

    if(!is_buy && !has_shares) 
        printf("REJ %u > %u\n", before_quantity, cs->shares - cs->reserved_shares);
    */

    if (will_modify){
        printf("[%us] order #%u buy %u quantity %u price %u client #%u [$%u/$%u/%uq/%uq] ", now_ns/S_TO_NS, exec_order_id, is_buy, in->quantity, in->price, agro, client_settings[agro].cash, client_settings[agro].reserved_cash, client_settings[agro].shares, client_settings[agro].reserved_shares);
        printf("accepted\n");
    }

    u8 REJECT_BIT = 0;

    u8 status = 0;

    if (!will_modify) {
        // drop the order, we dont need it anymore. they'll know it was rejected but we need the space back
        status |= (1<<REJECT_BIT);
        fl_release(orders, exec_order_id);
    }

    //void* mbo = bs_get_no_ref(mbo_bs, last_mbo);

    // calculate ref count AFTER setting ws lol
    if (will_modify) {
        //printf("operating on id %u\n", exec_order_id);

        // mbo_dump + used to create next snapshot
        u16 ref_count = 1;

        // todo create l3 list later
        for (u32 ci = 0; ci < ho->num_clients; ci++){
            if(client_settings[ci].ws) {
                ref_count++;
            }
        }

        u32 prev_last_mbo = sc->last_mbo;
        //if(exec_order_id > 2000000){
        //mbo_dump(mbo);
        //exit(1);
        //}

        u32 partial_fill_id = MAX_U32;
        u32 partial_fill_q = 0;

        sc->last_mbo = ob_limit(exec_order_id, orders, sc->last_mbo, mbo_bs, ref_count, fills, &partial_fill_id, &partial_fill_q);


        // lets go case by case
        // cash account - resting limit made

        // ^ but this is just for our client of incoming order
        // we still need to go through and fill the orders we hit
        // just dont update the incoming order client after this "taker"


        // check exec_order_id to see if we had a partial fill
        if (is_buy) {
            //client_settings[agro].buying_power -= in->quantity * in->price;
            //printf("decrementing reserved by 
            // yeah i think this is correct, in->quantity shoudl be left at 0
            // ah- marketable limits dont affect RESERVED cash, only cash itself
            client_settings[agro].reserved_cash += in->quantity * in->price;
        } else {
            //client_settings[agro].shares -= in->quantity;
            client_settings[agro].reserved_shares += in->quantity;
        }


        // now that it went through we need to update the buying power of the dude
        //client_settings[taker].buying_power -= in_cost;

        // ok now we have fills and partial_id maybe
        // partial id will be filled last by definiton
        while (!cb_is_empty(fills)){
            u32 filled_order_id = cb_deque(fills);
            // its filled, we dont need it anymore I think
            // we could probably release, but it's confusing right now
            Order* order = (Order*)fl_get(orders, filled_order_id);

            printf("TRADE %u %u %u %u %llu\n", (in->flags >> BUY_DIRECTION_BIT) & 1, order->price, order->quantity, filled_order_id, now_ns);


            u32 cost = order->price * order->quantity;

            u32 taker = in->client_id;
            u32 maker = order->client_id;
            //transfer(order, in, client_settings);

            u32 q = order->quantity;

            if (is_buy){
                cs->cash -= cost;
                cs->shares += q;
                client_settings[maker].cash += cost;
                client_settings[maker].shares -= q;
                client_settings[maker].reserved_shares -= q;
            } else {
                cs->shares -= order->quantity;
                cs->cash += cost;
                client_settings[maker].shares += q;
                client_settings[maker].cash -= cost;
                client_settings[maker].reserved_cash -= cost;
            }


        }

        if (partial_fill_id != MAX_U32) {
            // todo might be cleaner to just replace with an identical order and return the old one modified as if it were that "partial_fill_q"
            Order* order = (Order*)fl_get(orders, partial_fill_id);

            // remaining quantity? quantity is a bit tricky here
            printf("TRADE %u %u %u %u %llu PARTIAL\n", (in->flags >> BUY_DIRECTION_BIT) & 1, order->price, partial_fill_q, partial_fill_id, now_ns);

            // ok transfer $$$
            u32 cost = order->price * partial_fill_q;

            u32 taker = in->client_id;
            u32 maker = order->client_id;

            u32 q = partial_fill_q;

            if (is_buy){
                cs->cash -= cost;
                cs->shares += q;
                client_settings[maker].cash += cost;
                client_settings[maker].shares -= q;
                client_settings[maker].reserved_shares -= q;
            } else {
                cs->shares -= order->quantity;
                cs->cash += cost;
                client_settings[maker].shares += q;
                client_settings[maker].cash -= cost;
                client_settings[maker].reserved_cash -= cost;
            }

        }

        //

        bs_get(mbo_bs, prev_last_mbo);
        //mbo_dump(bs_get_no_ref(mbo_bs, sc->last_mbo));

        //bit hacky but ensures we can get the first snpashot into processing
        //u8 status = ob_limit(in->flags & (1 << BUY_DIRECTION_BIT), in->quantity, in, mbo_address, mbp_address, mbo_bs, mbp_bs);
    }

    if (is_toggle_ws){
        printf("status before togglign ws bit %u\n", status);
        status |= (1 << WS_BIT);
        printf("status after togglign ws bit %u\n", status);
    }

    if (!will_modify){
        // only send reject to that one client, later
        Response r = {.client_id = in->client_id, .snapshot_id = sc->last_mbo, .status=status, .order_id = exec_order_id};
        u32 response_id = fl_insert(responses, &r);

        u64 response_event = ((CLIENT_IN_TYPE & T_MASK) << PARAM_BITS) | (response_id & PARAM_MASK);
        sch_schedule(sch, response_event, calculate_jitter(client_settings + (in->client_id), sc->rand));

    } else {
        // accepted orders will modify this
        //u32 server_snapshot = handle;

        // i know its ugly
        for (u32 ci = 0; ci < ho->num_clients; ci++){
            if((ci != in->client_id) && (client_settings[ci].ws)) {
                //printf("making response for %u\n", ci);
                // make a new response for them
                Response r = {.client_id = ci, .snapshot_id = sc->last_mbo, .order_id = MAX_U32, .status=0};
                u32 response_id = fl_insert(responses, &r);
                u64 response_event = ((CLIENT_IN_TYPE & T_MASK) << PARAM_BITS) | (response_id & PARAM_MASK);

                //.. delay is tricky, we actually need to go get client values again
                // probably using holder.
                // but for now let's just say exactly 100ms lol
                sch_schedule(sch, response_event, calculate_jitter(client_settings + (ci), sc->rand)); 
                rand_next(sc->rand);
            }
        }

        // send special one to self
        Response r = {.client_id = in->client_id, .snapshot_id = sc->last_mbo, .order_id = exec_order_id, .status=status};
        u32 response_id = fl_insert(responses, &r);
        u64 response_event = ((CLIENT_IN_TYPE & T_MASK) << PARAM_BITS) | (response_id & PARAM_MASK);

        //.. delay is tricky, we actually need to go get client values again
        // probably using holder.
        // but for now let's just say exactly 100ms lol
        sch_schedule(sch, response_event, calculate_jitter(client_settings + (in->client_id), sc->rand)); 
    }


    /*
       if (need to convert stops)
       schedule EXEC_TO_SW_ID eevent
       u64 SW_TO_EXEC_DELAY = 100;
       u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_TO_SW_ID & PARAM_MASK);
       sch_schedule(sch, socket_event, SW_TO_EXEC_DELAY);
       cb_queue(sc->convert_holder);
       cb_queue(CONVERT_SENTINEL_VALUE);

     */

    if (cb_is_empty(sw_queue)){
        sc->executing = 0;
    } else {
        u64 EXEC_TIME = 10;
        u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_END_ID & PARAM_MASK);
        sch_schedule(sch, socket_event, EXEC_TIME);
    }
} 

void server_arrival(ServerContext* sc, u32 order_id) {
    //printf("order %llu arrives at server\n", order_id);
    cb_queue(sc->hw_queue, order_id);

    u64 HW_TO_SW_DELAY = 10000;
    u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (HW_TO_SW_ID & PARAM_MASK);
    sch_schedule(sc->sch, socket_event, HW_TO_SW_DELAY);
}

void server_exec_start(ServerContext* sc) {
    // This relies on the server->executing state and
    // the fact that an EXEC_END_ID is scheduled are in sync

    // cb is empty and executing - on the last order
    // cb is empty and not executing - do nothing
    // cb not empty and executing - don't do anything
    // cb not empty and not executing - do somethign

    if (sc->executing || cb_is_empty(sc->sw_queue)) {
        // do nothing
        return;
    }

    //printf("exec started\n");

    sc->executing = 1;

    u64 EXEC_TIME = 10;
    u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_END_ID & PARAM_MASK);
    sch_schedule(sc->sch, socket_event, EXEC_TIME);
}

void server_hw_to_sw(ServerContext* sc) {
    if (cb_is_empty(sc->hw_queue)) {
        //weird
        return;
    }   
    u32 moving_order = cb_deque(sc->hw_queue); 
    // handle zero case
    //printf("hw to sw move requested for order %u\n", moving_order);

    // If it's not empty, someone has already scheduled an exec_start_id
    if (cb_is_empty(sc->sw_queue)) {
        u64 SW_TO_EXEC_DELAY = 100;
        u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_START_ID & PARAM_MASK);
        sch_schedule(sc->sch, socket_event, SW_TO_EXEC_DELAY);
    }   

    cb_queue(sc->sw_queue, moving_order);

}

void server_exec_to_sw(ServerContext* sc){
    u32 synth_order_id = cb_deque(sc->convert_holder);

    if (cb_is_empty(sc->convert_holder) || synth_order_id == CONVERT_SENTINEL_VALUE) {
        return;
    }

    if (cb_is_empty(sc->sw_queue)) { 
        u64 SW_TO_EXEC_DELAY = 100;
        u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_START_ID & PARAM_MASK);
        sch_schedule(sc->sch, socket_event, SW_TO_EXEC_DELAY);
    }

    // this will pop the last CONVERT_SENTINEL_VALUE becuase we deque before we check
    while(!cb_is_empty(sc->convert_holder) && synth_order_id != CONVERT_SENTINEL_VALUE) {
        cb_queue(sc->sw_queue, synth_order_id);
        synth_order_id = cb_deque(sc->convert_holder);
    }       

}

// may as well

void server_free(ServerContext* sc) {
    holder_free(sc->ho);
    sch_free(sc->sch);
    bs_free(sc->mbo_bs);
    free(sc->client_settings);
    fl_free(sc->responses);
    fl_free(sc->orders);
    cb_free(sc->fills);
    cb_free(sc->sw_queue);
    cb_free(sc->hw_queue);
    cb_free(sc->convert_holder);

    free(sc->rand);

    free(sc);
}

