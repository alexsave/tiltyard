#ifndef TEST_CB_H
#define TEST_CB_H

#include <assert.h>
#include <stdio.h>

#include "types.h"
#include "cb.h"

void test_cb(){
    CB* cb = cb_init();

    u32* first_buffer = cb->buffer;

    cb_queue(cb, 50000);

    assert(cb->start == 0);
    assert(cb->end == 1);
    assert(cb->buffer == first_buffer);

    for (int i = 0; i < 1023; i++) {
        cb_queue(cb, i);
    }

    assert(cb->start == 0);
    assert(cb->end == 0);
    assert(cb->buffer == first_buffer);

    u32 get = cb_deque(cb);

    assert(get == 50000);

    assert(cb->start == 1);
    assert(cb->end == 0);
    assert(cb->buffer == first_buffer);

    cb_queue(cb, 12345678);

    assert(cb->start == 1);
    assert(cb->end == 1);
    assert(cb->buffer == first_buffer);

    cb_queue(cb, 0);

    assert(cb->start == 0);
    assert(cb->end == CB_INITIAL_CAPACITY + 1);
    assert(cb->buffer != first_buffer);

    cb_free(cb);
}

#endif

