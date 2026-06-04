#ifndef RANDOM_H
#define RANDOM_H

// rand prefix cuz these names are too generic
uint64_t* rand_init(uint64_t seed);

void rand_next(uint64_t* rand);

void rand_free(uint64_t* rand);

#endif
