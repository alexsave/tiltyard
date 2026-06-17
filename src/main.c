#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "rand.h"
#include "sch.h"
#include "fl.h"
#include "cb.h"
#include "strategy/client_zero.h"
#include "strategy/client_one.h"
#include "types.h"
#include "constants.h"

#include "client.h"

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
static const u32 HW_TO_SW_ID = 0;
static const u32 EXEC_START_ID = 1;
static const u32 EXEC_END_ID = 2;
static const u32 EXEC_TO_SW_ID = 3;
static const u32 MAX_RESERVED_ID = EXEC_TO_SW_ID;

// this only works because HW_TO_SW_ID = 0 and is reserved
static const u32 CONVERT_SENTINEL_VALUE = 0;

// subtypes for server in types


// ok so this is going to have a BUNCH of stuff in it, everything we need to specify an order
// really these are client->server network events, but wel
// ah that's the term
// for now just type and clientid
typedef struct Packet {
    uint8_t type;
    uint32_t client_id;
} Packet;

// these two are for clients
// and they do limit us to only 2^30 snapshots but maybe that's ok
static const u8 SNAPSHOT_BOOT_BIT = 31;

static const u8 SNAPSHOT_SOCKET_BIT = 7;

//|= 1 << SNAPSHOT_BOOT_BIT;

typedef struct Response {
    // client id may actually be 23 bits, so maybe we put flags into the top bits
    u32 client_id;
    // top bits of snapshot_id will actually have some interesting things in them
    // hopefully 1 millino snapshots is enough

    u32 snapshot_id;// also boot or socket
} Response;


int main(int argc, char* argv[]){


    //init_methods = [&cz_init, &co_init];
//
    //for init in init_methods
        //clinets.push(init())

    // fun interface stuff
    //CZ* (*functionPtr)();
    //functionPtr = &cz_init;
    //CZ* cz = (*functionPtr)();
//
    //CO* (*functionPtr)();
    //functionPtr = &co_init;
    //CO* co = (*functionPtr)();
//



    // fun interface stuff

    



    CZ* client = cz_init();

    CO* client1 = co_init();

    Server* server = malloc(sizeof(Server));
    server->executing = 0;

    FL* packets = fl_init(sizeof(Packet), MAX_RESERVED_ID);

    uint64_t seed = 603603603;
    uint64_t* rand = rand_init(seed);
    rand_next(rand);

    CB* sw_queue = cb_init();
    CB* hw_queue = cb_init();
    // when we have stop orders that are hit and convert to market orders
    // we cannot put them immediately into the sw queue
    CB* convert_holder = cb_init();


    SCH* sch = sch_init(rand);

    // Initialization

    uint64_t repeat_event = SLOW_REPEAT_TYPE << (PARAM_BITS) | 0;
    // is 0 safe to schedule into?
    // maybe
    sch_schedule(sch, repeat_event, 0);


    // no reservations
    FL* responses = fl_init(sizeof(Response), 0);


    //more interesting
    uint64_t client_id = 123; // good case for freelist maybe

    uint64_t first_boot = cz_initial_boot_time(client);

    Response awake = { .client_id = client_id, .snapshot_id = 0 | (1 << SNAPSHOT_BOOT_BIT) };
    u32 response_id = fl_insert(responses, &awake);

    uint64_t boot_event = ((CLIENT_IN_TYPE & T_MASK) << PARAM_BITS) | (response_id & PARAM_MASK);

    // not all these operations are necessary but this is the template
    //uint64_t boot_event = ((BOOT_TYPE & T_MASK) << PARAM_BITS) | (client_id & PARAM_MASK);

    // actually schedul ea client IN event with a speciic values in the response 

    // type zero event
    sch_schedule(sch, boot_event, first_boot);

    // Finish initialization section

    // ok so this is where we run the market simulator

    // where to even begin...
    // notes todo say "matching code"
    // yes

    // idk but this what the main loop will look a bit like

    for(int i = 0; i < 30; i++){
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
            if (packet_id > MAX_RESERVED_ID) {
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

                //Packet out = *(Packet*)(fl_release(packets,params));
                //printf("server got a packet!!! %d, %d \n", out.type, out.client_id);
                // TODO actually modify the order book based on packets[exec_packet_id]

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


            if (snapshot_id & (1 << SNAPSHOT_BOOT_BIT)) {
                printf("boot event\n");

                // for now, le't sjust say we wena tto connect to the websocket

                u64 delay = cz_postboot_socket(0);

                printf("%llu\n", delay);

                Packet p = {.type = 1 << SNAPSHOT_SOCKET_BIT, .client_id = client_id};

                u32 packet_id = fl_insert(packets,&p);
                printf("packet id, %u\n", packet_id);

                u64 socket_event = ((CLIENT_OUT_TYPE & T_MASK) << PARAM_BITS) | (packet_id & PARAM_MASK);

                //printf("delay %llu, event %llu\n", event, delay);

                sch_schedule(sch, socket_event, delay);
                continue;
            }

            // otherwise we actually look at the snapshot and do stuff with it
            


        } else if (type == CLIENT_OUT_TYPE) {

            u32 packet_id = params;
            Packet packet = *(Packet*)fl_get(packets, packet_id);

            // roughtly along these lines, need better solution

            u64 base_jitter = cz_base_latency(0);
            u64 random_jitter = 
                (base_jitter) + // 1.0x
                (base_jitter >> 7) + // + ~.01x
                (((base_jitter * (*rand & MAX_U32)) >> 32 ) >> 5); // + 0x~.03x



            printf("client out type\n");

            u64 out_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (packet_id & PARAM_MASK);

            sch_schedule(sch, out_event, random_jitter);
        } else if (type == SLOW_REPEAT_TYPE) {
            printf("slow repeat type\n");
        } 


        //boot has been removed. there is no difference between a computer that is off and a computer that is disconnected from the socket in this sim



    }


    sch_free(sch);
    cz_free(client);
    rand_free(rand);
    free(server);

    cb_free(hw_queue);
    cb_free(sw_queue);

    return 0;
}
