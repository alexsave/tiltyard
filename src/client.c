#include "client.h"
#include "types.h"
#include "strategy/client_zero.h"
#include "strategy/client_one.h"

TypeMetadata* get_types() {
    TypeMetadata* tm = malloc(sizeof(TypeMetadata));

    #define X(name) tm->name ## _index = tm->IMPLS_COUNT; tm->IMPLS_COUNT++;
    IMPLS
    #undef X

    tm->all_clients = malloc(tm->IMPLS_COUNT * sizeof(Client));

    #define X(a) \
        tm->all_clients[tm->a ## _index].client_init = (void* (*)())a ##_init; \
        tm->all_clients[tm->a ## _index].get_name = (char* (*)(void*))a ##_get_name; \
        tm->all_clients[tm->a ## _index].on_snapshot = (u32 (*)(void*, void*))a ##_on_snapshot; \
        tm->all_clients[tm->a ## _index].client_free = (void (*)(void*))a ##_free; \
        tm->all_clients[tm->a ## _index].initial_wakeup = (u64 (*)(void*))a ##_initial_wakeup; \
        tm->all_clients[tm->a ## _index].processing_time = (u64 (*)(void*))a ##_processing_time; \
        tm->all_clients[tm->a ## _index].net_latency = (u64 (*)(void*))a ##_net_latency;
    IMPLS
    #undef X

    return tm; 
}

void tm_free(TypeMetadata* tm){
    free(tm->all_clients);
    free(tm);
}

