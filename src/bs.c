#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "bs.h"

// variable size handled blob store

// initial state for metadata, start at -1, end at 0
// start at 0, end at 0 means that the circular buffer is full 100%

BS* bs_init() {

    bs->md_start = -1;
    bs->md_end = 0;
    bs->md_capacity = 1024;
    
}

// address_holder will be messed up if we need to double the store size
// users must write to the address before calling bs_reserve again
uint32_t bs_reserve(BS* bs, uint32_t size, uint32_t refs, && address_holder){
    if (bs->md_start == bs->md_end) {
        // need to double metadata array capacity
        return 0; 
    }


    // in the case that we only have one snapshot in, then first->offset = last->offset
    // and we handle it equivalently to first < last case

    // if we have zero, we need to explicitly handle it as -1 is not an index

    if (bs->md_start == -1 && bs->md_end == 0) {
        if (bs->store_capacity < size) {
            // double until it fits
            // easier but rare case - we don't need to copy, we just free and malloc
        } 

        // put at 0
        bs->md_start = bs->md_start + 1;

        bs->metadata[bs->md_end]->refs = refs;
        bs->metadata[bs->md_end]->offset = 0;
        bs->metadata[bs->md_end]->size = size;

        // just to be consistent
        bs->md_end = (bs->md_end + 1) % bs->md_capacity;
        
        return;
    }

    uint32_t last_md_index = (bs->md_end + bs->md_capacity - 1) % bs->md_capacity;
    
    BSM* first = bs->metadata[bs->md_start];
    BSM* last = bs->metadata[last_md_index];
    
    
    if (first->offset > last->offset){
        // |   [last ---- last+size] [size ---- size] ... [first --- first+size]
        if (first->offset < last->offset + last->size + size){

            // we need doubling code
            // double capacity
            // make an array at the new capacity
            // take each blob from start to last, 
            // copy it to new array and update offset in the metadata
            // free old array


            // try again - check between end and capacity

            //return -1; // we need to double store
        } else {
            // put it after last
            bs->metadata[bs->md_end]->refs = refs;
            bs->metadata[bs->md_end]->offset = last->offset + last->size;
            bs->metadata[bs->md_end]->size = size;

            bs->md_end = (bs->md_end + 1) % bs->md_capacity;
        }
    } else if (first->offset <= last->offset) {
        // | ...  [first ---- first+size] [] [][] [last --- last+size] ...   |

        if (bs->store_capacity - (last->offset + last->size) < size) {
            if (first->offset < size) {
                // we need doubling code
                // double capacity
                // make an array at the new capacity
                // take each blob from start to last, 
                // copy it to new array and update offset in the metadata
                // free old array


                // try again - check between end and capacity
            } else {
                // put it at 0
                bs->metadata[bs->md_end]->refs = refs;
                bs->metadata[bs->md_end]->offset = 0;
                bs->metadata[bs->md_end]->size = size;

                bs->md_end = (bs->md_end + 1) % bs->md_capacity;
            }
        } else {
            // put it after last
            bs->metadata[bs->md_end]->refs = refs;
            bs->metadata[bs->md_end]->offset = last->offset + last->size;
            bs->metadata[bs->md_end]->size = size;

            bs->md_end = (bs->md_end + 1) % bs->md_capacity;
        }
    }




}

// because we stop managing this block in the metadata, 
// nothing can happen until client is set with the returned data
void* bs_get(BS* bs, uint32_t bs_number) {
    // all snapshots freed, nothing managed
    if (bs->md_start == -1 && bs->md_end == 0)
        return 0;

    BSM* bsm = bs->metadata[bs_number];
    bsm->refs = bsm->refs--;

    if (bs_number == bs->md_start) {
        while (bs->metadata[bs->md_start]->refs == 0) {
            bs->md_capacity
                bs->md_start = (bs->md_start + 1) % bs->md_capacity;

            if (bs->md_start == bs->md_end) {
                // all snapshots wiped without any new ones
                // reset to this empty state, do not leave both some nonzero value
                bs->md_start = -1;
                bs->md_end = 0;
            }
        }
    }

    // i have no idea but somethign like this
    return (void *) (bs->store + bsm->offset);
}



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

void bs_free(BS* bs) {
    free(bs->metadata);
    free(bs->store);
    free(bs);
}


