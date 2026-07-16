#ifndef ICEBERG_H
#define ICEBERG_H

#include "types.h"

// the hidden half of an iceberg. the book sees one chunk-sized slice at a time;
// when a slice fills we mint the next and point it back here until remaining runs out.
// remaining is u64 - the whole point is it dwarfs any single u16 order.
// chunk lives here too: a nibbled-to-zero slice has lost its display size otherwise.
typedef struct Iceberg {
    u32 client_id;
    u64 remaining;
    u16 price;
    u16 chunk;
} Iceberg;

#endif
