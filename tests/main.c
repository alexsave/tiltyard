
// root test file
// will call other tests

#include <stdio.h>
#include <stdint.h>
#include "rand.h"

void test_random() {
    // call into random
    // now we're at the same problem 
    // in that idk how to incldue stuff

    uint64_t seed = 603;

    uint64_t* rand = rand_init(seed);
    printf("%llu\n", *rand);
    rand_next(rand);
    printf("%llu\n", *rand);
    
    
    printf("testing random\n");

    rand_free(rand);
}

int main(int argc, char* argv[]){
    // Random testing section
    test_random();

    return 0;
}
