#ifndef CLIENT_H
#define CLIENT_H

#include "strategy/client_zero.h"

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

// Glue code

const Client CZ_Client = { 
    .client_init = (void* (*)())cz_init,
    .get_name = (char* (*)(void*))cz_get_name,
    .on_snapshot = (u32 (*)(void*, void*))cz_on_snapshot,
    .client_free = (void (*)(void*))cz_free,
    .initial_wakeup = (u64 (*)(void*))cz_initial_boot_time,
    .processing_time = (u64 (*)(void*))cz_postboot_socket,
    .net_latency = (u64 (*)(void*))cz_base_latency,
};

// hmm
// this annoying as fuck
// maybe there's a better way
/*const Client CO_Client = { 
    .client_init = (void* (*)())cz_init,
    .get_name = (char* (*)(void*))cz_get_name,
    .on_snapshot = (u32 (*)(void*, void*))cz_on_snapshot,
    .client_free = (void (*)(void*))cz_free,
    .initial_wakeup = (u64 (*)(void*))cz_initial_boot_time,
    .processing_time = (u64 (*)(void*))cz_postboot_socket,
    .net_latency = (u64 (*)(void*))cz_base_latency,
};*/


#endif

