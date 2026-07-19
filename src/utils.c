#include "utils.h" 

#include "types.h" 
#include "constants.h" 
#include "client_settings.h" 
#include "rand.h" 

u64 calculate_jitter(ClientSettings* cs, u64* rand){
    rand_next(rand);
    u64 base_jitter = cs->net_latency;
    return 
        (base_jitter) + // 1.0x
        (base_jitter >> 7) + // + ~.01x
        (((base_jitter * (*(rand) & MAX_U32)) >> 32 ) >> 5); // + 0x~.03x
}

u64 build_event(u64 type, u32 params) {
    return ((type & T_MASK) << PARAM_BITS) | (params & PARAM_MASK);
}

// ns from now until the first instant of the next calendar month. t=0 is 1970-01-01, and with
// no leap years every year is 365 days, so we peel whole months off the day-of-year
u64 delay_to_next_month(u64 now_ns) {
    u64 day_index = now_ns / DAY_TO_NS;
    u64 day_of_month = day_index % DAYS_PER_YEAR; // day within the year, peeled down to the month
    u32 month = 0;
    while (day_of_month >= MONTH_DAYS[month]) {
        day_of_month -= MONTH_DAYS[month];
        month++;
    }
    u64 next_month_start = (day_index - day_of_month) + MONTH_DAYS[month];
    return next_month_start * DAY_TO_NS - now_ns;
}

