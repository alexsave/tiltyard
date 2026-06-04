#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "pq.h"

// min heap priority queue


// to kick this thing off, we need return a pointer to something

// we need a struct first actually
// something like... 
// int capacity, int current_index, int* heap
// ok
// that's not big just three x 64bit
// capacity and current_index isn't majorly consequential, but heap data type is
// I already know I need to do uint64_t
// now how do we make a struct lol

PQ* pq_init(uint32_t capacity) {
    uint64_t* heap = malloc(capacity * sizeof(uint64_t));

    PQ* pq = malloc(1 * sizeof(PQ));
    //pq->capacity = capacity;

    pq->current = 1; // possibly one for easier math later
    pq->heap = heap;
    pq->heap[0] = capacity;

    return pq;
}

// ok what do

// these "events" are actually going to encode entire market events
// either top or bottom 32 bits will be priority itself
// leaning towards top to keep this math rn simple
void pq_push(PQ* pq, uint64_t event) {
    // ok how do you push onto a priority queue
    // if you try to push at the front, you'd need to shift ALL scheduled events
    // but maybe we push to the back?

    // yes exactly
    // let me consider this
    // if you have 0 based, then 0 has children indicies 1 & 2

    // to go from 1 -> and 2 -> in a single operation... you can do 
    // (1-1)>>1, or (2-1)>>1
    // I swear I wrote this down
    // ok fair enough
    // but watch this
    // if it's 1 based
    // 1 has children 2 and 3
    // to go from 2->1 and 3->1, you just do >>1
    // every operation counts here
    // honestly we could even shove capacity into the "zero index" to really have fun
    //  yes

    uint32_t run = pq->current;
    //ok let's run through this
    // at the start, current = 1
    //pq->heap[pq->current] = event;


    // then try to move it down
    // actually keeping "0" in the 0 index is pretty clean as it's always lower 
    //lol infinite
    
    while (1) {
        // if we decide that we need to compare against 1, then it becomes the root

        // swap ok i actually do have notes on this

        uint32_t next = run >> 1; //parent
        if (run == 1 || pq->heap[run >> 1] <= event) { //equals very very unlikely you'll see why 
// what a mess
            // notes say "then you write d to d index"
            pq->heap[run] = event;
            break;
        } else {
            // write the parent to the "current"
            pq->heap[run] = pq->heap[run >> 1];
            run = run >> 1;
        }
    }

    // finally
    pq->current = pq->current + 1;
}

// ez one
uint64_t pq_peek(PQ* pq){
    return pq->heap[1];
}

uint64_t pq_pop(PQ* pq) {
    // quick check
    if (pq->current == 1)
        //idk
        return 0;

    //first
    pq->current = pq->current - 1;

    uint64_t event = pq->heap[1];  

    //...
    // ok how do we avoid shifting
    // take the head and replace it with 0. then that zero needs to be shifted to the end of hte stack

    // go through heap and swap with smaller value
    uint32_t run = 1;
    // this one needs to be hard coded

    uint64_t last_copy = pq->heap[pq->current];
    //printf("pop called, last copy: %llu\n", last_copy);
    // doesn't this song sample something btw?

    //hp->heap[run] = hp->heap[hp->current] this happens in the loop
    pq->heap[pq->current] = 0;
    //swap erase, come back as this isn't even necessary
    
    // then place it

    while(1){
        // 1 goes to 2 & 3
        // 2 goes to 2<<1 & 2<<1 +1, 4 and 5
        // dont swap left and right LMAO
        uint32_t left = (run << 1);
        uint32_t right = left + 1;

        if (right >= pq->current && left >= pq->current) {
            // gottem
            pq->heap[run] = last_copy;
            break;
        }

        if (right >= pq->current) {
            // only right out of bounds

            if (pq->heap[left] < last_copy) {
                // swap
                pq->heap[run] = pq->heap[left];
                pq->heap[left] = last_copy;
            } else {
                // we're still done anyways, as we have one child and its >= last_copy
            }

            break;
        }

        // normal case
        // it has to be the case that both the value at left & right is 
    
        // four cases
        // I think it's really just two, but I ned to test properly
        if (pq->heap[right] > last_copy && pq->heap[left] > last_copy) {
            // exactly where we need to be
            break;
        } else if (pq->heap[right] > last_copy && pq->heap[left] <= last_copy) {
            // impossible case? idk
            //left, copy, right ascending  -> move left
            pq->heap[run] = pq->heap[left];
            run = left;
        } else if (pq->heap[right] <= last_copy && pq->heap[left] > last_copy) {
            // impossible case? idk
            //rigth, copy, left ascending -> move right
            pq->heap[run] = pq->heap[right];
            run = right;
        } else if (pq->heap[right] <= last_copy && pq->heap[left] <= last_copy) {
            // very likely case
            // swap with lower one and update "run" properly
            if (pq->heap[right] <= pq->heap[left]) {
                // move right
                pq->heap[run] = pq->heap[right];
                run = right;
            } else {
                // move left
                pq->heap[run] = pq->heap[left];
                run = left;
            } // what if they're equal? unlikely, but we'll go right
        }




        // ok NOW we just need to place the 

        // two options
        // wait if the array is filled with 0 by default, 0 is less than all values so we need special check
        // unfortunate for minqueue
        // it could be filled with 2^64-1 instead?

        // example:
        // []
        // [,1]
        // [,1, 3]
        // [,1, 3, 4]
        // [,1, 2, 4, 3]
        //pop
        // [, F, 2, 4, 3]
        // [, 3, 2, 4]
        // [, 2, 3, 4]
        //pop
        // [, X, 3, 4]
        // [, 4, 3, X]
        // [, 3, 4,]

        // we then get [, 2, 3, 4, F]
        // F is actually 0 so it's [, 2, 3, 4]
        // next pop
        // [, F, 3, 4]
        // F compares against 3 & 4, swapping with 3
        // [, 3, F, 4]
        // F then compares against index 4 and 5, both of which are empty
        // but we cant have this hole between 3 and 4
        // i need to draw this out m
        // nah im actually missing something
        // i look like such a nerd
        // AH right you first swap the LAST element with the root



        //pq->heap[right]
            //pq->heap[left]

    }

    return event;
}



// not a bad idea to have a typedef of like Rand -> uint64_t*

// To start this off, we pass in a seed and return...
// a pointer to an int64
// not the int64 itself
// because if two completely separate consumers of random trigger next and get different int64s, it would be weird to reconsolidate
// the pointer never changes, we just call next() and update the number at it
// there fore:

//ez
void pq_free(PQ* pq) {
    free(pq->heap);
    free(pq);
}
