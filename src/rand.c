#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
// probably???
// this feels wrong
#include "rand.h"

// not a bad idea to have a typedef of like Rand -> uint64_t*

// To start this off, we pass in a seed and return...
// a pointer to an int64
// not the int64 itself
// because if two completely separate consumers of random trigger next and get different int64s, it would be weird to reconsolidate
// the pointer never changes, we just call next() and update the number at it
// there fore:

uint64_t* rand_init(uint64_t seed) {
    //printf("rand_init invoked\n");

    // yes this is 8 bytes but hey
    uint64_t* rand = malloc(1*sizeof(uint64_t));

    *rand = seed;


    return rand;
}

// also we need a free method obv

// is it even worth returning somethign?
// the caller will have a pointer to rand anyways

// option 1
// uint64_t value = next(rand);
// operate using value...
// option 2
// next(rand)
// operate using *rand
//i like option 2 as it avoids a stack? heap? allocation
// the one that isn't malloc/free
void rand_next(uint64_t* rand) {
    *rand ^= *rand << 13;
    *rand ^= *rand >> 7;
    *rand ^= *rand << 17;
    //return *rand;
}

// whats the convention for this?
void rand_free(uint64_t* rand) {
    free(rand);
}
