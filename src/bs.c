#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "bs.h"

#include "types.h"

// md_start is metadata start
// in the case that the metadata is empty, it will be -1
// then it will go up to 0, and increment as we "free" blocks
// md_end is the index of the next metadata slot
// we will write to it before incrementing it

// variable size handled blob store

// initial state for metadata, start at -1, end at 0
// start at 0, end at 0 means that the circular buffer is full 100%



BS* bs_init(uint16_t metadata_capacity) {
    BS* bs = malloc(sizeof(BS));

    bs->md_start = INITIAL_METADATA_INDEX;
    bs->md_end = 0;
    bs->md_capacity = metadata_capacity;

    bs->store_capacity = 8192;

    bs->store = malloc(bs->store_capacity);
    bs->metadata = malloc(bs->md_capacity * sizeof(BSM));
    
    return bs;
}

// address_holder will be messed up if we need to double the store size
// users must write to the address before calling bs_reserve again
// called like bs_reserve(..., &pointer)
// pointer will then be set to the actual memory spot
// pointer[0] = 42
// alternative option is to return the address, and modify a passed in int32
//size must be in bytes
// size is actually an upper bound
uint32_t bs_reserve(BS* bs, uint32_t size, uint32_t refs, void ** address_holder){
    if (bs->md_start == bs->md_end) {
        printf("md start is looped back to md end, double the capacity %u \n", bs->md_start);
        printf("last size is %u\n", bs->metadata[(bs->md_end-1+bs->md_capacity)%bs->md_capacity].size);
        exit(1);
        // there is no clean way to double the metadata capactiy
        // if we just ignore the current values of md_start and md_end, we lose good tracking on the store
        // if we try to move metdata 0 -- start to a new spot, we need to maintain two copies
        // so we just don't
        // be sure to size capacity of your BSM properly
        return INITIAL_METADATA_INDEX;
    }


    // in the case that we only have one snapshot in, then first->offset = last->offset
    // and we handle it equivalently to first < last case

    // if we have zero, we need to explicitly handle it as -1 is not an index

    if (bs->md_start == INITIAL_METADATA_INDEX/* && bs->md_end == 0*/) {
        //printf("md start %u, md end %u\n", bs->md_start, bs->md_end);
        if (bs->store_capacity < size) {
            printf("doubling time\n");
    
            // double until it fits
            // easier but rare case - we don't need to copy, we just free and malloc

            while (bs->store_capacity < size) {
                bs->store_capacity = bs->store_capacity << 1;
            }

            free(bs->store);
            bs->store = malloc(bs->store_capacity);
        } 

        // put at 0
        bs->md_start = 0;

        bs->metadata[bs->md_end].refs = refs;
        bs->metadata[bs->md_end].offset = 0;
        bs->metadata[bs->md_end].size = size;

        // just to be consistent
        bs->md_end = 1;//(bs->md_end + 1) % bs->md_capacity;

        *address_holder = &(bs->store[0]);
        //*address_holder = bs->store;???

        
        return 0;
    }

    uint32_t last_md_index = (bs->md_end + bs->md_capacity - 1) % bs->md_capacity;
    
    // do we want to dereference these into structs?
    BSM first = bs->metadata[bs->md_start];
    BSM last = bs->metadata[last_md_index];
    
    
    if (first.offset > last.offset){
        // |   [last ---- last+size] [size ---- size] ... [first --- first+size]
        printf("first offset %d, last offset %d, last size %d, size %d\n", first.offset, last.offset, last.size, size);
        if (first.offset < last.offset + last.size + size){
            printf("doubling time\n");

            bs->store_capacity = bs->store_capacity << 1;
            
            void* doubled = malloc(bs->store_capacity);

            uint32_t current_start = 0;

            for (uint32_t i = bs->md_start; i != bs->md_end; i = (i + 1) % bs->md_capacity){
                
                // copy it to new array and update offset in the metadata
                memcpy(&(doubled[current_start]), &(bs->store[bs->metadata[i].offset]), bs->metadata[i].size);

                printf("shifting block that was at %d to %d\n", bs->metadata[i].offset, current_start);
                bs->metadata[i].offset = current_start;
                current_start = current_start + bs->metadata[i].size;
        
            }

            free(bs->store);

            bs->store = doubled;

            // try again - check between end and capacity
            return bs_reserve(bs, size, refs, address_holder);

        } else {
            // put it after last
            bs->metadata[bs->md_end].refs = refs;
            bs->metadata[bs->md_end].offset = last.offset + last.size;
            bs->metadata[bs->md_end].size = size;

            *address_holder = &(bs->store[last.offset + last.size]);

            uint16_t result = bs->md_end;

            bs->md_end = (bs->md_end + 1) % bs->md_capacity;

            return result;
        }
    //} else if (first.offset <= last.offset) {
    } else {
        // | ...  [first ---- first+size] [] [][] [last --- last+size] ...   |

        // this else block needs to be tested

        if (bs->store_capacity - (last.offset + last.size) < size) {
            if (first.offset < size) {
                printf("doubling time\n");

                // it's possible that if we just shift it such that start offset is at 0, we can fit another block in
                // but we'll do the easier thing and just copy everything without modifying the metadata at all

                void* doubled = malloc(2 * (bs->store_capacity));

                memcpy(doubled, bs->store, bs->store_capacity);

                bs->store_capacity = bs->store_capacity << 1;

                free(bs->store);
                bs->store = doubled;

                return bs_reserve(bs, size, refs, address_holder);

                // try again - check between end and capacity
            } else {
                // put it at 0
                bs->metadata[bs->md_end].refs = refs;
                bs->metadata[bs->md_end].offset = 0;
                bs->metadata[bs->md_end].size = size;

                *address_holder = &(bs->store[0]);

                uint16_t result = bs->md_end;

                bs->md_end = (bs->md_end + 1) % bs->md_capacity;

                return result;
            }
        } else {
            // put it after last
            bs->metadata[bs->md_end].refs = refs;
            bs->metadata[bs->md_end].offset = last.offset + last.size;
            bs->metadata[bs->md_end].size = size;

            *address_holder = &(bs->store[last.offset + last.size]);

            uint16_t result = bs->md_end;

            bs->md_end = (bs->md_end + 1) % bs->md_capacity;

            return result;
        }
    }



    ///

}

// in the case that we were uncertain of the actual size during reserve
// we can use this to save some data
// critically this can only be done on the last pushed blob, so be sure to do it soon after
u8 bs_resize(BS* bs, u32 actual_size) {
    if (bs->md_start == INITIAL_METADATA_INDEX && bs->md_end == 0)
        return 0;

    u32 last_md_index = (bs->md_end + bs->md_capacity - 1) % bs->md_capacity;

    // is it even worth the possibility of pushing it back to match in case we loop through top?

    if (bs->metadata[last_md_index].size < actual_size) {
        printf("You just tried to resize a blob to a larger size. This is not supported\n");
        return 1;
    }
    bs->metadata[last_md_index].size = actual_size;
    return 0;
}

// because we stop managing this block in the metadata, 
// nothing can happen until client is set with the returned data
void* bs_get(BS* bs, uint32_t bs_number) {
    // all snapshots freed, nothing managed
    if (bs->md_start == INITIAL_METADATA_INDEX && bs->md_end == 0)
        return 0;

    //BSM bsm = bsm.refs = bsm.refs - 1;
    bs->metadata[bs_number].refs = bs->metadata[bs_number].refs - 1;

    if(bs_number == 0){
        //printf("bs number %d, md start %d, refs %u\n", bs_number, bs->md_start, bs->metadata[bs_number].refs);
        //exit(1);
    }
    if (bs_number == bs->md_start) {
        //printf("wiping out some chunks, bs number refs %d\n", bs->metadata[bs->md_start].refs);
        while (bs->metadata[bs->md_start].refs == 0) {
            //printf("%u wiped\n", bs->md_start);
            bs->md_start = (bs->md_start + 1) % bs->md_capacity;

            if (bs->md_start == bs->md_end) {
                // all snapshots wiped without any new ones
                // reset to this empty state, do not leave both some nonzero value
                bs->md_start = INITIAL_METADATA_INDEX;
                bs->md_end = 0;

                break;
            }
        }
    }

    // i have no idea but somethign like this
    return (void *) (bs->store + bs->metadata[bs_number].offset);
}

// sidedoor for server
void* bs_get_no_ref(BS* bs, u32 bs_number) {
    return (void *) (bs->store + bs->metadata[bs_number].offset);
}

void bs_bump_refs(BS* bs, uint32_t bs_number) {
    bs->metadata[bs_number].refs = bs->metadata[bs_number].refs + 1;
}


void bs_free(BS* bs) {
    free(bs->metadata);
    free(bs->store);
    free(bs);
}


