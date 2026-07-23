#include "client.h"
#include "client_settings.h"
#include "types.h"
#include "strategy/client_zero.h"
#include "strategy/client_one.h"

TypeMetadata* get_types() {
    // calloc: the X below reads IMPLS_COUNT before anything writes it, so it has to start at 0.
    // malloc leaves it indeterminate, which -O2 and up fold into garbage indices
    TypeMetadata* tm = calloc(1, sizeof(TypeMetadata));

    #define X(name) tm->name ## _index = tm->IMPLS_COUNT; tm->IMPLS_COUNT++;
    IMPLS
    #undef X

    tm->all_clients = malloc(tm->IMPLS_COUNT * sizeof(Client));

    #define X(a) \
        tm->all_clients[tm->a ## _index].client_init = (void* (*)())a ##_init; \
        tm->all_clients[tm->a ## _index].get_name = (char* (*)(void*))a ##_get_name; \
        tm->all_clients[tm->a ## _index].on_snapshot = (u8 (*)(void*, Context*))a ##_on_snapshot; \
        tm->all_clients[tm->a ## _index].get_settings = (void (*)(void*, ClientSettings*))a ##_get_settings; \
        tm->all_clients[tm->a ## _index].client_free = (void (*)(void*))a ##_free;
    IMPLS
    #undef X

    return tm; 
}

void tm_free(TypeMetadata* tm){
    free(tm->all_clients);
    free(tm);
}

