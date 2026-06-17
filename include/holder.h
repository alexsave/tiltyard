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
u64* holder_get_init_ts(Holder * ho);
void holder_free(Holder* ho);

#endif
