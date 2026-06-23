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

// $$, initial wake, processing time, latency
typedef struct ClientSettings {
    u8 ws;
    u32 buying_power;
    u32 shares;
} ClientSettings;

void transfer(Order* resting_order, Order* new_order, ClientSettings* client_settings){
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
    u8 status;
} Response;



int main(int argc, char* argv[]){
    uint64_t seed = 603603;
    uint64_t* rand = rand_init(seed);
    rand_next(rand);

    Context* context = malloc(sizeof(Context));
    context->random = (*rand) & MAX_U32;

    SCH* sch = sch_init(rand);

    FL* orders = fl_init(sizeof(Order), MIN_RESERVED_PACKET);

    TypeMetadata* tm = get_types();
    // Properly make sure we are using all the implementations

    // now here is the basic flow of things

    u32 * client_allocations = malloc(tm->IMPLS_COUNT * sizeof(u32*));

    // now we can do
    // this shoudl definitely be done by main
    client_allocations[tm->cz_index] = 0;
    client_allocations[tm->co_index] = 1;

    Holder* ho = holder_init(tm, client_allocations);
      
    ClientSettings* client_settings = calloc(ho->num_clients, sizeof(ClientSettings));
    // just a little treat to get them started
    client_settings[0].buying_power = 100000;
    client_settings[0].shares = 100;
    // ideally we also get this from clients


    // to keep track of fills, and the case where an incoming fill only partially fills existing order
    u32 partial_fill_id = MAX_U32;
    u32 partial_fill_q = 0;
    CB* fills = cb_init();

    void* mbo_address = 0;
    BS* mbo_bs = bs_init(8192);

    // 2 is correct here
    // server will read it to generate the second MBO, and this will be sent to first client that connects
    // but that's it
    // so complex
    // should only be freed after the next one is created, thus NOT messing up BS
    u32 last_mbo = bs_reserve(mbo_bs, sizeof(MBO), 1, &mbo_address);
    ((MBO*)mbo_address)->level_count = 0;
    ((MBO*)mbo_address)->hi_bid_index = MAX_U16;


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
    // maybe get all client init config here?
    u64* inits = holder_get_init_ns(ho);

    for(u32 i = 0; i < ho->num_clients; i++) {
        uint64_t client_id = i; 
        uint64_t first_boot = inits[i];

        uint64_t boot_event = ((CONTROL_TYPE & T_MASK) << PARAM_BITS) | (client_id & PARAM_MASK);
        sch_schedule(sch, boot_event, first_boot);
    }

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
    sch_schedule(sch, kill_event, 30*(24*60*60) *S_TO_NS);

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

    //for(u32 k = 0; k < 200; k++){
    while(1){
        rand_next(rand);
        context->random = (*rand) & MAX_U32;

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

            u64 order_id = params;


            // order arrives to server
            if (order_id < MIN_RESERVED_PACKET) {
                //printf("order %llu arrives at server\n", order_id);
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
                //printf("hw to sw move requested for order %u\n", moving_order);

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

                //printf("exec started\n");

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
                //printf("exec finished on order %u\n", exec_order_id);

                Order* in = (Order*)fl_get(orders, exec_order_id);

                u32 agro = in->client_id;

                printf("argro is %u btw\n" , agro);

                u8 will_modify_ob = 0;

                u32 before_quantity = in->quantity;
                u32 in_cost = before_quantity * in->price;
                u8 is_buy = (in->flags >> BUY_DIRECTION_BIT) & 1;

                u8 has_bp = client_settings[agro].buying_power >= in_cost;
                u8 has_shares = client_settings[agro].shares >= before_quantity;
                u8 is_valid_quantity = in->quantity > 0;
                u8 is_valid_price = in->price > 0;
                u8 is_toggle_ws = (in->flags & (1 << WS_BIT));

                
                

                // lets thingk
                /*
                   000 what the fuck is this guy even doing - bump refs and return current
                   001 fair enough, he just want to connect - bump refs and connect and return current
                   010 broke boy reject this - bump refs and return current
                   011 broke boy disconnect
                   100 invalid reqest
                   101 invalid reqest and toggle
                   110 ok let him trade
                   111 trade and disconnect?

                 */

                // as all possibilities need some response, everything except valid trade will need to bump refs

                u8 will_modify;
                if(is_buy)
                    will_modify = has_bp & is_valid_quantity & is_valid_price;
                else 
                    will_modify = has_shares & is_valid_quantity & is_valid_price;

                if (!will_modify)
                    bs_bump_refs(mbo_bs, last_mbo);

                // for now we'll just handle socket connections
                if (is_toggle_ws) 
                    client_settings[agro].ws = !(client_settings[agro].ws);

                printf("order info id %u buy? %u quantity %u price %u from client id #%u\n", exec_order_id, (in->flags >> BUY_DIRECTION_BIT)&1, in->quantity, in->price, agro);
                if(is_buy && !has_bp) 
                    printf("unfortunately hes a broke boy, rejected $%u > $%u\n", in_cost, client_settings[agro].buying_power);

                if(!is_buy && !has_shares) 
                    printf("unfortunately he does not have enough shares, rejected %u > %u\n", before_quantity, client_settings[agro].shares);

                //printf("order info id %u buy? %u quantity %u price %u from client id #%u\n", exec_order_id, (in->flags >> BUY_DIRECTION_BIT)&1, in->quantity, in->price, agro);


                void* mbo = bs_get_no_ref(mbo_bs, last_mbo);

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

                    u32 prev_last_mbo = last_mbo;
                    //if(exec_order_id > 2000000){
                    //mbo_dump(mbo);
                    //exit(1);
                    //}



                    // ok hold on

                    partial_fill_id = MAX_U32;

                    last_mbo = ob_limit(exec_order_id, orders, last_mbo, mbo_bs, ref_count, fills, &partial_fill_id, &partial_fill_q);

                    // check exec_order_id to see if we had a partial fill
                    if (is_buy) {
                        client_settings[agro].buying_power -= in->quantity * in->price;
                    } else {
                        client_settings[agro].shares -= in->quantity;
                    }

                    //if (in->quantity < before_quantity) {

                        // this is actually how much buying power to reduce by, as it's left on the board
                    //}


                    
                    
                    /*if (is_buy) {
                        if ((!cb_is_empty(fills)) || (partial_fill_id != MAX_U32)){
                            // instant fill, need to handle buying power separately
                        } else {
                            client_settings[agro].buying_power -= in_cost;
                        }
                    } else {
                        // long sales only for now
                        client_settings[agro].shares -= before_quantity;
                        //client_settings[taker].buying_power -= cost;
                    }*/

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

                        if (is_buy){
                            // if we're buying then the 
                            client_settings[taker].buying_power -= cost;
                            client_settings[maker].buying_power += cost;
                            client_settings[taker].shares += order->quantity;
                        } else {
                            client_settings[taker].shares -= order->quantity;
                            client_settings[maker].shares += order->quantity;
                            client_settings[taker].buying_power += cost;
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

                        // finally some accountability
                        if (is_buy){
                            client_settings[taker].buying_power -= cost;
                            client_settings[maker].buying_power += cost;
                            client_settings[taker].shares += partial_fill_q;
                        } else {
                            client_settings[taker].shares -= partial_fill_q;
                            client_settings[maker].shares += partial_fill_q;
                            client_settings[taker].buying_power += cost;
                        }

                    }

                    //

                    bs_get(mbo_bs, prev_last_mbo);
                    mbo_dump(bs_get_no_ref(mbo_bs, last_mbo));

                    //bit hacky but ensures we can get the first snpashot into processing
                    //u8 status = ob_limit(in->flags & (1 << BUY_DIRECTION_BIT), in->quantity, in, mbo_address, mbp_address, mbo_bs, mbp_bs);
                }

                if (!will_modify){
                    // only send reject to that one client, later
                    Response r = {.client_id = in->client_id, .snapshot_id = last_mbo, .status=1};
                    u32 response_id = fl_insert(responses, &r);
                    u64 response_event = ((CLIENT_IN_TYPE & T_MASK) << PARAM_BITS) | (response_id & PARAM_MASK);
                    sch_schedule(sch, response_event, 100000000); 

                } else {
                    // accepted orders will modify this
                    //u32 server_snapshot = handle;

                    // i know its ugly
                    for (u32 ci = 0; ci < ho->num_clients; ci++){
                        if(client_settings[ci].ws) {
                            //printf("making response for %u\n", ci);
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
                (((base_jitter * (*rand & MAX_U32)) >> 32 ) >> 5); // + 0x~.03x



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

                printf("boot event\n");
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
        } 

    }

    printf("client 0 $%u and shares %u\n", client_settings[0].buying_power, client_settings[0].shares);

    mbo_dump(bs_get_no_ref(mbo_bs, last_mbo));

    sch_free(sch);
    rand_free(rand);
    free(server);
    tm_free(tm);

    cb_free(hw_queue);
    cb_free(sw_queue);

    holder_free(ho);


    return 0;
}
