#include <stdlib.h>

#include "strategy/client_one.h"
#include "sch.h"
#include "types.h"

CO* co_init() {
    return malloc(sizeof(CO));
}

char* co_get_name(CO* co){
    return "client.1";
}

u32 co_on_snapshot(CO* cz, void* snapshot){
    return 32; 
}

u64 co_initial_wakeup(CO* co) {
    return (uint64_t)24*60*60*S_TO_NS;
}

// you turn on the comptuer, then the trading software launches in like 15 seconds?
u64 co_processing_time(CO* co) {
    return (uint64_t)15 * S_TO_NS;
}

u64 co_net_latency(CO* co) {
    return (uint64_t)300000000;// 300 ms?
}




void co_free(CO* co) {
    free(co);
}
