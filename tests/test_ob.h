#ifndef TEST_OB_H
#define TEST_OB_H

#include "ob.h"
#include "order.h"

void test_ob() {
    void* mbo_address = 0;
    BS* mbo_bs = bs_init(1000);
    u32 mbo_handle = bs_reserve(mbo_bs, sizeof(MBO), 10, &mbo_address);
    ((MBO*)mbo_address)->level_count = 0;

    mbo_address = bs_get(mbo_bs, 0);

    FL* orders = fl_init(4, 9128);


    Order p = { 
        .flags = (0 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9900,
        .client_id = 0 };
    Order* in = &p;
    u32 order_id = fl_insert(orders, &p);
    ob_limit(in, order_id, orders, mbo_handle, mbo_bs);
    mbo_address = bs_get(mbo_bs, 1);
    mbo_dump(mbo_address);

    Order p2 = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9900,
        .client_id = 0 };
    in = &p2;
    order_id = fl_insert(orders, &p2);
    ob_limit(in, order_id, orders, 1, mbo_bs);
    mbo_address = bs_get(mbo_bs, 2);
    mbo_dump(mbo_address);

    bs_free(mbo_bs);
}

#endif

