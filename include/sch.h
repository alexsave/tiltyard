#include <stdint.h>

// take the more interesting case first
static const uint8_t P_BITS = 39;

//"event"
static const uint8_t E_BITS = 64 - P_BITS;

// 0000.1111
// or like inverse P mask?
static const uint64_t E_MASK = (1 << E_BITS) - 1;

// number of fast buckets
static const uint8_t BUCKET_BITS = 3;
static const uint8_t SCH_BUCKETS = 1 << BUCKET_BITS;


static const uint64_t S_TO_NS = 1000000000;

static const uint8_t SLOW_CHECK_TYPE = 7;
