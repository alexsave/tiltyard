#include "client.h"
#include "types.h"
#include "strategy/client_zero.h"
#include "strategy/client_one.h"

TypeMetadata* get_types() {
    TypeMetadata* tm = malloc(sizeof(TypeMetadata));
    tm->all_clients = malloc(2 * sizeof(Client*));

    #define X(name) tm->name ## _index = tm->IMPLS_COUNT; tm->IMPLS_COUNT++;
    IMPLS
    #undef X


    // these two need to combine


    #define X(a1) \
    Client* a1 = malloc(sizeof(Client)); \
        a1 ->client_init = (void* (*)())a1 ##_init; \
        a1 ->get_name = (char* (*)(void*))a1 ##_get_name; \
        a1 ->on_snapshot = (u32 (*)(void*, void*))a1 ##_on_snapshot; \
        a1 ->client_free = (void (*)(void*))a1 ##_free; \
        a1 ->initial_wakeup = (u64 (*)(void*))a1 ##_initial_wakeup; \
        a1 ->processing_time = (u64 (*)(void*))a1 ##_processing_time; \
        a1 ->net_latency = (u64 (*)(void*))a1 ##_net_latency; 
    IMPLS
    #undef X

    #define X(a1) tm->all_clients[tm->a1 ## _index] = a1;
    IMPLS
    #undef X



    return tm; 
}

void tm_free(TypeMetadata* tm){
    free(tm->all_clients);
    free(tm);
}
