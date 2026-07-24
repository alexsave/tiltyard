#include <stdio.h>
#include <stdlib.h>

#include "types.h"
#include "server.h"
#include "order.h"
#include "constants.h"

// decode a run log written by server_log_dump. the sim now emits records raw because formatting
// them inline cost 12% of the run, so this is where the old text lines are produced instead -
// byte-identical to what the inline printfs used to write, so every grep and awk over the old
// output still works. run it once, or pipe it, rather than paying for it 26M times mid-simulation.
//
// usage: ./logdump [path]     (defaults to LOG_BIN_PATH)

#define CHUNK 8192

static void print_rec(LogRec* r) {
    if (r->kind == LOG_TRADE) {
        printf("TRADE buy %u p %u q %u id %u now %llu part %u\n",
               r->taker_is_buy, r->price, r->quantity_filled, r->order_id, r->now_ns, r->partial);
        return;
    }

    u8 is_buy     = (r->status >> BUY_DIRECTION_BIT) & 1;
    u8 is_cancel  = (r->status >> CANCEL_BIT) & 1;
    u8 is_can_rep = (r->status >> CAN_REP_BIT) & 1;
    u8 is_stop    = (r->status >> HAS_STOP_BIT) & 1;

    printf("[%llus] order #%u ", r->now_ns / S_TO_NS, r->order_id);
    printf("client #%u [$%lld/$%u/%lldq/%uq] ", r->client_id, r->cash, r->reserved_cash, r->shares, r->reserved_shares);
    if (is_cancel) {
        printf("cancel order #%u ", r->other_id);
    } else if (is_stop) {
        if (r->quantity > 0)
            printf("%s %s %ush @ $%u + ", ((r->status >> IS_MARKET_BIT) & 1) ? "market" : "limit",
                   is_buy ? "buy" : "sell", r->quantity, r->price);
        printf("stop %s %s %ush trigger $%u ", ((r->status >> STOP_LIMIT_BIT) & 1) ? "limit" : "market",
               r->second_direction ? "buy" : "sell", r->second_quantity, r->stop_price);
    } else {
        printf("%s %s %ush @ $%u ", ((r->status >> IS_MARKET_BIT) & 1) ? "market" : "limit",
               is_buy ? "buy" : "sell", r->quantity, r->price);
        if (is_can_rep) {
            printf("+ cancel order %u ", r->other_id);
        }
    }
    printf("\n");
}

int main(int argc, char* argv[]) {
    const char* path = argc > 1 ? argv[1] : LOG_BIN_PATH;

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "logdump: cannot open %s\n", path);
        return 1;
    }

    // read in chunks - one fread per record would put us right back in syscall overhead
    LogRec* buf = malloc(CHUNK * sizeof(LogRec));
    size_t got;
    while ((got = fread(buf, sizeof(LogRec), CHUNK, f)) > 0)
        for (size_t i = 0; i < got; i++)
            print_rec(buf + i);

    free(buf);
    fclose(f);
    return 0;
}
