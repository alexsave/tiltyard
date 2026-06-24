#ifndef CONSTANTS_H
#define CONSTANTS_H

#include "types.h"

static const uint64_t P_BITS = 39; 
static const uint64_t P_SPAN = (uint64_t)1 << P_BITS;
static const uint64_t P_MASK = P_SPAN - 1;

static const uint64_t FULL_SIZE_BITS = sizeof(uint64_t) * 8;

//"event"
static const uint64_t E_BITS = FULL_SIZE_BITS - P_BITS;

// 0000.1111
// or like inverse P mask?
static const uint64_t E_MASK = ((uint64_t)1 << E_BITS) - 1;

// number of fast buckets
static const uint8_t BUCKET_BITS = 3;
static const uint8_t SCH_BUCKETS = 1 << BUCKET_BITS;
static const uint8_t BUCKET_MASK = SCH_BUCKETS - 1;


static const uint64_t S_TO_NS = 1000000000;

//type bits
static const uint8_t T_BITS = 2;
static const uint8_t T_MASK = (1 << T_BITS) - 1;

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
static const u32 MIN_CONTROL_PARAM = CONTROL_PARAM_SLOW;


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

