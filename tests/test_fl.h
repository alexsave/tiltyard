#ifndef TEST_FL_H
#define TEST_FL_H

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "fl.h"

void test_fl() {

    FL* fl = fl_init(4);

    uint64_t idk = ((((1*256)+2)*256+3)*256)+4;

    void* ref = &idk;

    uint32_t id = fl_insert(fl, ref);
    assert(id == 8191);

    printf("testing fl\n");

    assert(fl->data[id*4 + 0] == 4);
    assert(fl->data[id*4 + 1] == 3);
    assert(fl->data[id*4 + 2] == 2);
    assert(fl->data[id*4 + 3] == 1);

    *(uint8_t*)(ref+3) = 5;

    uint32_t id2 = fl_insert(fl, ref);
    assert(id2 == 8190);

    assert(fl->data[id2*4 +3]== 5);

    fl_release(fl, id2);
    fl_release(fl, id);

    uint32_t id3 = fl_insert(fl, ref);
    
    assert(id3 == 8191);

    assert(fl->data[id3*4 + 3] == 5);

    for (int i = 0; i < 8192 - 1; i++){
        id = fl_insert(fl, ref);
    }

    assert(fl->capacity == 8192);
    id = fl_insert(fl, ref);
    assert(fl->capacity == 8192*2);


    fl_free(fl);

}

#endif
