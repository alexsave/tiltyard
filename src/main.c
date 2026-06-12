#include <stdio.h>
#include <stdint.h>

#include "rand.h"
#include "sch.h"
#include "fl.h"
#include "strategy/client_zero.h"

void log_full(uint64_t raw) {
    printf("raw: %llu, priority %llu, type %llu, params %llu\n", 
        raw, 
        raw >> E_BITS, 
        (raw >> (E_BITS-T_BITS)) & T_MASK, 
        raw & PARAM_MASK);
}

// indicates a packet is a websocket thing
static uint8_t WS_BIT = 0;

static uint64_t BOOT_TYPE = 0;
static uint64_t SOCKET_TYPE = 1;
static uint64_t SERVER_IN_TYPE = 4;
static uint64_t SLOW_REPEAT_TYPE = 7;

// ok so this is going to have a BUNCH of stuff in it, everything we need to specify an order
// really these are client->server network events, but wel
// ah that's the term
// for now just type and clientid
typedef struct Packet {
    uint8_t type;
    uint32_t client_id;
} Packet;

int main(int argc, char* argv[]){

    FL* packets = fl_init(sizeof(Packet));

    uint64_t seed = 603603603;
    uint64_t* rand = rand_init(seed);
    rand_next(rand);


    SCH* sch = sch_init();

    // Initialization

    uint64_t event = SLOW_REPEAT_TYPE << (PARAM_BITS) | 0;
    // is 0 safe to schedule into?
    // maybe
    sch_schedule(sch, event, 0);



    CZ* client = cz_init();

    //more interesting
    uint64_t client_id = 123; // good case for freelist maybe

    uint64_t first_boot = cz_initial_boot_time(client);


    // not all these operations are necessary but this is the template
    uint64_t boot_event = ((BOOT_TYPE & T_MASK) << PARAM_BITS) | (client_id & PARAM_MASK);

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

        // i dont like switches lol
        if (type == BOOT_TYPE) {
            //client toggle on/off

            // AH hold on, WHICH client?
            // we will have a more concrete client table later

            // we should actually see this soon

            uint64_t socket_delay = cz_postboot_socket(client);
            uint64_t socket_event = ((SOCKET_TYPE & T_MASK) << PARAM_BITS) | (params & PARAM_MASK);
            sch_schedule(sch, socket_event, socket_delay);

        } else if (type == 1) {
            // client socket toggle connected/unconnected

            // let's say clients DO NOT have a out network queue

            // this calculation will be used for network latency in a few places
            uint64_t base_jitter = cz_base_latency(client);
            uint64_t bottom = ((uint64_t)1 << 32) - 1;
            uint64_t random_jitter = 
                (base_jitter) + 
                (base_jitter >> 7) +
                (((base_jitter * (*rand & bottom)) >> 32 ) >> 5);


            // now here's the trick
            // i don't think we should have an event specifically for
            // "client socket connection reached server"
            // this is where freelist comes in

            uint8_t type = 0;
            type = type | (1 << WS_BIT);
            // this is legal C, right?
            Packet p = { .type = type, .client_id = params};

            // we cannot just use &packet or something, because 64 bit addresses do not fit in the 24 or so bits of parameters
            uint32_t packet_id = fl_insert(packets, &p);

            // ok now shove it into the packet table

            // what the hell is an order id
            // so glad you asked
            uint64_t server_in = ((SERVER_IN_TYPE & T_MASK) << PARAM_BITS) | (packet_id & PARAM_MASK);

            sch_schedule(sch, server_in, random_jitter);

        } else if (type == 2) {
            // "server broadcast start" 
        } else if (type == 3) {
            // "client in"
        } else if (type == 4) {
            // "server in"


            // in this case params is packet id
            // packet id is in the params

            Packet out = *(Packet*)(fl_release(packets,params));

            printf("server got a packet!!! %d, %d \n", out.type, out.client_id);
            
            // dont' need that packet anymore
        
        } else if (type == 5) {
            // server exec - pull an order from server q
        } else if (type == 6) {
            // order book delivery to client
        } else if (type == 7) {
            // recurring slow checker event - with special params for EOD, EOM
        }

        // im not even sure about these event types honestly
        // maybe some can be removed

        // but here is the flow, for the most complex type of client
        // server is assumed on since before the dawn of time
        // some "init" method boots each client at some time with a type 0 event
        // some time after booting, client schedules a socket toggle event to connect
        // this gets to the server... but doesn't need to get to the order book handler tho
        // but i still want to simulate this socket latency
        // so maybe socket toggle is a special type of client order
        // this gets into server queue
        // at some point is then pulled into server exec 
        // server_exec then schedules a mesage sent only to that specific client
        // "client in" recieves the socket connection
        // it is now connected, so eventually server_exec will schedule additional client in events
        // these client will read the data from client in events and make decision, scheduling them at "client out"
        // client out events then reach the server in, 
        // what a mess
        // but lets see if we can do at least the first slowly



    }


    sch_free(sch);
    cz_free(client);
    rand_free(rand);

    return 0;
}
