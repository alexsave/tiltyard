#ifndef HOLDER_H
#define HOLDER_H

#include "types.h"

typedef struct Holder {
    u32* client_allocations;
    void** client_data;
} Holder;

Holder* holder_init(u32* client_allocations);
void holder_free(Holder* ho);

#endif
