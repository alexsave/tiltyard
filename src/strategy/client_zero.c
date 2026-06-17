#include <stdlib.h>

#include "strategy/client_zero.h"
#include "sch.h"
#include "types.h"

CZ* cz_init() {
    return malloc(sizeof(CZ));
}

char* cz_get_name(CZ* cz) {
    return "client.0";
}

u32 cz_on_snapshot(CZ* cz, void* snapshot){
    return 32;
}


uint64_t cz_initial_wakeup(CZ* cz) {
    return (uint64_t)24*60*60*S_TO_NS;
}

// you turn on the comptuer, then the trading software launches in like 15 seconds?
uint64_t cz_processing_time(CZ* cz) {
    return (uint64_t)15 * S_TO_NS;
}

uint64_t cz_net_latency(CZ* cz) {
    return (uint64_t)300000000;// 300 ms?
}




void cz_free(CZ* cz) {
    free(cz);
}
