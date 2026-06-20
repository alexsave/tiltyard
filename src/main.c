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

#include "client.h"
#include "holder.h"
#include "order.h"

#include "ob.h"

typedef struct Server {
    u8 executing;
} Server;

void log_full(uint64_t raw) {
    printf("raw: %llu, priority %llu, type %llu, params %llu\n", 
            raw, 
            raw >> E_BITS, 
            (raw >> (E_BITS-T_BITS)) & T_MASK, 
            raw & PARAM_MASK);
}

// the ordering of these actually matters for tie breakers
static const u32 HW_TO_SW_ID = MAX_PARAM - 0;
static const u32 EXEC_START_ID = MAX_PARAM - 1;
static const u32 EXEC_END_ID = MAX_PARAM - 2;
static const u32 EXEC_TO_SW_ID = MAX_PARAM - 3;
static const u32 MIN_RESERVED_PACKET = EXEC_TO_SW_ID;

// this only works because HW_TO_SW_ID = 0 and is reserved
// come back to this lol
static const u32 CONVERT_SENTINEL_VALUE = 0;

typedef struct Response {
    // client id may actually be 23 bits, so maybe we put flags into the top bits
    u32 client_id;
    // top bits of snapshot_id will actually have some interesting things in them
    // hopefully 1 millino snapshots is enough

    u32 snapshot_id;// also boot or socket
} Response;

// $$, initial wake, processing time, latency
typedef struct ClientSettings {
    u8 ws;
} ClientSettings;



int main(int argc, char* argv[]){
    uint64_t seed = 603603603;
    uint64_t* rand = rand_init(seed);
    rand_next(rand);

    Context* context = malloc(sizeof(Context));
    context->random = (*rand) & MAX_U32;

    SCH* sch = sch_init(rand);

    printf("size mbp %lu\n", sizeof(MBP));

    FL* orders = fl_init(sizeof(Order), MIN_RESERVED_PACKET);

    TypeMetadata* tm = get_types();
    // Properly make sure we are using all the implementations

    // now here is the basic flow of things

    u32 * client_allocations = malloc(tm->IMPLS_COUNT * sizeof(u32*));

    printf("%d\n", tm->co_index);
    // now we can do
    // this shoudl definitely be done by main
    client_allocations[tm->cz_index] = 1;
    client_allocations[tm->co_index] = 1;

    printf("initialzing holder\n");
    Holder* ho = holder_init(tm, client_allocations);
    printf("done initialzing holder\n");

    ClientSettings* client_settings = calloc(ho->num_clients, sizeof(ClientSettings));

    // let's fucking roll

    // initial order book - two opposing orders across $100

    // two fake orders just to make thigns fun
    Order p = {
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9900,
        .client_id = 0 };
    u32 order_id99 = fl_insert(orders,&p);

    Order p2 = {
        .flags = 1 << IS_LIMIT_BIT, 
        .quantity = 1,
        .price = 10100,
        .client_id = 1 };
    u32 order_id101 = fl_insert(orders,&p2);

    // FINALLY bs comes in 


    BS* ob_snapshots = bs_init(1000);
    void* bs_address = 0;
    // 1024 is overkill but hold on
    u32 handle = bs_reserve(ob_snapshots, 1024, 1, &bs_address);

    /*OrderBookMetadata* obm = (OrderBookMetadata*)bs_address;
    // $101, $99
    obm->lowest_ask = 10100;
    obm->highest_bid = 9900;
    obm->row_count = 2;

    bs_address += sizeof(OrderBookMetadata);

    for (u8 row = 0; row < obm->row_count; row++) {
        //make it explicit

        OrderBookRowMetadata* obrm = (OrderBookRowMetadata*)bs_address;
        if (row == 0)
            obrm->price = 9900;
        else
            obrm->price = 10100;

        obrm->order_count = 1;

        // interestingly my notes say to limit it to 10 quantity bits, combined with 22bit order id yeilds 32 bits
        // yeah probably a good idea considering this will be a massive data thing

        bs_address += sizeof(OrderBookRowMetadata);

        for (u8 oi = 0; oi < obrm->order_count; oi++) {
            // the fact that it's here tells us its limit
            // adn the side tell us buy/sell
            // and the price
            // we just need quantity?
            // which is stored in the order anyways...?
            // well it's goig to be 32 bits anyways

            OrderInBook* oib = (OrderInBook*)bs_address;
            // ah this is so dumb
            if (row == 0)
                oib->order_id = order_id99;
            else 
                oib->order_id = order_id101;

            bs_address += sizeof(OrderInBook);
        }
    }*/

    void* mbo_address = 0;
    BS* mbo_bs = bs_init(1000);
    u32 mbo_handle = bs_reserve(mbo_bs, sizeof(MBO), 1, &mbo_address);
    u32 last_mbo;
    ((MBO*)mbo_address)->level_count = 0;
    last_mbo = mbo_handle;

    void* mbp_address = 0;
    BS* mbp_bs = bs_init(10000);
    u32 mbp_handle = bs_reserve(mbp_bs, sizeof(MBP), 1, &mbp_address);
    ((MBP*)mbp_address)->level_count = 0;
    


    // this is just one row btw


    // this is how we write to order book snapshot






    // next step is to get all the awake times
    // and yes this will immediately schedule like 10M events
    // each of which needs... a order id
    // oh 
    // maybe it's not a good idea to shove wakup into a special order type

    // fuck it
    // we'll figure out later how to actually schedul eit
    // mabye add a new boot type, like we preivously had, just to avoid the response redirection
    // and shove the slow checker into the server type too

    // shoudl be pretty easy to veirfy

    printf("getting in it times\n");
    u64* inits = holder_get_init_ns(ho);

    for(u32 i = 0; i < ho->num_clients; i++) {
        uint64_t client_id = i; 
        uint64_t first_boot = inits[i];

        uint64_t boot_event = ((CONTROL_TYPE & T_MASK) << PARAM_BITS) | (client_id & PARAM_MASK);
        sch_schedule(sch, boot_event, first_boot);
        printf("booting %u at %llu \n", i, inits[i]);
    }

    // and this is just "initial boot" - we don't need "inits" anymore now that it's in the scheudler
    free(inits);



    Server* server = malloc(sizeof(Server));
    server->executing = 0;



    CB* sw_queue = cb_init();
    CB* hw_queue = cb_init();
    // when we have stop orders that are hit and convert to market orders
    // we cannot put them immediately into the sw queue
    CB* convert_holder = cb_init();



    // Initialization

    uint64_t repeat_event = CONTROL_TYPE << (PARAM_BITS) | CONTROL_PARAM_SLOW;
    // is 0 safe to schedule into?
    // maybe
    sch_schedule(sch, repeat_event, 0);


    u64 kill_event = CONTROL_TYPE << (PARAM_BITS) | CONTROL_PARAM_KILL;
    // one week
    sch_schedule(sch, kill_event, (u64)(3)*24*60*60*S_TO_NS);

    // no reservations
    FL* responses = fl_init(sizeof(Response), MAX_U32);


    //more interesting


    // we need to do that for ALL 10M clients
    // but we only need to do it once
    // but here's an easy way to get it

    // Finish initialization section

    // ok so this is where we run the market simulator

    // where to even begin...
    // notes todo say "matching code"
    // yes

    // idk but this what the main loop will look a bit like

    while(1) {
        rand_next(rand);
        context->random = (*rand) & MAX_U32;

        uint64_t next = sch_pop(sch);

        uint64_t now_ns = sch_now_ns(sch);
        printf("Now %llu ~%llus - ", now_ns, now_ns/1000000000);
        log_full(next);

        uint8_t type = (next >> PARAM_BITS) & T_MASK;

        uint64_t params = next & PARAM_MASK;

        uint32_t start_server_exec = 2;


        // different from waht is below
        if (type == SERVER_TYPE) {
            // something in the server

            u64 order_id = params;


            // order arrives to server
            if (order_id < MIN_RESERVED_PACKET) {
                printf("order %llu arrives at server\n", order_id);
                cb_queue(hw_queue, order_id);

                u64 HW_TO_SW_DELAY = 10000;
                u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (HW_TO_SW_ID & PARAM_MASK);
                sch_schedule(sch, socket_event, HW_TO_SW_DELAY);
            } else if (order_id == HW_TO_SW_ID) {
                if (cb_is_empty(hw_queue)) {
                    //weird
                    continue;
                }
                u32 moving_order = cb_deque(hw_queue); // handle zero case
                printf("hw to sw move requested for order %u\n", moving_order);

                // If it's not empty, someone has already scheduled an exec_start_id
                if (cb_is_empty(sw_queue)) {
                    u64 SW_TO_EXEC_DELAY = 100;
                    u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_START_ID & PARAM_MASK);
                    sch_schedule(sch, socket_event, SW_TO_EXEC_DELAY);
                }

                cb_queue(sw_queue, moving_order);


            } else if (order_id == EXEC_START_ID) {
                // This relies on the server->executing state and 
                // the fact that an EXEC_END_ID is scheduled are in sync

                // cb is empty and executing - on the last order 
                // cb is empty and not executing - do nothing
                // cb not empty and executing - don't do anything
                // cb not empty and not executing - do somethign

                if (server->executing || cb_is_empty(sw_queue)) {
                    // do nothing
                    continue;
                }

                printf("exec started\n");

                server->executing = 1;

                u64 EXEC_TIME = 10;
                u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_END_ID & PARAM_MASK);
                sch_schedule(sch, socket_event, EXEC_TIME);
            } else if (order_id == EXEC_END_ID) {
                if (cb_is_empty(sw_queue)) {
                    //really weird
                    server->executing = 0;
                    continue;
                }

                u32 exec_order_id = cb_deque(sw_queue);
                printf("exec finished on order %u\n", exec_order_id);

                Order* in = (Order*)fl_get(orders, exec_order_id);
                printf("order info %u %u %u %u\n", in->flags, in->quantity, in->price, in->client_id);

                // for now we'll just handle socket connections
                if (in->flags & (1 << WS_BIT)) {
                    //something like
                    // toggle connection
                    client_settings[in->client_id].ws = 
                        !(client_settings[in->client_id].ws);
                }  else {
                    last_mbo = ob_limit(exec_order_id, orders, last_mbo, mbo_bs);
                    //u8 status = ob_limit(in->flags & (1 << BUY_DIRECTION_BIT), in->quantity, in, mbo_address, mbp_address, mbo_bs, mbp_bs);
                }

                if (0/*status == REJECT*/) {
                    // only send reject to that one client, later

                } else {
                    // accepted orders will modify this
                    //u32 server_snapshot = handle;

                    // i know its ugly
                    for (u32 ci = 0; ci < ho->num_clients; ci++){
                        if(client_settings[ci].ws) {
                            printf("making response for %u\n", ci);
                            // make a new response for them
                            Response r = {.client_id = ci, .snapshot_id = last_mbo};
                            u32 response_id = fl_insert(responses, &r);
                            u64 response_event = ((CLIENT_IN_TYPE & T_MASK) << PARAM_BITS) | (response_id & PARAM_MASK);

                            //.. delay is tricky, we actually need to go get client values again
                            // probably using holder.
                            // but for now let's just say exactly 100ms lol
                            sch_schedule(sch, response_event, 100000000); 
                        }
                    }
                }



                //Order out = *(Order*)(fl_release(orders,params));
                //printf("server got a order!!! %d, %d \n", out.type, out.client_id);
                // TODO actually modify the order book based on orders[exec_order_id]

                // also schedule a bunch of CLIENT_IN events


                /*
                   if (need to convert stops)
                   schedule EXEC_TO_SW_ID eevent
                   u64 SW_TO_EXEC_DELAY = 100;
                   u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_TO_SW_ID & PARAM_MASK);
                   sch_schedule(sch, socket_event, SW_TO_EXEC_DELAY);
                   cb_queue(convert_holder);
                   cb_queue(CONVERT_SENTINEL_VALUE);

                 */

                if (cb_is_empty(sw_queue)){
                    server->executing = 0;
                } else {
                    u64 EXEC_TIME = 10;
                    u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_END_ID & PARAM_MASK);
                    sch_schedule(sch, socket_event, EXEC_TIME);
                }




            } else if (order_id == EXEC_TO_SW_ID) {
                u32 synth_order_id = cb_deque(convert_holder);

                if (cb_is_empty(convert_holder) || synth_order_id == CONVERT_SENTINEL_VALUE) {
                    continue;
                }

                if (cb_is_empty(sw_queue)) {
                    u64 SW_TO_EXEC_DELAY = 100;
                    u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_START_ID & PARAM_MASK);
                    sch_schedule(sch, socket_event, SW_TO_EXEC_DELAY);
                }

                // this will pop the last CONVERT_SENTINEL_VALUE becuase we deque before we check
                while(!cb_is_empty(convert_holder) && synth_order_id != CONVERT_SENTINEL_VALUE) {
                    cb_queue(sw_queue, synth_order_id);
                    synth_order_id = cb_deque(convert_holder);
                }       

            }
        } else if (type == CLIENT_IN_TYPE) {
            printf("client in type\n");

            //print

            // client actually needs to read the response
            // params provides response id
            u32 response_id = params;

            Response response = *(Response*)fl_release(responses, response_id);

            printf("response gotten\n");

            u32 client_id = response.client_id;
            u32 snapshot_id = response.snapshot_id;

            // otherwise we actually look at the snapshot and do stuff with it

            
            void* mbo_raw = bs_get(mbo_bs, mbo_handle);
            printf("mbo gotten\n");

            // reserved client space for order
            // just need something to copy from
            u32 order_id = fl_insert(orders, mbo_bs);
            Order* empty = fl_get(orders, order_id);

            context->order_ptr = empty;
            context->mbo_snapshot = mbo_raw;

            printf("emtpy reserved\n");
            
            // wait a minute, will this go out of scope. hopefully not
            Order* client_order = 0;//client#on_tick( mbo_raw);
            u8 action = holder_client_on_snapshot(ho, client_id, context);
            printf("client consulted, action %u\n", action);
            if (action == 0) {
                fl_release(orders, order_id);
            } else {
                empty->client_id = client_id;
                u64 order_event = ((CLIENT_OUT_TYPE & T_MASK) << PARAM_BITS) | (order_id & PARAM_MASK);
                u64 delay = 300000000;
                printf("about to schedule\n");
                sch_schedule(sch, order_event, delay);
                printf("scheduled\n");
            }


 
    

        } else if (type == CLIENT_OUT_TYPE) {

            u32 order_id = params;
            Order order = *(Order*)fl_get(orders, order_id);

            // roughtly along these lines, need better solution

            u64 base_jitter = 42;//cz_base_latency(0);
            u64 random_jitter = 
                (base_jitter) + // 1.0x
                (base_jitter >> 7) + // + ~.01x
                (((base_jitter * (*rand & MAX_U32)) >> 32 ) >> 5); // + 0x~.03x



            printf("client out type\n");

            u64 out_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (order_id & PARAM_MASK);

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

                printf("boot event\n");
                // for now, le't sjust say we wena tto connect to the websocket

                u64 delay = 300000000;//cz_postboot_socket(0);
                printf("%llu\n", delay);
                Order p = {
                    .flags = 1 << WS_BIT, 
                    .client_id = client_id };
                u32 order_id = fl_insert(orders,&p);
                printf("order id, %u\n", order_id);
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
        } 

    }

    sch_free(sch);
    rand_free(rand);
    free(server);
    tm_free(tm);

    cb_free(hw_queue);
    cb_free(sw_queue);

    holder_free(ho);

    bs_free(ob_snapshots);

    return 0;
}
