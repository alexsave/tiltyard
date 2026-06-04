#include <stdint.h>
#include <stdlib.h>
#include "fl.h"

// fixed sized free list


FL* fl_init(uint32_t capacity) {
    FL* fl = malloc(1*sizeof(FL));

    Order* data = malloc(capacity * sizeof(Order));
    uint32_t* stack = malloc(capacity * sizeof(uint32_t));

    for (uint32_t i = 0; i < capacity; i++) {
        stack[i] = i;
    }

    fl->data = data;
    fl->stack = stack;
    fl->sp = capacity;
    fl->capacity = capacity;

    return fl;
}

// orderid is 32bits
uint32_t fl_insert(FL* fl, Order order) {

    if (fl->sp == 0)
        return 0;// error, figure it out

    fl->sp = fl->sp - 1;

    uint32_t order_id = fl->stack[fl->sp];

    fl->data[order_id] = order;


    return order_id;
}

void fl_release(FL* fl, uint32_t order_id) {
    // shouldn't happen but prevents extra releases
    if (fl->sp == fl->capacity)
        return;

    fl->stack[fl->sp] = order_id;
    fl->sp = fl->sp + 1;
}

void fl_free(FL* fl) {
    free(fl->data);
    free(fl->stack);
    free(fl);
}
