#include <stdint.h>

// fixed sized free list

typedef struct Order {
    uint32_t quantity;
    uint32_t client_id;
    uint32_t price;
    uint32_t type;
} Order;

typedef struct BS {
    //uint32_t sp;
    //uint32_t* stack;
    //Order* data;
    //uint32_t capacity;
} BS;


FL* fl_init(uint32_t capacity);

uint32_t fl_insert(FL* fl, Order order);

void fl_release(FL* fl, uint32_t order_id);

void bs_free(BS* bs);

