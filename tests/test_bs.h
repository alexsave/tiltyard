#ifndef TEST_BS_H
#define TEST_BS_H

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "bs.h"

void test_fl() {

    printf("testing blobstore\n");

    BS* bs = bs_init();

    // ...

    void* bs_address = 0;
    
    // last parameter is kinda funny as we need to modify it
    uint32_t bs_number = bs_reserve(bs, size, refs, bs_address);

    // server creates snapshot
    bs_address[0] = 42;

    // client readds it
    BSM* bsm = 

    // we get start, but it's kinda up to the parser of the snapshots to ensure we don't read past end
    void* start = bs_get(bs, bs_number);

    printf("%d\n", start[0]);


    bs_free();
}

#endif
