#include <stdlib.h>

#include "strategy/client_zero.h"
#include "sch.h"

CZ* cz_init() {
    return malloc(sizeof(CZ));
}

uint64_t cz_initial_boot_time(CZ* cz) {
    return (uint64_t)24*60*60*S_TO_NS;
}


void cz_free(CZ* cz) {
    free(cz);
}
