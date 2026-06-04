#include <stdio.h>
#include <stdint.h>

int main(int argc, char* argv[]){
    // first thigns first, random generator
    // there are better methods but we're going to go with xorshift64
    // only use bottom 32 bits though

    // 42 too obvious
    // shout out NH
    uint64_t seed = 603;

    // randomness will be used to simulate network jitter, emotional trading decisions, etc
    uint64_t one = 1;
    // this is 32 ones
    uint64_t bottom = (one << 32) - 1;

    for (int i = 0; i < 100; i++){
        //printf("%llu\n", seed);
        //printf("%llu\n", (seed&bottom));

        // max value of this is "bottom"
        // scaling this to 0..1 means
        float scaled = (seed&bottom)/((float)bottom);
        printf("%f\n", scaled);

        // very random
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;

        
        // bottom 32?
        //seed & (1 << 31);

        // u is unsigned, d is int I think
        // %u is unsigned... int?
        // now these could be different
    }


    

    return 0;
}
