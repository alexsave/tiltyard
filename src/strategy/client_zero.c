#include <stdlib.h>

#include "strategy/client_zero.h"
#include "sch.h"

CZ* cz_init() {
    return malloc(sizeof(CZ));
}

uint64_t cz_initial_boot_time(CZ* cz) {
    return (uint64_t)24*60*60*S_TO_NS;
}

// you turn on the comptuer, then the trading software launches in like 15 seconds?
uint64_t cz_postboot_socket(CZ* cz) {
    return (uint64_t)15 * S_TO_NS;
}

uint64_t cz_base_latency(CZ* cz) {
    return (uint64_t)300000000;// 300 ms?
}



void cz_free(CZ* cz) {
    free(cz);
}
