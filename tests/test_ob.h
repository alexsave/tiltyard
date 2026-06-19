#ifndef TEST_OB_H
#define TEST_OB_H

#include "ob.h"
#include "order.h"

void test_ob() {
    void* mbo_address = 0;
    BS* mbo_bs = bs_init(1000);
    u32 mbo_handle = bs_reserve(mbo_bs, sizeof(MBO), 1, &mbo_address);
    ((MBO*)mbo_address)->level_count = 0;

    Order p = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9900,
        .client_id = 0 };

    Order* in = &p;

    FL* orders = fl_init(4, 9128);

    ob_limit(in, 123, orders, mbo_handle, mbo_bs);

}

#endif

