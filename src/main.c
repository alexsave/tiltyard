#include <stdio.h>
#include <stdint.h>
#include "rand.h"

int main(int argc, char* argv[]){
    // first thigns first, random generator
    // there are better methods but we're going to go with xorshift64
    // only use bottom 32 bits though

    // 42 too obvious
    // shout out NH
    uint64_t seed = 603;

    uint64_t* rand = rand_init(seed);
    printf("%llu\n", *rand);
    
    
    printf("testing random\n");

    uint64_t one = 1;
    uint64_t bottom = (one << 32) - 1;


    for (int i = 0; i < 100; i++){
        float scaled = (*rand&bottom)/((float)bottom);
        printf("%f\n", scaled);
        rand_next(rand);
    }


    rand_free(rand);
    return 0;
}
