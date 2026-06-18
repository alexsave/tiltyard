#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "rand.h"
#include "sch.h"
#include "fl.h"
#include "cb.h"
#include "types.h"
#include "constants.h"

#include "client.h"
#include "holder.h"
#include "order.h"

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

// subtypes for server in types

// these two are for clients
// and they do limit us to only 2^30 snapshots but maybe that's ok
// none of this shit
//static const u8 SNAPSHOT_BOOT_BIT = 31;

static const u8 SNAPSHOT_SOCKET_BIT = 7;

//|= 1 << SNAPSHOT_BOOT_BIT;

typedef struct Response {
    // client id may actually be 23 bits, so maybe we put flags into the top bits
    u32 client_id;
    // top bits of snapshot_id will actually have some interesting things in them
    // hopefully 1 millino snapshots is enough

    u32 snapshot_id;// also boot or socket
} Response;

// $$, initial wake, processing time, latency
typedef struct ClientNums {
    
} ClientNums;

int main(int argc, char* argv[]){
    uint64_t seed = 603603603;
    uint64_t* rand = rand_init(seed);
    rand_next(rand);
    SCH* sch = sch_init(rand);

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

    // let's fucking roll


    // next step is to get all the awake times
    // and yes this will immediately schedule like 10M events
    // each of which needs... a packet id
    // oh 
    // maybe it's not a good idea to shove wakup into a special packet type

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

    FL* orders = fl_init(sizeof(Order), MIN_RESERVED_PACKET);


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
    FL* responses = fl_init(sizeof(Response), 0);


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

            u64 packet_id = params;


            // packet arrives to server
            if (packet_id < MIN_RESERVED_PACKET) {
                cb_queue(hw_queue, packet_id);

                u64 HW_TO_SW_DELAY = 10000;
                u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (HW_TO_SW_ID & PARAM_MASK);
                sch_schedule(sch, socket_event, HW_TO_SW_DELAY);
            } else if (packet_id == HW_TO_SW_ID) {
                if (cb_is_empty(hw_queue)) {
                    //weird
                    continue;
                }
                u32 moving_packet = cb_deque(hw_queue); // handle zero case

                // If it's not empty, someone has already scheduled an exec_start_id
                if (cb_is_empty(sw_queue)) {
                    u64 SW_TO_EXEC_DELAY = 100;
                    u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_START_ID & PARAM_MASK);
                    sch_schedule(sch, socket_event, SW_TO_EXEC_DELAY);
                }

                cb_queue(sw_queue, moving_packet);


            } else if (packet_id == EXEC_START_ID) {
                // This relies on the server->executing state and 
                // the fact that an EXEC_END_ID is scheduled are in sync

                // cb is empty and executing - on the last packet 
                // cb is empty and not executing - do nothing
                // cb not empty and executing - don't do anything
                // cb not empty and not executing - !!!

                if (server->executing || cb_is_empty(sw_queue)) {
                    // do nothing
                    continue;
                }

                server->executing = 1;

                u64 EXEC_TIME = 10;
                u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_END_ID & PARAM_MASK);
                sch_schedule(sch, socket_event, EXEC_TIME);
            } else if (packet_id == EXEC_END_ID) {
                if (cb_is_empty(sw_queue)) {
                    //really weird
                    server->executing = 0;
                    continue;
                }

                u32 exec_packet_id = cb_deque(sw_queue);

                //Order out = *(Order*)(fl_release(orders,params));
                //printf("server got a packet!!! %d, %d \n", out.type, out.client_id);
                // TODO actually modify the order book based on orders[exec_packet_id]

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




            } else if (packet_id == EXEC_TO_SW_ID) {
                u32 synth_packet_id = cb_deque(convert_holder);

                if (cb_is_empty(convert_holder) || synth_packet_id == CONVERT_SENTINEL_VALUE) {
                    continue;
                }

                if (cb_is_empty(sw_queue)) {
                   u64 SW_TO_EXEC_DELAY = 100;
                   u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_START_ID & PARAM_MASK);
                   sch_schedule(sch, socket_event, SW_TO_EXEC_DELAY);
                }

                // this will pop the last CONVERT_SENTINEL_VALUE becuase we deque before we check
                while(!cb_is_empty(convert_holder) && synth_packet_id != CONVERT_SENTINEL_VALUE) {
                    cb_queue(sw_queue, synth_packet_id);
                    synth_packet_id = cb_deque(convert_holder);
                }       

            }
        } else if (type == CLIENT_IN_TYPE) {
            printf("client in type\n");

            //print

            // client actually needs to read the response
            // params provides response id
            u32 response_id = params;

            Response response = *(Response*)fl_release(responses, response_id);
            
            u32 client_id = response.client_id;
            u32 snapshot_id = response.snapshot_id;

            // special checks for snapshot id


            // otherwise we actually look at the snapshot and do stuff with it
            


        } else if (type == CLIENT_OUT_TYPE) {

            u32 packet_id = params;
            Order packet = *(Order*)fl_get(orders, packet_id);

            // roughtly along these lines, need better solution

            u64 base_jitter = 42;//cz_base_latency(0);
            u64 random_jitter = 
                (base_jitter) + // 1.0x
                (base_jitter >> 7) + // + ~.01x
                (((base_jitter * (*rand & MAX_U32)) >> 32 ) >> 5); // + 0x~.03x



            printf("client out type\n");

            u64 out_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (packet_id & PARAM_MASK);

            sch_schedule(sch, out_event, random_jitter);
        } else if (type == CONTROL_TYPE) {
            printf("control type\n");
            // it will go here
            // and this will look a bit like the server event type

            // we should probalby actually you know wake up the client
            u32 control_id = params;
            if (control_id < MIN_CONTROL_PARAM) {
                u32 client_id = params;
                // bump client, not necessarily for first tim
                // for now we'll just have them connect with a socket packet
                // like we were doin earlier
                // obviously not all clieints will want to do WS connection

                printf("boot event\n");
                // for now, le't sjust say we wena tto connect to the websocket

                u64 delay = 300000000;//cz_postboot_socket(0);
                printf("%llu\n", delay);
                Order p = {
                    .flags = 1 << WS_BIT, 
                    .client_id = client_id };
                u32 packet_id = fl_insert(orders,&p);
                printf("packet id, %u\n", packet_id);
                u64 socket_event = ((CLIENT_OUT_TYPE & T_MASK) << PARAM_BITS) | (packet_id & PARAM_MASK);

                sch_schedule(sch, socket_event, delay);

            } else if (control_id == CONTROL_PARAM_SLOW) {
                // ignore, mostly handled by sch.c
            } else if (control_id == CONTROL_PARAM_EOD) {
                // charge interest on borrowed shorts
            } else if (control_id == CONTROL_PARAM_EOM) {
                // charge clients subscription costs

            } else if (control_id == CONTROL_PARAM_KILL) {
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

    return 0;
}
