#ifndef TEST_OB_H
#define TEST_OB_H

#include "ob.h"
#include "order.h"
#include "constants.h"

void test_bugfinder() {
    void* mbo_address = 0;
    BS* mbo_bs = bs_init(1000);
    u32 mbo_handle = bs_reserve(mbo_bs, sizeof(MBO), 2, &mbo_address);
    ((MBO*)mbo_address)->level_count = 0;
    ((MBO*)mbo_address)->hi_bid_index = MAX_U8;

    mbo_address = bs_get(mbo_bs, 0);

    FL* orders = fl_init(sizeof(Order), 9128);

    //order info id 3 buy? 0 quantity 57 price 18404 from client id #1
    //order info id 5 buy? 0 quantity 46 price 61998 from client id #0

    Order p3 = { 
        .flags = (0 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 57,
        .price = 18404,
        .client_id = 0 };
    mbo_handle = ob_limit(fl_insert(orders, &p3), orders, mbo_handle, mbo_bs, 2);
    printf("bsget addr #1 %p\n", bs_get_no_ref(mbo_bs, mbo_handle));
    void* ptr = bs_get(mbo_bs, mbo_handle);
    printf("bsget addr #2 %p\n", ptr);
    mbo_dump(ptr);

    Order p5 = { 
        .flags = (0 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 46,
        .price = 61998,
        .client_id = 0 };
    mbo_handle = ob_limit(fl_insert(orders, &p5), orders, mbo_handle, mbo_bs,1);
    printf("bsget p5 addr #1 %p\n", bs_get_no_ref(mbo_bs, mbo_handle));
    ptr = bs_get(mbo_bs, mbo_handle);
    printf("bsget p5 addr #2 %p\n", ptr);
    mbo_dump(ptr);


    bs_free(mbo_bs);
    fl_free(orders);
}

void test_fuzz(){
    printf("\n\n\n");
    void* mbo_address = 0;
    BS* mbo_bs = bs_init(1000);
    u32 mbo_handle = bs_reserve(mbo_bs, sizeof(MBO), 10, &mbo_address);
    ((MBO*)mbo_address)->level_count = 0;
    ((MBO*)mbo_address)->hi_bid_index = MAX_U8;

    mbo_address = bs_get(mbo_bs, 0);

    FL* orders = fl_init(sizeof(Order), 9128);

    Order p1 = { 
        .flags = (0 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 26,
        .price = 44138,
        .client_id = 0 };
    ob_limit(fl_insert(orders, &p1), orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));

    Order p2 = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 203,
        .price = 49867,
        .client_id = 0 };
    ob_limit(fl_insert(orders, &p2), orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));

    bs_free(mbo_bs);
    fl_free(orders);

}

void test_ob() {
    test_bugfinder();
    if(1) return;

    void* mbo_address = 0;
    BS* mbo_bs = bs_init(1000);
    u32 mbo_handle = bs_reserve(mbo_bs, sizeof(MBO), 10, &mbo_address);
    ((MBO*)mbo_address)->level_count = 0;
    ((MBO*)mbo_address)->hi_bid_index = MAX_U8;

    mbo_address = bs_get(mbo_bs, 0);

    FL* orders = fl_init(sizeof(Order), 9128);

    mbo_handle = 0;
    Order p = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9900,
        .client_id = 0 };
    u32 order_id = fl_insert(orders, &p);
    ob_limit(order_id, orders, mbo_handle, mbo_bs,2);
    mbo_handle++;
    mbo_address = bs_get(mbo_bs, mbo_handle);
    mbo_dump(mbo_address);


    Order p2 = { 
        .flags = (0 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9900,
        .client_id = 0 };
    order_id = fl_insert(orders, &p2);
    ob_limit(order_id, orders, mbo_handle, mbo_bs,2);
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
    ob_limit(order_id, orders, mbo_handle, mbo_bs,2);
    mbo_handle++;
    mbo_address = bs_get(mbo_bs, mbo_handle);
    mbo_dump(mbo_address);

    Order p4 = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9800,
        .client_id = 0 };
    order_id = fl_insert(orders, &p4);
    ob_limit(order_id, orders, mbo_handle, mbo_bs,2);
    mbo_handle++;
    mbo_address = bs_get(mbo_bs, mbo_handle);
    mbo_dump(mbo_address);

    Order p5 = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9700,
        .client_id = 0 };
    order_id = fl_insert(orders, &p5);
    ob_limit(order_id, orders, mbo_handle, mbo_bs,2);
    mbo_handle++;
    mbo_address = bs_get(mbo_bs, mbo_handle);
    mbo_dump(mbo_address);

    Order p6 = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9700,
        .client_id = 0 };
    order_id = fl_insert(orders, &p6);
    ob_limit(order_id, orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));

    Order p7 = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 9750,
        .client_id = 0 };
    order_id = fl_insert(orders, &p7);
    ob_limit(order_id, orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));


    Order p8 = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 10000,
        .client_id = 0 };
    ob_limit(fl_insert(orders, &p8), orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));

    assert(((MBO*)((bs_get(mbo_bs, mbo_handle))))->level_count == 5);
    assert(((MBO*)((bs_get(mbo_bs, mbo_handle))))->hi_bid_index == 4);

    Order p9 = { 
        .flags = (0 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 10200,
        .client_id = 0 };
    ob_limit(fl_insert(orders, &p9), orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));

    Order p10 = { 
        .flags = (0 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 2,
        .price = 10100,
        .client_id = 0 };
    ob_limit(fl_insert(orders, &p10), orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));


    // test the market matching part

    Order p11 = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 10150,
        .client_id = 0 };
    ob_limit(fl_insert(orders, &p11), orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));

    assert(((MBO*)((bs_get(mbo_bs, mbo_handle))))->level_count == 7);
    assert(((MBO*)((bs_get(mbo_bs, mbo_handle))))->hi_bid_index == 4);

    Order p12 = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 10100,
        .client_id = 0 };
    ob_limit(fl_insert(orders, &p12), orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));

    Order p13 = { 
        .flags = (0 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 10000,
        .client_id = 0 };
    ob_limit(fl_insert(orders, &p13), orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));

    Order p14 = { 
        .flags = (0 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 2,
        .price = 9700,
        .client_id = 0 };
    ob_limit(fl_insert(orders, &p14), orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));

    Order p15 = { 
        .flags = (0 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 2,
        .price = 9725,
        .client_id = 0 };
    ob_limit(fl_insert(orders, &p15), orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));

    Order p16 = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 2,
        .price = 10000,
        .client_id = 0 };
    ob_limit(fl_insert(orders, &p16), orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));

    Order p17 = { 
        .flags = (1 << BUY_DIRECTION_BIT) | (1 << IS_LIMIT_BIT),
        .quantity = 1,
        .price = 10200,
        .client_id = 0 };
    ob_limit(fl_insert(orders, &p17), orders, mbo_handle++, mbo_bs,2);
    mbo_dump(bs_get(mbo_bs, mbo_handle));

    bs_free(mbo_bs);
    fl_free(orders);

    test_fuzz();
}

#endif

