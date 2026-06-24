#ifndef UTILS_H
#define UTILS_H

#include "types.h" 
#include "client_settings.h" 

u64 calculate_jitter(ClientSettings* cs, u64* rand);
u64 build_event(u64 type, u32 params);

#endif

