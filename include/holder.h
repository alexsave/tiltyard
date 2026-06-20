#ifndef HOLDER_H
#define HOLDER_H

#include "types.h"
#include "client.h"

typedef struct Holder {
    u32* client_allocations;
    void** client_data;
    u32 num_clients;
    TypeMetadata* tm;
} Holder;

Holder* holder_init(TypeMetadata* tm, u32* client_allocations);
u64* holder_get_init_ns(Holder * ho);

u8 holder_client_on_snapshot(Holder * ho, u32 client_id, Context* context);

void holder_free(Holder* ho);

#endif
