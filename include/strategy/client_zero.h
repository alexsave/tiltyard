#ifndef CLIENT_ZERO_H
#define CLIENT_ZERO_H

#include "types.h"
#include "context.h"
#include "client_settings.h"


// this will eventually be C's equivalent of an interface but for now-

typedef struct CZ {
    uint64_t idk;
} CZ;

CZ* cz_init();

char* cz_get_name(CZ* cz);
u8 cz_on_snapshot(CZ* cz, Context* ctx);

void cz_get_settings(CZ* cz, ClientSettings* client_settings);

void cz_free(CZ* cz);

#endif
