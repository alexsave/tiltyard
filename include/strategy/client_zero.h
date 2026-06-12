#ifndef CLIENT_ZERO_H
#define CLIENT_ZERO_H

// this will eventually be C's equivalent of an interface but for now-

typedef struct CZ {
    uint64_t idk;
} CZ;

CZ* cz_init();

uint64_t cz_initial_boot_time(CZ* cz);
uint64_t cz_postboot_socket(CZ* cz);
uint64_t cz_base_latency(CZ* cz);

void cz_free(CZ* cz);

#endif
