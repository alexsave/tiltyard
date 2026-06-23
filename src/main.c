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

#include "ob.h"

void log_full(uint64_t raw) {
    printf("raw: %llu, priority %llu, type %llu, params %llu\n", 
            raw, 
            raw >> E_BITS, 
            (raw >> (E_BITS-T_BITS)) & T_MASK, 
            raw & PARAM_MASK);
}


/*void transfer(Order* resting_order, Order* new_order, ClientSettings* client_settings){
    // ok transfer $$$ 
    u32 cost = resting_order->price * resting_order->quantity;

    u32 taker = new_order->client_id;
    u32 maker = resting_order->client_id;

    // finally some accountability
    if ((new_order->flags >> BUY_DIRECTION_BIT) & 1){ 
        //client_settings[taker].buying_power -= cost;
        client_settings[maker].buying_power += cost;
        //client_settings[taker].shares += resting_order->quantity;
        client_settings[maker].shares -= resting_order->quantity;
    } else {
        //client_settings[taker].buying_power += cost;
        client_settings[maker].buying_power -= cost;
        //client_settings[taker].shares -= resting_order->quantity;
        client_settings[maker].shares += resting_order->quantity;
    }   
}*/

int main(int argc, char* argv[]){

    // tf do we even pass in here?


    // of course we have servercontext and context
    
    TypeMetadata* tm = get_types();

    u32 * client_allocations = malloc(tm->IMPLS_COUNT * sizeof(u32*));

    // now we can do
    // this shoudl definitely be done by main
    client_allocations[tm->cz_index] = 0;
    client_allocations[tm->co_index] = 1;

    ServerContext* sc = server_init(tm, client_allocations, 603);

    // genuinely needed everywhere
    SCH* sch = sc->sch;

    Holder* ho = sc->ho;
      
    ClientSettings* client_settings = sc->client_settings;
//calloc(ho->num_clients, sizeof(ClientSettings));
    // just a little treat to get them started
    client_settings[0].is_cash_account = 1;
    client_settings[0].cash = 100000;
    client_settings[0].reserved_cash = 100000;
    client_settings[0].buying_power = 100000;
    client_settings[0].shares = 100;
    client_settings[0].reserved_shares = 0;
    // ideally we also get this from clients

    FL* orders = sc->orders;
    FL* responses = sc->responses;

    BS* mbo_bs = sc->mbo_bs;

    // shoudl be pretty easy to veirfy

    // maybe get all client init config here?
    u64* inits = holder_get_init_ns(sc->ho);
    //printf("got init times\n");

    for(u32 i = 0; i < ho->num_clients; i++) {
        uint64_t client_id = i; 
        uint64_t first_boot = inits[i];

        uint64_t boot_event = ((CONTROL_TYPE & T_MASK) << PARAM_BITS) | (client_id & PARAM_MASK);
        
        sch_schedule(sch, boot_event, first_boot);
    }

    free(inits);
    //printf("done freeing\n");

    // when we have stop orders that are hit and convert to market orders
    // we cannot put them immediately into the sw queue

    // Initialization

    uint64_t repeat_event = CONTROL_TYPE << (PARAM_BITS) | CONTROL_PARAM_SLOW;
    // is 0 safe to schedule into?
    // maybe
    sch_schedule(sch, repeat_event, 0);


    u64 kill_event = CONTROL_TYPE << (PARAM_BITS) | CONTROL_PARAM_KILL;
    // one week
    sch_schedule(sch, kill_event, 2*(24*60*60) *S_TO_NS);

    Context* context = malloc(sizeof(Context));

    while(1){
        rand_next(sc->rand);
        context->random = (*(sc->rand)) & MAX_U32;

        uint64_t next = sch_pop(sch);

        uint64_t now_ns = sch_now_ns(sch);
        //printf("NOW %llu ~%llus \n", now_ns, now_ns/1000000000);
        //printf("NOW %llu ~%llus - ", now_ns, now_ns/1000000000);
        //log_full(next);

        uint8_t type = (next >> PARAM_BITS) & T_MASK;

        uint64_t params = next & PARAM_MASK;



        // different from waht is below
        if (type == SERVER_TYPE) {
            // something in the server
            //printf("server_type");

            u64 order_id = params;


            // order arrives to server
            if (order_id < MIN_RESERVED_PACKET) {
                server_arrival(sc, order_id);
            } else if (order_id == HW_TO_SW_ID) {
                server_hw_to_sw(sc);
            } else if (order_id == EXEC_START_ID) {
                server_exec_start(sc);
            } else if (order_id == EXEC_END_ID) {
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

            context->order_ptr = empty;
            context->mbo_snapshot = mbo_raw;

            // wait a minute, will this go out of scope. hopefully not
            u8 action = holder_client_on_snapshot(ho, client_id, context);

            if (action == 0) {
                //printf("reeasing order id\n");
                fl_release(orders, order_id);
            } else {
                empty->client_id = client_id;
                u64 order_event = ((CLIENT_OUT_TYPE & T_MASK) << PARAM_BITS) | (order_id & PARAM_MASK);
                u64 delay = 300000000;
                //printf("scheduling\n");
                sch_schedule(sch, order_event, delay);
                //printf("scheduled\n");
            }




            //printf("client in type done\n");

        } else if (type == CLIENT_OUT_TYPE) {
            //printf("client out type\n");

            u32 order_id = params;
            //Order order = *(Order*)fl_get(orders, order_id);

            // roughtly along these lines, need better solution

            u64 base_jitter = 200000000;//cz_base_latency(0);
            u64 random_jitter = 
                (base_jitter) + // 1.0x
                (base_jitter >> 7) + // + ~.01x
                (((base_jitter * (*(sc->rand) & MAX_U32)) >> 32 ) >> 5); // + 0x~.03x



            u64 out_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (order_id & PARAM_MASK);
            //printf("scheduling %llu\n", out_event);

            sch_schedule(sch, out_event, random_jitter);
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
                Order p = {
                    .flags = 1 << WS_BIT, 
                    .client_id = client_id };
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

    printf("client 0 $%u and shares %u\n", client_settings[0].buying_power, client_settings[0].shares);

    mbo_dump(bs_get_no_ref(sc->mbo_bs, sc->last_mbo));

    server_free(sc);

    return 0;
}
