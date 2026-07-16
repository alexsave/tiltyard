#ifndef SERVER_H
#define SERVER_H

#include "types.h"
#include "client_settings.h"
#include "bs.h"
#include "fl.h"
#include "cb.h"
#include "sch.h"
#include "holder.h"

// this only works because HW_TO_SW_ID = 0 and is reserved
// come back to this lol
static const u32 CONVERT_SENTINEL_VALUE = 0;

// basically everything we were holding as locals in src/main
// bundled into a struct
// also takes it off the stack and onto the beautiful heap
// if it seems like a lot note that this is THE "global" object
typedef struct ServerContext {

    u32* client_allocations;

    u32 last_mbo;
    u16 mark; // last trade price, what margin marks LMV/SMV against
    ClientSettings* client_settings;
    BS* mbo_bs;
    u8 executing;
    CB* sw_queue;
    CB* hw_queue;
    FL* orders;
    Holder* ho;
    CB* fills;
    SCH* sch;
    FL* responses;
    FL* icebergs;
    CB* convert_holder;

    u64* rand;

    // mirror of MBO
    u32 last_mbp;
    BS* mbp_bs;

} ServerContext;


// yeah you need these two to initialize the server, cuz really it initalizes everything
ServerContext* server_init(TypeMetadata* tm, u32 * client_allocations, u64 seed);

void server_arrival(ServerContext* sc, u32 order_id);
void server_hw_to_sw(ServerContext* sc);
void server_exec_end(ServerContext* sc);
void server_exec_start(ServerContext* sc);
void server_exec_to_sw(ServerContext* sc);

void server_free(ServerContext* sc);

#endif

