#include <stdint.h>

// fixed sized free list

typedef struct FL {
    uint32_t sp;
    uint32_t* stack;
    // technically liek "any" or void* data, but easier this way
    uint8_t* data;
    uint32_t capacity;
    uint8_t type_size;
} FL;

static const uint32_t FL_INITIAL_CAPACITY = 8192;

FL* fl_init(uint8_t type_size);

uint32_t fl_insert(FL* fl, void* data);

void* fl_release(FL* fl, uint32_t id);

void fl_free(FL* fl);

