#include <stdlib.h>
#include <stdio.h>
#include "client.h"
#include "types.h"
#include "holder.h"

// methods for dealing with the fact that we have many clients and many types


// this takes ownership of client_allocations
Holder* holder_init(TypeMetadata* tm, u32* client_allocations) {
    Holder* ho = malloc(sizeof(Holder));
    ho->client_allocations = client_allocations;
    ho->tm = tm;

    u32 type_index = 0;
    // FINALLY this comes into play

    u32 num_clients = 0;
    for (type_index = 0; type_index < tm->IMPLS_COUNT; type_index++)
        for (u32 i = 0; i < client_allocations[type_index]; i++){
            num_clients++;
        }

    ho->num_clients = num_clients;
    // probably the biggest memory block in the entire program
    ho->client_data = malloc(num_clients * sizeof(void*));

    type_index = 0;
    u32 client_id = 0;
    // doesn't even matter if cz_index is 1 or 0 or whatever
    for (type_index = 0; type_index < tm->IMPLS_COUNT; type_index++) {
        for (u32 i = 0; i < client_allocations[type_index]; i++){
            //printf("%d, %d\n", ho->tm->IMPLS_COUNT, ho->client_allocations[0]);
            printf("hi\n");
            // so this is something like CZ*
              printf("pointer %p \n", (void*)(ho->tm->all_clients[type_index].client_free));
            //printf("%p %p \n", (void*)(ho->tm->all_clients[type_index]->client_free), ho->client_data[client_id]);
            ho->client_data[client_id] = (ho->tm->all_clients[type_index].client_init)();
            printf("bye\n");

            // store this somewhere
            client_id++;
        }
        // so go through all the types, and create client_allocation amount of that type of client
    }
            printf("end of loop\n");

    return ho;
}

// loop through all, get initalize wakeup values
u64* holder_get_init_ns(Holder * ho){
    u64* init_ts = malloc(ho->num_clients * sizeof(u64));
    u32 client_id = 0;
    printf("%d, %d\n", ho->tm->IMPLS_COUNT, ho->client_allocations[0]);
    for (u32 type_index = 0; type_index < ho->tm->IMPLS_COUNT; type_index++) {
        for (u32 i = 0; i < ho->client_allocations[type_index]; i++){
            // so this is something like CZ*
            init_ts[client_id] = (ho->tm->all_clients[type_index].initial_wakeup)(ho->client_data[client_id]);
            printf("%llu is a init time\n", init_ts[client_id]);


            client_id++;
        }
    }

    return init_ts;
}

void holder_free(Holder * ho) {
    u32 type_index = 0;
    // FINALLY this comes into play
    u32 client_id = 0;  
    // doesn't even matter if cz_index is 1 or 0 or whatever
    for (u32 type_index = 0; type_index < ho->tm->IMPLS_COUNT; type_index++) {
        for (u32 i = 0; i < ho->client_allocations[type_index]; i++){
            // so this is something like CZ* 

       
            (ho->tm->all_clients[type_index].client_free)(ho->client_data[client_id]);
            // store this somewhere
            client_id++;
        }   

    }   
    // deceiving, but its really that mapping of index to client impl

    free(ho->client_allocations);
    //tm_free(ho->tm);
    free(ho->client_data);
    free(ho);
}

