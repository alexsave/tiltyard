#ifndef CLIENT_ZERO_H
#define CLIENT_ZERO_H

#include "types.h"
#include "context.h"


// this will eventually be C's equivalent of an interface but for now-

typedef struct CZ {
    uint64_t idk;
} CZ;

CZ* cz_init();

char* cz_get_name(CZ* cz);
u8 cz_on_snapshot(CZ* cz, Context* ctx);

uint64_t cz_initial_wakeup(CZ* cz);
uint64_t cz_processing_time(CZ* cz);
uint64_t cz_net_latency(CZ* cz);

void cz_free(CZ* cz);

#endif
