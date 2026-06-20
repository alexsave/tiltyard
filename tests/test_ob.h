#ifndef TEST_OB_H
#define TEST_OB_H

#include "ob.h"
#include "order.h"
#include "constants.h"

void test_ob() {
    void* mbo_address = 0;
    BS* mbo_bs = bs_init(1000);
    u32 mbo_handle = bs_reserve(mbo_bs, sizeof(MBO), 10, &mbo_address);
    ((MBO*)mbo_address)->level_count = 0;

    mbo_address = bs_get(mbo_bs, 0);

    FL* orders = fl_init(sizeof(Order), 9128);

    mbo_handle = 0;
    Order p = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9900,
        .client_id = 0 };
    u32 order_id = fl_insert(orders, &p);
    ob_limit(order_id, orders, mbo_handle, mbo_bs);
    mbo_handle++;
    mbo_address = bs_get(mbo_bs, mbo_handle);
    mbo_dump(mbo_address);


    Order p2 = { 
        .flags = (0 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9900,
        .client_id = 0 };
    order_id = fl_insert(orders, &p2);
    ob_limit(order_id, orders, mbo_handle, mbo_bs);
    mbo_handle++;
    mbo_address = bs_get(mbo_bs, mbo_handle);
    mbo_dump(mbo_address);

    assert(((MBO*)(mbo_address))->level_count == 0);
    assert(((MBO*)(mbo_address))->hi_bid_index == MAX_U8);

    Order p3 = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9900,
        .client_id = 0 };
    order_id = fl_insert(orders, &p3);
    ob_limit(order_id, orders, mbo_handle, mbo_bs);
    mbo_handle++;
    mbo_address = bs_get(mbo_bs, mbo_handle);
    mbo_dump(mbo_address);

    Order p4 = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9800,
        .client_id = 0 };
    order_id = fl_insert(orders, &p4);
    ob_limit(order_id, orders, mbo_handle, mbo_bs);
    mbo_handle++;
    mbo_address = bs_get(mbo_bs, mbo_handle);
    mbo_dump(mbo_address);
    


    bs_free(mbo_bs);
    fl_free(orders);
}

#endif

