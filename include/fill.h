#ifndef FILL_H
#define FILL_H

typedef struct Fill {
    u32 order_id;
    u32 quantity_filled;
    // without checking "orders", which is 100% up to date and impossible for clients to see realtime
    // we have this to let them know if it was wiped out or not
    u8 partial;
} Fill;

#endif

