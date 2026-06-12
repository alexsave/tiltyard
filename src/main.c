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
// default HW packet, but easier to indicate this is a SW packet later
static uint8_t SW_BIT = 1;

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

        uint32_t start_server_exec = 2;

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
            uint32_t hw_to_sw = 2;

            // ok so server in. if we have a hardware packet, we schedule a software packet enqueue
            // this will be pseudocode as I'm sleepy 

            // ah fuck we need a fl_get that doesn't discard it

            if (params == hw_to_sw){
                uint32_t packet_id = cb_pop(hw_queue);
                cb_push(sw_queue, packet_id);

                // btw we need software queue, because when a stop loss order converts to a market, it needs to go back into the software queue but NOT thee full NIC hardware queue
                // hence this mess
                    
                uint64_t sw_to_exec_ns = 100; //100ns? idk

                // as mentioned, schedule that SERVER_EXEC_START command
                uint64_t server_exec = ((SERVER_EXEC_TYPE & T_MASK) << PARAM_BITS) | (start_server_exec & PARAM_MASK);
                sch_schedule(sch, server_in, sw_to_exec_ns);
                continue;
            }

            Packet* out = (Packet*)(fl_get(packets,params));
            if ((out->type & (1 << SW_BIT)) == 0) {
                //hardware
                cb_push(hw_queue, out);
                // schedule a hw->sw event
                // ideally we keep the memory but just mdoify that one bit from HW to SW

                // special packet_id that indicates HW->SF
                uint64_t server_in = ((SERVER_IN_TYPE & T_MASK) << PARAM_BITS) | (hw_to_sw & PARAM_MASK);
                // this will "call itself" later from the scheduler

                uint64_t hw_to_sw_ns = 10000; //10micro?
                sch_schedule(sch, server_in, hw_to_sw_ns);
                
                // and that's it
                // we don't need the packet id in the params for the rest of the server events

                continue;
            }

        

            

            // "server in"

            // let's work through an example
            // there is nothing in the server queue
            // schedule a server exec event for some fixed latency away 
            // server exec doesn't actually need params

            // notes suggest that we fold all server events into a single type
            // so sometimes it takes a packet, but certain packet ids actually mean "server_exec"
            // even the slow checker is suggested to be folded in
            // but idk we'll do it like this for now

            if (server_queue->size == 0) {
                uint64_t nic_to_sw_latency = 1000;// 1 microsecond?


                
                // somethign like this - circular buffer push
                cb_push(packet_id);

                uint64_t server_in = ((SERVER_EXEC_TYPE & T_MASK) << PARAM_BITS) | (packet_id & PARAM_MASK);
                sch_schedule(sch, server_in, nic_to_sw_latency);
                // and that's it
                continue;
            } else {
                // let's say there is one packet in the queue
                // that packet arrived at some time y
                // it cannot get to server exec BEFORE y + nic_to_sw_latency

                // our packet will get to server_exec at y + nic_to_sw_latency + order_processing time

                // our packet arrived at nic at time x, where x > y

                // our packet CANNOT be processed before x + nic_to_sw_latency
                // question - 
                // is y + nic_to_sw_latency + order_processing_time > x + nic_to_sw_latency
                // y + order_processing_time > x
                // not necessarily if they arrive right after each other

                // ok how about this - each arrival of each packet to the NIC schedules a server_exec at nic_to_sw_latency away
                // so the arrival of y schedules y + nic_to_sw_latency exec event
                // the arrival of x schedules a x + nic_to_sw_latency exec event
                // as long as the order of [y,x] is maintainedo

                // oh wait
                // if x and y arrive very close in time, less diff than processing time, then we will schedule two server_exec calls in less time than it takes to process an order
                // not ideal
                // unless....
                // the "server_exec" code controls what it pulls
                // so we can schedule as many server_exec calls as we want
                // but if the server is "mid calculation", it won't take anything
                // slow arrival case:
                // packet A arrives to nic, schedules exec, it gets executed, some time later, server state is set to not processing because the queue is empty, packet B schedules exec AFTER that and it gest 

                // let's assume a large execution time

                // fast arrival chronoligcal order
                // paacket A arrives to NIC, packet B arrives to NIC, packet As server exec request start, packet Bs server exec request start (must be rejected), packetAs server exec done (and immediately reschedules packetBs server exec request becasue queue is not empty), server executes packetBs server exec request

                // fast relative execution time

                // slow arrival chronological order
                // packet A arrives to NIC, packet B arrives to NIC, packetAs server exec start, packet As server exec finish, packetBs server exec start, packet Bs server exec finish

                // what is the state we need to maintain?
                // the main difference is the ordering of 
                // packetsA server exec finish vs packetBs server exec start 
                // SERVER EXEC START event type must first check some "executing" state, and reject (not at scheduling time because we can't know?) and ignore the event if it's the case. if not set, set it to true and do order book stuff
                // SERVER EXEC STOP event type must turn off the executing state (as well as scheduling client delivery)

                // now how do we make sure that if we do have a order in queue (rejected exec start), we execute it immediately after EXEC STOP, so right after the previous order prcoessing
                // we could just execute within that exec stop, keep the flag on, and reschedule another EXEC STOP at "processing time" later

                // so it'll be MOSTLY exec stop events, driven by server order processing time, with the occastional exec start kinda "waking up" this flow

                // this gets even more fun when you consider we need two queues!
                // FUCK
                // or maybe the HW queue vs SW queue is just a flag in the packet type

                // I think the coffee is frying my brain cells a bit
                // but we can do something similar
                // each arrival of a HW packet schedules an HW dequeue + SW enqueue event
                // some fixed latency later
                // maybe in this case, 
                // two HW arrivals right after each other is fine to == two SW arrivals right after each other
                // let me think about this... yeah fuck it why not
                // so the madness is only in the SW -> order book execution part
                

                // fuck i think we need an additional type, or to hardcode some "packet ids" to mean server stuff
               
            }


            // in this case params is packet id
            // packet id is in the params

            Packet out = *(Packet*)(fl_release(packets,params));

            printf("server got a packet!!! %d, %d \n", out.type, out.client_id);

            // ok so the server is really busy.
            // just because a client WANTS to connect with a websocket doesn't mean the server is obligated to immediately

            // it needs to wait in line behind everyone else... I think
            // a websocket request doesn't need the orderbook logic
            //hmm

            // whatever

            // but let's do the whole server exec scheduling, which is the real juicy part

            // um
            // if there is something in the server exec queue, just push this packet onto it
            // the server exec will kinda pop itself
            // if the server exec queue is empty (less likely), then this server in needs to kick it off
            // therefore
            // we need yet another data structure

            // ah this is actually strange
            // when a packet gets to the server: 

            // 1.at some time it enters the NIC
            // 2. some time later it enters a software queue (you'll see why its separate one day)
            // 3. some time later it hits the order book code
            // even if the queues are empty, it cannot skip some fixed latency between HW queue -> SW queue and SW queue -> execution
            // hence the scheduler thing



            // dont' need that packet anymore

        } else if (type == 5) {
            // server exec - pull an order from server q


            // no params - maybe - just pull off the server queue

            // ok so
            if (params == start_server_exec) {
                if (server->processing == 1){
                    //just ignore, we can't pop from the queue
                    // the next end_server_exec will handle it

                    continue;
                }

                server->processing = 1;
                // TODO handle the thing

                Packet p = cb_pop(sw_queue);// somethign along these lines
                // finally releases the packet


                // then schedule an end
                uint64_t server_exec = ((SERVER_EXEC_TYPE & T_MASK) << PARAM_BITS) | (end_server_exec & PARAM_MASK);
                sch_schedule(sch, server_exec, execution_time_ns);
                

            } else if (params == end_server_exec) {
                //server->processing = 0;
                // this was requested by a previous server_exec run
                // this is like AFTER execution
        
                // in any case, schedule messages to connected sockets
                // (a whole other complex thing) for later

                if (!cb_empty(sw_queue)){ // yes, sw enqueueing will scheudle serverexec
                    Packet p = cb_pop(sw_queue);// somethign along these lines
                    // TODO execute on it


                    // then scheudle another "done"
                    uint64_t server_exec = ((SERVER_EXEC_TYPE & T_MASK) << PARAM_BITS) | (end_server_exec & PARAM_MASK);
                    sch_schedule(sch, server_exec, execution_time_ns);

                } else {
                    // we're done here, allow the next "start_server" to kick things off again
                    server->processing = 0;
                }


            }




        } else if (type == 6) {
            // order book delivery to client

            // there's going to be SO SO SO many of these
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
