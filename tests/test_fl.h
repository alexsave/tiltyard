#ifndef TEST_FL_H
#define TEST_FL_H

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "fl.h"

void test_fl() {

    FL* fl = fl_init(1024);

    Order order = {1, 2, 3, 4};

    uint32_t order_id = fl_insert(fl, order);

    Order saved_order = fl->data[order_id];

    printf("testing fl\n");

    assert(saved_order.quantity == 1);
    assert(saved_order.client_id == 2);
    assert(saved_order.price == 3);
    assert(saved_order.type == 4);

    order.type = 5;

    uint32_t order_id2 = fl_insert(fl, order);

    assert(fl->data[order_id2].type == 5);

    fl_release(fl, order_id2);
    fl_release(fl, order_id);

    uint32_t order_id3 = fl_insert(fl, order);
    
    assert(order_id3 == 1023);

    assert(fl->data[order_id3].type == 5);


    fl_free(fl);

}

#endif
