#ifndef CONSTANTS_H
#define CONSTANTS_H

#include "types.h"

static const u64 P_BITS = 39; 
static const u64 P_SPAN = (u64)1 << P_BITS;
static const u64 P_MASK = P_SPAN - 1;

static const u64 FULL_SIZE_BITS = sizeof(u64) * 8;

//"event"
static const u64 E_BITS = FULL_SIZE_BITS - P_BITS;

// 0000.1111
// or like inverse P mask?
static const u64 E_MASK = ((u64)1 << E_BITS) - 1;

// number of fast buckets
static const u8 BUCKET_BITS = 3;
static const u8 SCH_BUCKETS = 1 << BUCKET_BITS;
static const u8 BUCKET_MASK = SCH_BUCKETS - 1;

static const u64 S_TO_NS = 1000000000;
static const u64 MIN_TO_NS = S_TO_NS * 60;
static const u64 H_TO_NS = MIN_TO_NS * 60;
static const u64 DAY_TO_NS = H_TO_NS * 24;

//type bits
static const u8 T_BITS = 2;
static const u8 T_MASK = (1 << T_BITS) - 1;

static const uint32_t PARAM_BITS = FULL_SIZE_BITS - P_BITS - T_BITS;
static const u32 MAX_PARAM = (1 << PARAM_BITS) - 1;
static const u32 PARAM_MASK = MAX_PARAM;

static const u64 SERVER_TYPE = 0;
static const u64 CLIENT_IN_TYPE = 1;
static const u64 CLIENT_OUT_TYPE = 2;
// client bump, slow scheduler check, EOD, EOM, kill sim
static const u64 CONTROL_TYPE = 3;

// yes i know that these go from the top while the special packets go from teh bototm
// honestly special packets should probably go from the top too.
// lets see if fl can quickly accomodate this
static const u32 CONTROL_PARAM_KILL = MAX_PARAM - 0;
static const u32 CONTROL_PARAM_EOM = MAX_PARAM - 1;
static const u32 CONTROL_PARAM_EOD = MAX_PARAM - 2;
static const u32 CONTROL_PARAM_SLOW = MAX_PARAM - 3;
static const u32 CONTROL_PARAM_NEWS = MAX_PARAM - 4;
static const u32 CONTROL_PARAM_OPEN = MAX_PARAM - 5;
static const u32 CONTROL_PARAM_CLOSE = MAX_PARAM - 6;
static const u32 MIN_CONTROL_PARAM = CONTROL_PARAM_CLOSE;

// 9:30 to 4pm eastern with no dst, so 14:30 to 21:00 utc. t=0 is midnight utc 1/1/1970,
// which makes the first bell 14.5h in. a session runs 6.5h, the overnight gap 17.5h
static const u64 FIRST_OPEN_NS = 14 * H_TO_NS + 30 * MIN_TO_NS;
static const u64 OPEN_TO_CLOSE_NS = 6 * H_TO_NS + 30 * MIN_TO_NS;
static const u64 CLOSE_TO_OPEN_NS = 17 * H_TO_NS + 30 * MIN_TO_NS;

// 1/1/1970 is a thursday, so day % 7 == 1 is a friday - that close also jumps the weekend
static const u64 FRIDAY_MOD = 1;
static const u64 WEEKEND_NS = 2 * DAY_TO_NS;


static const u32 MAX_U32 = 4294967295;
static const u16 MAX_U16 = 65535;
static const u8 MAX_U8 = 255;

// server param types
// the ordering of these actually matters for tie breakers
static const u32 HW_TO_SW_ID = MAX_PARAM - 0;
static const u32 EXEC_START_ID = MAX_PARAM - 1;
static const u32 EXEC_END_ID = MAX_PARAM - 2;
static const u32 EXEC_TO_SW_ID = MAX_PARAM - 3;
static const u32 MIN_RESERVED_PACKET = EXEC_TO_SW_ID;



#endif

