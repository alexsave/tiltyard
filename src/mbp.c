#include "mbp.h"
#include "ob.h"
#include "types.h"

u32 mbp_derive_size(void* mbo_raw) {
    return sizeof(MBP) + ((MBO*)mbo_raw)->level_count * sizeof(MBPIndex);
}

void mbp_derive(void* mbp_raw, void* mbo_raw) {
    MBO* mbo = (MBO*)mbo_raw;
    MBP* mbp = (MBP*)mbp_raw;

    mbp->level_count = mbo->level_count;
    mbp->hi_bid_index = mbo->hi_bid_index;

    for (u16 i = 0; i < mbo->level_count; i++) {
        (mbp->levels + i)->price = (mbo->levels + i)->price;
        (mbp->levels + i)->quantity = (mbo->levels + i)->quantity;
    }
}
