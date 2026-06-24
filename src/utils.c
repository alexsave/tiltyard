#include "utils.h" 

#include "types.h" 
#include "constants.h" 
#include "client_settings.h" 

u64 calculate_jitter(ClientSettings* cs, u64* rand){
    u64 base_jitter = cs->net_latency;
    return 
        (base_jitter) + // 1.0x
        (base_jitter >> 7) + // + ~.01x
        (((base_jitter * (*(rand) & MAX_U32)) >> 32 ) >> 5); // + 0x~.03x
}

u64 build_event(u64 type, u32 params) {
    return ((type & T_MASK) << PARAM_BITS) | (params & PARAM_MASK);
}

