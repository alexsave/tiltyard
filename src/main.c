#include <stdio.h>
#include <stdint.h>

#include "rand.h"
#include "sch.h"
#include "strategy/client_zero.h"

void log_full(uint64_t raw) {
    printf("raw: %llu, priority: %llu, type: %llu, params: %llu\n", 
        raw, 
        raw >> E_BITS, 
        (raw >> (E_BITS-T_BITS)) & T_MASK, 
        raw & PARAM_MASK);
}

int main(int argc, char* argv[]){
        
    static uint64_t BOOT_TYPE = 0;
    static uint64_t SLOW_REPEAT_TYPE = 7;

    SCH* sch = sch_init();

    // Initialization

    uint64_t event = SLOW_REPEAT_TYPE << (PARAM_BITS) | 0;
    // is 0 safe to schedule into?
    // maybe
    sch_schedule(sch, event, 0);



    CZ* client = cz_init();

    uint64_t client_id = 0; // good case for freelist maybe

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

    for(int i = 0; i < 1000; i++){
        uint64_t next = sch_pop(sch);

        uint64_t now_ns = sch_now_ns(sch);
        printf("Now %llu or ~%llus - ", now_ns, now_ns/1000000000);
        log_full(next);

        uint8_t type = (next >> PARAM_BITS) & T_MASK;
        
        uint64_t params = next & PARAM_MASK;

        // i dont like switches lol
        if (type == BOOT_TYPE) {
            //client toggle on/off

            // we should actually see this soon

            
                
        } else if (type == 1) {
            // client socket toggle connected/unconnected
        } else if (type == 2) {
            // "server broadcast start" 
        } else if (type == 3) {
            // "client in"
        } else if (type == 4) {
            // "server in"
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

    return 0;
}
