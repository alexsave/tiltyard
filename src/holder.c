#include <stdlib.h>
#include "client.h"
#include "types.h"
#include "holder.h"

// methods for dealing with the fact that we have many clients and many types


// this takes ownership of client_allocations
Holder* holder_init(u32* client_allocations) {
    Holder* ho = malloc(sizeof(Holder));
    ho->client_allocations = client_allocations;

    u32 type_index = 0;
    // FINALLY this comes into play

    u32 num_clients = 0;
    for (type_index = 0; type_index < IMPLS_COUNT; type_index++)
        for (u32 i = 0; i < client_allocations[type_index]; i++)
            num_clients++;

    // probably the biggest memory block in the entire program
    ho->client_data = malloc(num_clients * sizeof(void*));

    type_index = 0;
    u32 client_id = 0;
    // doesn't even matter if cz_index is 1 or 0 or whatever
    for (type_index = 0; type_index < IMPLS_COUNT; type_index++) {
        for (u32 i = 0; i < client_allocations[type_index]; i++){
            // so this is something like CZ*
            ho->client_data[client_id] = all_clients[type_index]->client_init();
            //printf("%p %p \n", (void*)(all_clients[type_index]->client_free), client_data[client_id]);

            // store this somewhere
            client_id++;
        }
        // so go through all the types, and create client_allocation amount of that type of client
    }

    return ho;
}

void holder_free(Holder * ho) {
    u32 type_index = 0;
    // FINALLY this comes into play
    u32 client_id = 0;  
    // doesn't even matter if cz_index is 1 or 0 or whatever
    for (u32 type_index = 0; type_index < IMPLS_COUNT; type_index++) {
        for (u32 i = 0; i < ho->client_allocations[type_index]; i++){
            // so this is something like CZ* 

            // probably this is causing seg fault
       
            //printf("client id %d\n", client_id);
            //printf("%p %p \n", (void*)(all_clients[type_index]->client_free), client_data[client_id]);

            (all_clients[type_index]->client_free)(ho->client_data[client_id]);
            // store this somewhere
            client_id++;
        }   

    }   
    // deceiving, but its really that mapping of index to client impl
    all_clients_free();

    free(ho->client_allocations);
    free(ho->client_data);
    free(ho);
}

