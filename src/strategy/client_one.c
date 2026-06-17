#include <stdlib.h>

#include "strategy/client_one.h"
#include "sch.h"

CO* co_init() {
    return malloc(sizeof(CO));
}

uint64_t co_initial_boot_time(CO* co) {
    return (uint64_t)24*60*60*S_TO_NS;
}

// you turn on the comptuer, then the trading software launches in like 15 seconds?
uint64_t co_postboot_socket(CO* co) {
    return (uint64_t)15 * S_TO_NS;
}

uint64_t co_base_latency(CO* co) {
    return (uint64_t)300000000;// 300 ms?
}




void co_free(CO* co) {
    free(co);
}
