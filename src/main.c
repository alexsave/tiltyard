#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "rand.h"
#include "sch.h"
#include "fl.h"
#include "bs.h"
#include "cb.h"
#include "types.h"
#include "constants.h"
#include "response.h"
#include "client.h"
#include "server.h"
#include "client_settings.h"
#include "holder.h"
#include "order.h"
#include "utils.h"

#include "ob.h"

void log_full(uint64_t raw) {
    printf("raw: %llu, priority %llu, type %llu, params %llu\n", 
            raw, 
            raw >> E_BITS, 
            (raw >> (E_BITS-T_BITS)) & T_MASK, 
            raw & PARAM_MASK);
}

int main(int argc, char* argv[]){
    TypeMetadata* tm = get_types();
    u32 * client_allocations = malloc(tm->IMPLS_COUNT * sizeof(u32*));

    // now we can do
    client_allocations[tm->cz_index] = 1;
    client_allocations[tm->co_index] = 1;

    ServerContext* sc = server_init(tm, client_allocations, 603);

    // genuinely needed everywhere
    SCH* sch = sc->sch;

    uint64_t repeat_event = build_event(CONTROL_TYPE, CONTROL_PARAM_SLOW);
    sch_schedule(sch, repeat_event, 0);

    u64 kill_event = build_event(CONTROL_TYPE, CONTROL_PARAM_KILL);
    sch_schedule(sch, kill_event, 2*(24*60*60) *S_TO_NS);

    Holder* ho = sc->ho;
    ClientSettings* client_settings = sc->client_settings;
    for(u32 i = 0; i < ho->num_clients; i++) {
        uint64_t client_id = i; 
        
        uint64_t boot_event = build_event(CONTROL_TYPE, client_id);
        sch_schedule(sch, boot_event, client_settings[i].initial_wake);
    }

    FL* orders = sc->orders;
    FL* responses = sc->responses;
    BS* mbo_bs = sc->mbo_bs;

    Context* context = malloc(sizeof(Context));

    while(1){
        rand_next(sc->rand);
        context->random = (*(sc->rand)) & MAX_U32;

        uint64_t next = sch_pop(sch);

        uint64_t now_ns = sch_now_ns(sch);
        //printf("NOW %llu ~%llus - ", now_ns, now_ns/1000000000);
        //log_full(next);

        uint8_t type = (next >> PARAM_BITS) & T_MASK;

        uint64_t params = next & PARAM_MASK;

        // different from waht is below
        if (type == SERVER_TYPE) {
            // something in the server
            //printf("server type\n");

            u64 order_id = params;


            // order arrives to server
            if (order_id < MIN_RESERVED_PACKET) {
                server_arrival(sc, order_id);
            } else if (order_id == HW_TO_SW_ID) {
                server_hw_to_sw(sc);
            } else if (order_id == EXEC_START_ID) {
                server_exec_start(sc);
            } else if (order_id == EXEC_END_ID) {
                //printf("NOW %llu ~%llus \n", now_ns, now_ns/1000000000);
                server_exec_end(sc);
            } else if (order_id == EXEC_TO_SW_ID) {
                // kinda broken for now
                server_exec_to_sw(sc);
            }
        } else if (type == CLIENT_IN_TYPE) {
            //printf("client in type\n");

            // now how do we handle a ping event

            u32 response_id = params;

            Response response = *(Response*)fl_release(responses, response_id);

            u32 client_id = response.client_id;
            u32 snapshot_id = response.snapshot_id;
            u16 status = response.status;

            Order tmp = {};
            u32 new_order_id = fl_insert(orders, &tmp);
            Order* empty = fl_get(orders, new_order_id);

            context->next_order_id = new_order_id;
            context->next_order_ptr = empty;

            context->status = status;
            context->quantity_filled = response.quantity_filled;
            context->price = response.price;
            
            if ((status >> PING_BIT) & 1) {
                // the client can schedule these from themselves kinda
                // so this will not come with any knowledge of snapshot
                // and this is not in response to an order id 
                // this just wakes them up
                context->mbo_snapshot = 0;
            } else {
                context->mbo_snapshot = bs_get(mbo_bs, snapshot_id);
                context->order_id = response.order_id;
                // they cant use it anyways
                //if (response.order_id != MAX_U32)
                    //context->response_order_ptr = (Order*)fl_get(orders, response.order_id);
            }

            //printf("sending to client %u order %u\n", client_id, response.order_id);
            // order 7, which filled, seems to be ignored...
            // wait a minute, will this go out of scope. hopefully not
            u8 action = holder_client_on_snapshot(ho, client_id, context);

            // ok they read the response order id, if it's rejected free it as its useless
            if ((context->status >> REJECT_BIT) & 1) {
                fl_release(orders, response.order_id);
            }

            // or an order was filled, in which case we can also free it now that they've read it

            if ((status >> FILL_BIT) & 1){
                if ((status >> PARTIAL_FILL_BIT) & 1) {
                } else {
                    fl_release(orders, response.order_id);
                }
            }

            if (action == 0) {
                //printf("reeasing order id\n");
                fl_release(orders, new_order_id);
            } else {
                empty->client_id = client_id;
                //empty->flags = 0;
                u64 order_event = ((CLIENT_OUT_TYPE & T_MASK) << PARAM_BITS) | (new_order_id & PARAM_MASK);
                u64 delay = client_settings[client_id].processing_time;

                sch_schedule(sch, order_event, delay);
            }


        } else if (type == CLIENT_OUT_TYPE) {
            //printf("client out type\n");

            u32 order_id = params;
            Order order = *(Order*)fl_get(orders, order_id);

            u64 out_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (order_id & PARAM_MASK);
            sch_schedule(sch, out_event, calculate_jitter(client_settings + (order.client_id), sc->rand));
        } else if (type == CONTROL_TYPE) {
            //printf("control type\n");
            // it will go here
            // and this will look a bit like the server event type

            // we should probalby actually you know wake up the client
            u32 control_id = params;
            if (control_id < MIN_CONTROL_PARAM) {
                u32 client_id = params;

                // idk I feel like this makes more sense as a response type

                Response r = {.client_id = client_id, .status = 1 << PING_BIT};
                u32 response_id = fl_insert(responses, &r);
                u64 response_event = build_event(CLIENT_IN_TYPE, response_id);
                // really not sure about the delay here tbh
                sch_schedule(sch, response_event, calculate_jitter(client_settings + (client_id), sc->rand));


                // bump client, not necessarily for first tim
                // for now we'll just have them connect with a socket order
                // like we were doin earlier
                // obviously not all clieints will want to do WS connection

                //printf("boot event\n");
                // for now, le't sjust say we wena tto connect to the websocket

                //u64 delay = 300000000;//cz_postboot_socket(0);
                //Order p = { .flags = 1 << PING_BIT, .client_id = client_id };
                //u32 order_id = fl_insert(orders,&p);
                //u64 socket_event = ((CLIENT_OUT_TYPE & T_MASK) << PARAM_BITS) | (order_id & PARAM_MASK);

                //sch_schedule(sch, socket_event, delay);

            } else if (control_id == CONTROL_PARAM_SLOW) {
                // ignore, mostly handled by sch.c
            } else if (control_id == CONTROL_PARAM_EOD) {
                // charge interest on borrowed shorts
            } else if (control_id == CONTROL_PARAM_EOM) {
                // charge clients subscription costs

            } else if (control_id == CONTROL_PARAM_KILL) {
                printf("kill event triggered\n");
                // gg 
                break;
            }
            //printf("control type done\n");
        } 

    }

    for(u32 i = 0; i < ho->num_clients; i++) {
        u32 agro = i;
        ClientSettings* cs = sc->client_settings + agro;
        printf("from client id #%u [$%u/$%u/%ush/%ush]\n", agro, cs->cash, cs->reserved_cash, cs->shares, cs->reserved_shares);
    }

    mbo_dump(bs_get_no_ref(sc->mbo_bs, sc->last_mbo));

    server_free(sc);

    return 0;
}
