#ifndef CLIENT_H
#define CLIENT_H

#include <stdlib.h>
#include "context.h"
#include "types.h"

#include "strategy/client_zero.h"
#include "strategy/client_one.h"

// just add more as you wish :)
#define IMPLS \
    X(cz) \
    X(co)

// THIS WHOLE THING needs to be passed around

// Interface
typedef struct Client {
    // Read reads up to len(p) bytes into p.
    void* (*client_init)();
    char* (*get_name)(void* self);
    //Order (*on_snapshot)(void* self, void* snapshot, Context* context); 
    // return 1 means order, return 0 means no order
    u8 (*on_snapshot)(void* self, Context* ctx); 
    void (*client_free)(void* self);

    // not as sure about these but hey 
    // when to schedule the FIRST awaken, called just once
    u64 (*initial_wakeup)(void* self);
    // how long it takes to do something on client
    u64 (*processing_time)(void* self);
    u64 (*net_latency)(void* self);
} Client;

typedef struct TypeMetadata {
    Client * all_clients;
    #define X(name) u8 name ## _index;
    IMPLS
    #undef X
    u8 IMPLS_COUNT;
} TypeMetadata;

// start this whole thing up
TypeMetadata* get_types();

void tm_free(TypeMetadata* tm);


#endif

