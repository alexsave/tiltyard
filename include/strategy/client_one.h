#ifndef CLIENT_ONE_H
#define CLIENT_ONE_H

// this will eventually be C's equivalent of an interface but for now-


typedef struct CO {
    uint64_t idk;
} CO;

CO* co_init();

uint64_t co_initial_boot_time(CO* co);
uint64_t co_postboot_socket(CO* co);
uint64_t co_base_latency(CO* co);

void co_free(CO* co);

/*struct Interface co_get_interface() {
    return [&co_init, &co_initial_boot_time, ...]
}

void* co = (*co_get_interface[0])()
(*co_get_interface[1])(co, adfsdfds)
(*co_get_interface[2])(co, adfsdfds)*/

#endif
