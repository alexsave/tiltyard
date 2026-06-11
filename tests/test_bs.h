#ifndef TEST_BS_H
#define TEST_BS_H

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "bs.h"

void first_tests() {
    // full metadata
    BS* bs = bs_init(1024);

    void* first_store = bs->store;

    void* bs_address = 0;

    uint32_t num = 0;
    num = bs_reserve(bs, 130000, 1, &bs_address);

    assert(num == 0);
    assert(bs->md_start == 0);
    assert(bs->md_end == 1);

    assert(bs->md_capacity == 1024);
    assert(bs->store_capacity == 131072);
    assert(bs->store != first_store);
    assert(bs_address == bs->store);

    assert(bs->metadata[0].refs == 1);
    assert(bs->metadata[0].size == 130000);
    assert(bs->metadata[0].offset == 0);

    bs_free(bs);
}

void overflow_tests() {
    printf("overflow\n");
    // full metadata
    BS* bs = bs_init(1024);

    void* bs_address = 0;

    uint32_t num = 0;

    for (int i = 0; i < 1024; i++) {
        num = bs_reserve(bs, 4, 1, &bs_address);
    }

    assert(num == 1023);
    assert(bs->md_start == 0);
    assert(bs->md_end == 0);

    printf("start and end at 0\n");

    void* zero_block = bs_get(bs, 0);
    assert(zero_block == bs->store);

    assert(bs->md_start == 1);
    assert(bs->md_end == 0);

    printf("start at 1, end at 0\n");

    num = bs_reserve(bs, 4, 1, &bs_address);
    printf("%d\n", num);
    assert(num == 0);

    assert(bs->md_start == 1);
    assert(bs->md_end == 1);

    // we don't double md anymore
    num = bs_reserve(bs, 4, 1, &bs_address);
    assert(num == INITIAL_METADATA_INDEX);
    assert(bs->md_start == 1);
    assert(bs->md_end == 1);
    assert(bs->md_capacity == 1024);
    // store did not get reallocated
    assert(bs->store == zero_block);
    assert(bs->store_capacity == 8192);

    bs_free(bs);
    printf("overflow done\n");
}

void last_before_first_tests() {
    BS* bs = bs_init(1024);

    void* first_store = bs->store;
    
    void* bs_address = 0;

    uint32_t num = 0;

    for (int i = 0; i < 8; i++) {
        num = bs_reserve(bs, 1024, 1, &bs_address);
    }

    // [0][1][2][3][4][5][6][7]
    assert(bs->md_end == 8);
    assert(bs->metadata[bs->md_start].offset == 0);
    assert(bs->metadata[7].offset == 1024*7);


    for (int i = 0; i < 4; i++) {
        bs_get(bs, i);
        //num = bs_reserve(bs, 1024, 1, &bs_address);
    }

    // [][][][][4][5][6][7]
    assert(bs->md_start == 4);
    assert(bs->metadata[bs->md_start].offset == 4*1024);
    assert(bs->md_end == 8);

    num = bs_reserve(bs, 3072, 1, &bs_address);
    
    // [8][8][8][][4][5][6][7]
    assert(bs->md_start == 4);
    assert(bs->metadata[8].offset == 0);
    assert(bs->metadata[8].size == 3072);

    num = bs_reserve(bs, 3072, 1, &bs_address);

    // [4][5][6][7][8][8][8][9][9][9][]...

    assert(bs->store != first_store);
    assert(bs->md_start == 4);
    assert(bs->md_end == 10);
    assert(bs->metadata[4].offset == 0);
    assert(bs->metadata[4].size == 1024);
    assert(bs->metadata[8].size == 3072);
    assert(bs->metadata[9].size == 3072);

    assert(bs->metadata[8].offset == 4*1024);
    assert(bs->metadata[9].offset == 7*1024);
}

void last_after_first_tests(){
    //gonna copy a bit from the other test
    BS* bs = bs_init(1024);

    void* first_store = bs->store;
    
    void* bs_address = 0;

    uint32_t num = 0;

    for (int i = 0; i < 6; i++) {
        num = bs_reserve(bs, 1024, 1, &bs_address);
    }

    // [0][1][2][3][4][5][ ][ ]
    assert(bs->md_end == 6);
    assert(bs->metadata[bs->md_start].offset == 0);
    assert(bs->metadata[5].offset == 1024*5);

    bs_get(bs, 0);

    // we're going to test that last easiest case first, with space after last
    // [ ][1][2][3][4][5][ ][ ]

    num = bs_reserve(bs, 1024, 1, &bs_address);

    // [ ][1][2][3][4][5][6][ ]

    assert(bs->md_end == 7);
    assert(bs->metadata[bs->md_start].offset == 1024*1);
    assert(bs->metadata[6].offset == 1024*6);

    // let's run it
    //not convinced so
    // ok we're back, we're still testing the bs

    // ok that highlighted else branch is ready
    // next up is what if we have place before first, but not after last???

    bs_get(bs, 1);
    // [ ][ ][2][3][4][5][6][ ]

    // we have enough for 2048 BEFORE "2"

    num = bs_reserve(bs, 2048, 1, &bs_address);

    assert(bs->md_start == 2);
    assert(bs->metadata[bs->md_start].offset == 1024*2);
    assert(bs->metadata[2].offset == 1024*2);

    // [7][7][2][3][4][5][6][ ]

    // wtf is md end again lol
    assert(bs->md_start == 2);
    assert(bs->md_end == 8);
    assert(bs->metadata[bs->md_start].offset == 1024*2);
    assert(bs->metadata[7].offset == 0);
    assert(bs->metadata[7].size == 2048);

    // if we try to insert one more time...
    // actually curious what will happen
    // we hit that first if - if branch and double capacity, reorganizing the whole thing
    // but we already test this in the other method
    // lets run - looks good

    // remove that 7 and run the third case
    //oh wait
    // this is kinda of a strange case because this whoel thing relies on the "7" block generally not being freed before "2,3,4,5,6" blocks are freed
    // because the snapshots are sequential
    // if we "free 7", the md_end won't go back to 6
    // it will only advance when we add a new one
    // little design descision
    // so le'ts insert one more for fun and watch it double

    num = bs_reserve(bs, 1024, 1, &bs_address);
    // [2][3][4][5][6][7][7][8][.....]
    assert(bs->md_start == 2);
    assert(bs->md_end == 9);
    assert(bs->metadata[2].offset == 0);
    assert(bs->metadata[2].size == 1024);
    assert(bs->metadata[3].offset == 1024);
    assert(bs->metadata[7].offset == 5*1024);
    assert(bs->metadata[7].size == 2048);
    assert(bs->metadata[8].offset == 7*1024);
    assert(bs->metadata[8].size == 1024);
    assert(bs->store_capacity == 8192*2);

    // let's run
    // dead ass?

    // ok now we can set up a case for that last case

    bs_get(bs, 2);
    // [ ][3][4][5][6][7][7][8][.....]
    assert(bs->metadata[bs->md_start].offset == 1024);

    // now if we request a large enough one, there won't be room at the END or START
    // we are now at store capacity 16x1024
    // first one is empty, then 7 more reserved
    // requesting a size 9x1024 should be enough 
    // or even 8x1024+1

    // actually to make it more fun let's force it to double twice, to 64
    // doubling to 32 will allow us to fit... 24x1024
    // so 25x1024 is enough

    num = bs_reserve(bs, 25*1024, 1, &bs_address);
    
    // now we will we a different doubling behavior
    // offsets wont change, but store capacity will quadruple 

    // [ ][3][4][5][6][7][7][8][9............9]
    assert(bs->md_start == 3);
    assert(bs->md_end == 10);
    assert(bs->metadata[bs->md_start].offset == 1024);
    assert(bs->metadata[9].offset == 8192);
    assert(bs->metadata[9].size == 25*1024);
    assert(bs->store_capacity == 64*1024);
        
}


void test_bs() {
    overflow_tests();

    first_tests();

    last_before_first_tests();

    last_after_first_tests();

    /*printf("testing blobstore\n");

    BS* bs = bs_init(1024);

    // ...

    
    // last parameter is kinda funny as we need to modify it
    uint32_t bs_number = bs_reserve(bs, size, refs, &bs_address);

    // server creates snapshot
    bs_address[0] = 42;

    // client readds it
    BSM* bsm = 

    // we get start, but it's kinda up to the parser of the snapshots to ensure we don't read past end
    void* start = bs_get(bs, bs_number);

    printf("%d\n", start[0]);


    bs_free();*/
}

#endif
