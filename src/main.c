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

    // tf do we even pass in here?


    // of course we have servercontext and context
    
    TypeMetadata* tm = get_types();

    u32 * client_allocations = malloc(tm->IMPLS_COUNT * sizeof(u32*));

    // now we can do
    // this shoudl definitely be done by main
    client_allocations[tm->cz_index] = 2;
    client_allocations[tm->co_index] = 0;

    ServerContext* sc = server_init(tm, client_allocations, 603603);

    // genuinely needed everywhere
    SCH* sch = sc->sch;

    Holder* ho = sc->ho;
      
    ClientSettings* client_settings = sc->client_settings;

    FL* orders = sc->orders;
    FL* responses = sc->responses;

    BS* mbo_bs = sc->mbo_bs;

    // shoudl be pretty easy to veirfy

    for(u32 i = 0; i < ho->num_clients; i++) {
        uint64_t client_id = i; 
        uint64_t boot_event = ((CONTROL_TYPE & T_MASK) << PARAM_BITS) | (client_id & PARAM_MASK);
        sch_schedule(sch, boot_event, client_settings[i].initial_wake);
        //printf("schedling %u for %llu\n", i, client_settings[i].initial_wake);
    }

    // when we have stop orders that are hit and convert to market orders
    // we cannot put them immediately into the sw queue

    // Initialization

    uint64_t repeat_event = CONTROL_TYPE << (PARAM_BITS) | CONTROL_PARAM_SLOW;
    // is 0 safe to schedule into?
    // maybe
    sch_schedule(sch, repeat_event, 0);

    u64 kill_event = CONTROL_TYPE << (PARAM_BITS) | CONTROL_PARAM_KILL;
    // one week
    sch_schedule(sch, kill_event, 365*(24*60*60) *S_TO_NS);

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

            //print

            // client actually needs to read the response
            // params provides response id
            u32 response_id = params;

            //printf("releaseing response \n");
            Response response = *(Response*)fl_release(responses, response_id);


            u32 client_id = response.client_id;
            u32 snapshot_id = response.snapshot_id;
            u8 status = response.status;

            //printf("client_id %u snapshot id %u status %u\n", client_id, snapshot_id, status);

            // otherwise we actually look at the snapshot and do stuff with it

            // idk should we bump refs on error then send it anyways?
            //void* mbo_raw
            //if (status != 1){

            //printf("getting mbo raw\n");
            void* mbo_raw = bs_get(mbo_bs, snapshot_id);
            //printf("got mbo raw\n");
            //}

            Order tmp = {};

            // reserved client space for order
            // just need something to copy from
            //printf("inserting order to fl\n");
            u32 order_id = fl_insert(orders, &tmp);
            //printf("getting order from fl\n");
            Order* empty = fl_get(orders, order_id);
            //printf("got order from fl\n");

            context->next_order_ptr = empty;
            context->mbo_snapshot = mbo_raw;
            context->status = status;
            context->order_id = response.order_id;

            // wait a minute, will this go out of scope. hopefully not
            u8 action = holder_client_on_snapshot(ho, client_id, context);

            if (action == 0) {
                //printf("reeasing order id\n");
                fl_release(orders, order_id);
            } else {
                empty->client_id = client_id;
                u64 order_event = ((CLIENT_OUT_TYPE & T_MASK) << PARAM_BITS) | (order_id & PARAM_MASK);
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
                // bump client, not necessarily for first tim
                // for now we'll just have them connect with a socket order
                // like we were doin earlier
                // obviously not all clieints will want to do WS connection

                //printf("boot event\n");
                // for now, le't sjust say we wena tto connect to the websocket

                u64 delay = 300000000;//cz_postboot_socket(0);
                Order p = { .flags = 1 << WS_BIT, .client_id = client_id };
                u32 order_id = fl_insert(orders,&p);
                u64 socket_event = ((CLIENT_OUT_TYPE & T_MASK) << PARAM_BITS) | (order_id & PARAM_MASK);

                sch_schedule(sch, socket_event, delay);

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
