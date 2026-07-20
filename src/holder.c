#include <stdlib.h>
#include <stdio.h>
#include "client.h"
#include "types.h"
#include "constants.h"
#include "holder.h"
#include "client_settings.h"

// methods for dealing with the fact that we have many clients and many types


// this takes ownership of client_allocations
Holder* holder_init(TypeMetadata* tm, u32* client_allocations, ClientSettings** client_settings) {
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

    // ok hear me out
    *client_settings = calloc(num_clients, sizeof(ClientSettings));

    // calloc's 0 would read as "a wake already pending at t=0" and drop every one after it
    for (u32 c = 0; c < num_clients; c++)
        (*client_settings)[c].next_wake_ns = MAX_U64;

    ho->num_clients = num_clients;
    // probably the biggest memory block in the entire program
    ho->client_data = malloc(num_clients * sizeof(void*));

    type_index = 0;
    u32 client_id = 0;
    // doesn't even matter if cz_index is 1 or 0 or whatever
    for (type_index = 0; type_index < tm->IMPLS_COUNT; type_index++) {
        for (u32 i = 0; i < client_allocations[type_index]; i++){
            ho->client_data[client_id] = (ho->tm->all_clients[type_index].client_init)();

            // somethign like 
            
            (ho->tm->all_clients[type_index].get_settings)(ho->client_data[client_id], &((*client_settings)[client_id]));
            // what do you think

            // store this somewhere
            client_id++;
        }
    }

    return ho;
}


// this wont be necessary anymore

// loop through all, get initalize wakeup values
/*u64* holder_get_init_ns(Holder * ho){
    u64* init_ts = malloc(ho->num_clients * sizeof(u64));
    u32 client_id = 0;
    //printf("%d, %d\n", ho->tm->IMPLS_COUNT, ho->client_allocations[0]);
    for (u32 type_index = 0; type_index < ho->tm->IMPLS_COUNT; type_index++) {
        for (u32 i = 0; i < ho->client_allocations[type_index]; i++){
            // so this is something like CZ*
            init_ts[client_id] = (ho->tm->all_clients[type_index].initial_wakeup)(ho->client_data[client_id]);
            //printf("%llu is a init time\n", init_ts[client_id]);


            client_id++;
        }
    }

    return init_ts;
}*/

u8 holder_client_on_snapshot(Holder * ho, u32 client_id, Context* context) {
    u32 running_max = 0;
    u32 type_index = 0;
    //printf("client id %u\n", client_id);

    //printf("%p\n", ho);
    //printf("%p\n", ho->tm);
    for (; type_index < ho->tm->IMPLS_COUNT; type_index++) {
        u32 bucket_count = ho->client_allocations[type_index];
        running_max += bucket_count;
        //printf("running_max %u\n", running_max);
        if (running_max > client_id)
            break;
    }
    if (running_max < client_id){
        printf("client id out of range of allocations, this is probably a bug %u %u\n", running_max, client_id);
        exit(1);
        return 255;
    }

    u8 ret = (ho->tm->all_clients[type_index].on_snapshot)(ho->client_data[client_id], context);
    return ret;
}

void holder_free(Holder * ho) {
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
    tm_free(ho->tm);// not sure why this was commented out
    free(ho->client_data);
    free(ho);
}

