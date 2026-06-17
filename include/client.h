#ifndef CLIENT_H
#define CLIENT_H

#include <stdlib.h>
#include "strategy/client_zero.h"
#include "strategy/client_one.h"

// Interface
typedef struct Client {
    // Read reads up to len(p) bytes into p.
    void* (*client_init)();
    char* (*get_name)(void* self);
    //Order (*on_snapshot)(void* self, void* snapshot); 
    u32 (*on_snapshot)(void* self, void* snapshot); 
    void (*client_free)(void* self);

    // not as sure about these but hey 
    // when to schedule the FIRST awaken, called just once
    u64 (*initial_wakeup)(void* self);
    // how long it takes to do something on client
    u64 (*processing_time)(void* self);
    u64 (*net_latency)(void* self);
} Client;

// just add more as you wish :)
#define IMPLS \
    X(co) \
    X(cz)

#define X(a1) \
    static Client a1 ##_client = { \
        .client_init = (void* (*)())a1 ##_init, \
        .get_name = (char* (*)(void*))a1 ##_get_name, \
        .on_snapshot = (u32 (*)(void*, void*))a1 ##_on_snapshot, \
        .client_free = (void (*)(void*))a1 ##_free, \
        .initial_wakeup = (u64 (*)(void*))a1 ##_initial_wakeup, \
        .processing_time = (u64 (*)(void*))a1 ##_processing_time, \
        .net_latency = (u64 (*)(void*))a1 ##_net_latency, \
    };
IMPLS
#undef X

static Client ** all_clients;

#define X(name) static u8 name ## _index;
IMPLS
#undef X

static u8 IMPLS_COUNT = 0;

void assign_indicies() {
    all_clients = malloc(2 * sizeof(Client*));
#define X(name) name ## _index = IMPLS_COUNT; IMPLS_COUNT++;
IMPLS
#undef X

#define X(a1) all_clients[a1 ## _index] = &a1 ##_client;
IMPLS
#undef X

}

static void all_clients_free(){
    free(all_clients);
}


#endif

