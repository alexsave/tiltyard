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
#include "fill.h"

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
    sc->sw_queue = cb_init(sizeof(u32));
    sc->hw_queue = cb_init(sizeof(u32));
    sc->convert_holder = cb_init(sizeof(u32));

    sc->orders = fl_init(sizeof(Order), MIN_RESERVED_PACKET);
    sc->fills = cb_init(sizeof(Fill));
    sc->responses = fl_init(sizeof(Response), MAX_U32);

    sc->rand = rand_init(seed);
    rand_next(sc->rand);
    sc->sch = sch_init(sc->rand);

    return sc;
}

void schedule_response(ServerContext* sc, u32 client_id, u16 status, u32 quantity_filled, u32 order_id) {
    bs_bump_refs(sc->mbo_bs, sc->last_mbo);
    Response r = {.snapshot_id = sc->last_mbo, .client_id = client_id, .status = status, .order_id = order_id, .quantity_filled = quantity_filled};
    u32 response_id = fl_insert(sc->responses, &r);
    u64 response_event = build_event(CLIENT_IN_TYPE, response_id);
    sch_schedule(sc->sch, response_event, calculate_jitter(sc->client_settings + (client_id), sc->rand));
    // so don't blast over websocket
    sc->client_settings[client_id].will_notify = 1;
}

void server_cancel_order(ServerContext* sc, u32 exec_order_id) {
    ClientSettings* client_settings = sc->client_settings;
    BS* mbo_bs = sc->mbo_bs;
    Holder* ho = sc->ho;
    FL* orders = sc->orders;

    Order* in = (Order*)fl_get(orders, exec_order_id);
    u8 status = 0;
    u8 will_modify = 0;
    printf("cancel event %u!\n", in->other_id);
    // different checks
    // the usual ones check if the money and quantity is right
    // this one is easier maybe
    // what could prevent us from cancelling?
    // 1. we dont own the order
    // 2. it's 100% filled, ie not in the book

    Order* to_cancel = (Order*)fl_get(orders, in->other_id);
    // easy checks 
    u8 is_ours = to_cancel->client_id == in->client_id;
    u8 is_active = to_cancel->quantity > 0;

    // should we set the cancellation bit in the rejection response?
    // or will they know by the id?
    // its not liek there will be a second response along the lines of "your order was cancelled"
    // no, the repsonse to this cancel will either be "your order was cancelled" or "your cancellation order was rejected"
    // ah but wait its kinda like if you have a fill, you need to provide the id
    // so we should send some special thing back to the client like 
    // THIS order was cancelled

    status |= (1 << CANCEL_BIT);

    if (!is_ours || !is_active)// reject the cancel
        will_modify = 0;
    else
        will_modify = 1;

    // aight then fuck it
    // what do we know
    // if the cancel fails, it goes only to the self
    // if the cancel succeeds, it goes only to the self + websocket listeners
    // there is no "other client" that is exclusively notified, you cancel yourself

    // simpler case
    if (!will_modify){
        printf("REJECTED\n");
        status |= (1<<REJECT_BIT);

        schedule_response(sc, in->client_id, status, 0, exec_order_id);
        return;
    }

    printf("CANCEL ACCEPTED %u!\n", in->other_id);

    u32 old_size = mbo_bs->metadata[sc->last_mbo].size;
    u32 max_new_size = old_size + sizeof(MBOIndex) + sizeof(MBOLevel) + sizeof(MBOEntry);

    void* new_mbo_raw;
    u32 next_last_mbo = bs_reserve(mbo_bs, max_new_size, 1, &new_mbo_raw);
    void* old_mbo_raw = bs_get_no_ref(mbo_bs, sc->last_mbo);

    u32 new_size = ob_cancel(to_cancel, in->other_id, old_mbo_raw, new_mbo_raw);
    bs_resize(sc->mbo_bs, new_size);

    sc->last_mbo = next_last_mbo;

    // ok so thats just the OB manipulation
    // now we need to actually do accounting

    // it's not a trade per se
    u8 was_buy = ((to_cancel->status) >> BUY_DIRECTION_BIT ) & 1;

    for(u32 i = 0; i < ho->num_clients; i++) {
        ClientSettings* cs = sc->client_settings + i;
        printf("from client id #%u [$%u/$%u/%ush/%ush]\n", i, cs->cash, cs->reserved_cash, cs->shares, cs->reserved_shares);
    }  

    // was buy/bid, need to unreserve money
    // was sell/ask, need to unrserve shares
    if (was_buy){
        client_settings[in->client_id].reserved_cash -= to_cancel->quantity * to_cancel->price;
    } else {
        client_settings[in->client_id].reserved_shares -= to_cancel->quantity;
    }

    // special response for self,

    schedule_response(sc, in->client_id, status, 0, exec_order_id);

    // final broadcast send
    for (u32 ci = 0; ci < ho->num_clients; ci++){
        if (client_settings[ci].will_notify == 0 && client_settings[ci].ws) {
            schedule_response(sc, ci, 0, 0, MAX_U32);
        }
        // reset
        client_settings[ci].will_notify = 0;
    }

    mbo_dump(old_mbo_raw);
    mbo_dump(new_mbo_raw);
    for(u32 i = 0; i < ho->num_clients; i++) {
        ClientSettings* cs = sc->client_settings + i;
        printf("from client id #%u [$%u/$%u/%ush/%ush]\n", i, cs->cash, cs->reserved_cash, cs->shares, cs->reserved_shares);
    }  
    //exit(1);

}

// checks the cancel order id valididtiy
u8 cancel_precheck(Order* in, FL* orders) {
    Order* to_cancel = (Order*)fl_get(orders, in->other_id);
    // easy checks
    u8 is_ours = to_cancel->client_id == in->client_id;
    u8 is_active = to_cancel->quantity > 0;

    return is_ours && is_active;
}

// the usual stuff
u8 add_precheck(Order* in, ClientSettings* cs) {
    u8 valid;

    u32 before_quantity = in->quantity;
    u32 in_cost = before_quantity * in->price;
    u8 is_buy = (in->status >> BUY_DIRECTION_BIT) & 1;
    u8 has_bp = (cs->cash - cs->reserved_cash) >= in_cost;
    u8 has_shares = (cs->shares - cs->reserved_shares) >= before_quantity;
    u8 is_valid_quantity = in->quantity > 0;
    u8 is_valid_price = in->price > 0;

    if(is_buy)
        valid = has_bp & is_valid_quantity & is_valid_price;
    else 
        valid = has_shares & is_valid_quantity & is_valid_price;
    return valid;
}

// much better
// the big driver of all market book stuff
// this mostly takes care of scheduling, then passes it off to server_order
void server_order(ServerContext* sc, u32 exec_order_id) {

    // maybe I rename at least the struct names
    //u32 last_mbo = sc->last_mbo;
    ClientSettings* client_settings = sc->client_settings;
    BS* mbo_bs = sc->mbo_bs;
    SCH* sch = sc->sch;
    Holder* ho = sc->ho;
    FL* orders = sc->orders;
    CB* fills = sc->fills;
    u64 now_ns = sch_now_ns(sch);

    Order* in = (Order*)fl_get(orders, exec_order_id);

    ClientSettings* cs = (client_settings + in->client_id);

    u16 status = 0;

    // for now we'll just handle socket connections
    u8 is_toggle_ws = (in->status & (1 << WS_BIT));
    if (is_toggle_ws) {
        cs->ws = !(cs->ws);
        status |= (1 << WS_BIT);
    }
    u8 is_ping = (in->status >> PING_BIT) & 1;
    if (is_ping)
        status |= 1 << PING_BIT;

    // we should have prechecks for cancel orders too


    u8 is_can_rep = (in->status >> CAN_REP_BIT) & 1;
    u8 is_cancel = (in->status >> CANCEL_BIT) & 1;

    u8 will_modify;
    if (is_can_rep){
        will_modify = cancel_precheck(in, orders);
        will_modify &= add_precheck(in, cs);
        status |= (1 << CAN_REP_BIT);
    } else if (is_cancel) {
        will_modify = cancel_precheck(in, orders);
        status |= (1 << CANCEL_BIT);
    } else {
        will_modify = add_precheck(in, cs);
    }

    // honestly if we truly have a method that can handle everything, we can send it here
    // but we do need to do prechecks
    // which could certainly split out into other methods


    u8 is_buy = (in->status >> BUY_DIRECTION_BIT) & 1;
    u32 before_quantity = in->quantity;
    u32 in_cost = before_quantity * in->price;

    // next big challenge cancelreplace orders
    // so now we need like a cancel id to bundle into this
    // may as well use the padding we have

    // and we needyet another flag
    // more interestingly, what will this look like in the actual OB


    // you can have a "ping order", so order without the websocket stuff

    //u8 is_pure_get = !is_valid_quantity & is_ping;

    /*

       if(is_buy && !has_bp) 
       printf("REJ $%u > $%u\n", in_cost, cs->cash - cs->reserved_cash);

       if(!is_buy && !has_shares) 
       printf("REJ %u > %u\n", before_quantity, cs->shares - cs->reserved_shares);
     */

    if(!will_modify){
        status |= (1<<REJECT_BIT);
        schedule_response(sc, in->client_id, status, 0, exec_order_id);
        return;
    }


    printf("[%llus] order #%u buy %u quantity %u price %u client #%u [$%u/$%u/%uq/%uq] ", now_ns/S_TO_NS, exec_order_id, is_buy, in->quantity, in->price, in->client_id, cs->cash, cs->reserved_cash, cs->shares, cs->reserved_shares);
    printf("accepted\n");

    // mbo_dump + used to create next snapshot
    // us, plus at least the one who sent request?

    u32 prev_last_mbo = sc->last_mbo;
    //if(exec_order_id > 2000000){
    //mbo_dump(mbo);
    //exit(1);
    //}

    // this WILL modify the in Order, to modify quantity
    // it's either that or we mess with the return value somehow

    u32 old_size = mbo_bs->metadata[sc->last_mbo].size;
    // new resting order on new price level, requring an additional index, additional level header, and additional level entry
    u32 max_new_size = old_size + sizeof(MBOIndex) + sizeof(MBOLevel) + sizeof(MBOEntry);

    void* new_mbo_raw;
    u32 next_last_mbo = bs_reserve(mbo_bs, max_new_size, 1, &new_mbo_raw);
    void* old_mbo_raw = bs_get_no_ref(mbo_bs, sc->last_mbo);

    u32 new_size = ob_canrep(orders, exec_order_id, old_mbo_raw, new_mbo_raw, fills);
    printf("new size %u\n", new_size);
    //if (exec_order_id == 24){
        printf("old\n");
        mbo_dump(old_mbo_raw);

        printf("new\n");
        mbo_dump(new_mbo_raw);
        //exit(1);
    //}

    sc->last_mbo = next_last_mbo;

    bs_resize(mbo_bs, new_size);

    // ^ but this is just for our client of incoming order
    // we still need to go through and fill the orders we hit
    // just dont update the incoming order client after this "taker"

    if (in->quantity < before_quantity){
        status |= (1 << FILL_BIT);
        if (in->quantity > 0)
            status |= (1 << PARTIAL_FILL_BIT);
    }

    // check exec_order_id to see if we had a partial fill
    if (is_buy) 
        cs->reserved_cash += in->quantity * in->price;
    else 
        cs->reserved_shares += in->quantity;

    // ok now we have fills and partial_id maybe
    // partial id will be filled last by definiton
    while (!cb_is_empty(fills)){
        // this guy is actuall responsible for ensuring "orders" fl is updated

        Fill* fill = (Fill*)cb_deque(fills);
        // its filled, we dont need it anymore I think
        // we could probably release, but it's confusing right now
        //printf("order releasing due to fill %u\n", filled_order_id);
        Order* order = (Order*)fl_get(orders, fill->order_id);

        u32 q = fill->quantity_filled;//order->quantity;
        u32 cost = order->price * q;

        printf("TRADE buy %u p %u q %u id %u now %llu part %u\n", (in->status >> BUY_DIRECTION_BIT) & 1, order->price, q, fill->order_id, now_ns, fill->partial);

        printf("order q before %u\n", order->quantity);
        order->quantity -= q;
        // its either do it here, or have the ob file take on even more responsibility for handling stuff
        in->quantity -= q;
        printf("order q after %u\n", order->quantity);

        u32 maker = order->client_id;

        ClientSettings* mcs = (client_settings + maker);

        if (is_buy){
            cs->cash -= cost;
            cs->shares += q;
            mcs->cash += cost;
            mcs->shares -= 1;
            mcs->reserved_shares -= 1;
        } else {
            cs->shares -= q;
            cs->cash += cost;
            mcs->shares += q;
            mcs->cash -= cost;
            mcs->reserved_cash -= cost;
        }

        u16 fstatus = 1 << FILL_BIT;
        if (fill->partial) {
            fstatus |= 1 << PARTIAL_FILL_BIT;
        }

        // we should genuinely update teh order->quantity because cancels rely on it
        

        printf("scheduling response %u\n", fill->order_id);
        schedule_response(sc, maker, fstatus, q, fill->order_id);

    }

    bs_get(mbo_bs, prev_last_mbo);
    //mbo_dump(bs_get_no_ref(mbo_bs, sc->last_mbo));

    // i know its ugly

    // send special one to self
    printf("scheduling response %u\n", exec_order_id);
    schedule_response(sc, in->client_id, status, (before_quantity - in->quantity), exec_order_id);

    // final broadcast send
    for (u32 ci = 0; ci < ho->num_clients; ci++){
        if (client_settings[ci].will_notify == 0 && client_settings[ci].ws) {
            schedule_response(sc, ci, 0, 0, MAX_U32);
        }
        // reset
        client_settings[ci].will_notify = 0;
    }
} 

void server_exec_end(ServerContext* sc) {
    SCH* sch = sc->sch;
    CB* sw_queue = sc->sw_queue;

    if (cb_is_empty(sw_queue)) {
        //really weird
        sc->executing = 0;
        return;
    }

    u32 exec_order_id = *(u32*)cb_deque(sw_queue);

    server_order(sc, exec_order_id);

    /*
       if (need to convert stops)
       schedule EXEC_TO_SW_ID eevent
       u64 SW_TO_EXEC_DELAY = 100;
       u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_TO_SW_ID & PARAM_MASK);
       sch_schedule(sch, socket_event, SW_TO_EXEC_DELAY);
       cb_queue(&(sc->convert_holder));
       cb_queue(&CONVERT_SENTINEL_VALUE);

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
    cb_queue(sc->hw_queue, &order_id);

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
    u32 moving_order = *(u32*)cb_deque(sc->hw_queue); 
    // handle zero case
    //printf("hw to sw move requested for order %u\n", moving_order);

    // If it's not empty, someone has already scheduled an exec_start_id
    if (cb_is_empty(sc->sw_queue)) {
        u64 SW_TO_EXEC_DELAY = 100;
        u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_START_ID & PARAM_MASK);
        sch_schedule(sc->sch, socket_event, SW_TO_EXEC_DELAY);
    }   

    cb_queue(sc->sw_queue, &moving_order);
}

void server_exec_to_sw(ServerContext* sc){
    u32 synth_order_id = *(u32*)cb_deque(sc->convert_holder);

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
        cb_queue(sc->sw_queue, &synth_order_id);
        synth_order_id = *(u32*)cb_deque(sc->convert_holder);
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

