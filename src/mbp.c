#include "mbp.h"
#include "ob.h"
#include "types.h"
#include "constants.h"
#include "bs.h"
// not circular right?
#include "server.h"


u32 mbp_derive_size(void* mbo_raw) {
    return sizeof(MBP) + ((MBO*)mbo_raw)->level_count * sizeof(MBPIndex);
}

void mbp1_derive(ServerContext* sc) {

    void* new_mbp1_raw;
    u32 new_last_mbp1 = bs_reserve(sc->mbp1_bs, sizeof(MBP1), 1, &new_mbp1_raw);
    MBP10* new_mbp10 = (MBP10*)(bs_get_no_ref(sc->mbp10_bs, sc->last_mbp10))
    MBP1* new_mbp1 = (MBP1*)(new_mbp1_raw);
    
    u8 HI_BID_INDEX = 9;
    u8 LO_ASK_INDEX = 10;

    new_mbp1->hi_bid->quantity = (new_mbp10->levels + HI_BID_INDEX)->quantity;
    new_mbp1->hi_bid->price = (new_mbp10->levels + HI_BID_INDEX)->price;
    new_mbp1->lo_ask->quantity = (new_mbp10->levels + LO_ASK_INDEX)->quantity;
    new_mbp1->lo_ask->price = (new_mbp10->levels + LO_ASK_INDEX)->price;

    bs_get(sc->mbp1_bs, sc->last_mbp1);
    sc->last_mbp1 = next_last_mbp1;
}

// we dont need level count or hi bid index, but we should do like
// a single byte with top half coutninyaing how many asks, bottom half how many bids
// BBBBAAAA 
// because it could be not ten, for earlier ones
void mbp10_derive(ServerContext* sc) {
    // it updated, lets get a new one
    void* new_mbp10_raw;
    u32 next_last_mbp10 = bs_reserve(sc->mbp10_bs, sizeof(MBP10), 1, &new_mbp10_raw);
    MBP* new_mbp = (MBP*)(bs_get_no_ref(sc->mbp_bs, sc->last_mbp));
    MBP10* new_mbp10 = (MBP10*)new_mbp_10_raw;


    for (u8 i = 0; i < 20; i++) {
        MBPI* old_mbp10_level = old_mbp10->level + i;

        u16 new_quantity = 0;
        u16 new_price = 0;
        // put it on the toher side and i think we're good? if 
        if ( i < (i16)(9 - new_mbp->hi_bid_index)){
        } else if (( i  + new_mbp->hi_bid_index > new_mbp->level_count+8)){
        } else {
            new_quantity = new_mbp->levels[i + new_mbp->hi_bid_index - 9].quantity;
            new_price = new_mbp->levels[i + new_mbp->hi_bid_index - 9].price;
        }

        new_mbp_10_raw->quantity = new_quantity;
        new_mbp_10_raw->price = new_price;
    }

    // ez check
    // compare old and new very specfiically
    MBP10* old_mbp10 = (MBP10*)(bs_get_no_ref(sc->mbp10_bs, sc->last_mbp10));

    u8 HI_BID_INDEX = 9;
    u8 LO_ASK_INDEX = 10;



    // drop old mbp 10
    bs_get(sc->mbp_bs, sc->last_mbp10);
    sc->last_mbp10 = next_last_mbp10;

    if ((old_mbp10->levels + HI_BID_INDEX)->quantity != new_mbp10->levels + HI_BID_INDEX)->quantity) || (old_mbp10->levels + HI_BID_INDEX)->price != new_mbp10->levels + HI_BID_INDEX)->price))
        mbp1_derive(sc);
}

// more like a full update of mbp stuff
void mbp_derive(ServerContext* sc){
    void* new_mbo_raw = bs_get_no_ref(sc->mbo_bs, sc->last_mbo);
    // use this to check diffs in top ten levels
    u32 old_mbp_id = sc->last_mbp;


    void* new_mbp_raw;
    u32 next_last_mbp = bs_reserve(sc->mbp_bs, mbp_derive_size(new_mbo_raw), 1, &new_mbp_raw);

    MBO* mbo = (MBO*)new_mbo_raw;
    MBP* mbp = (MBP*)new_mbp_raw;

    mbp->level_count = mbo->level_count;
    mbp->hi_bid_index = mbo->hi_bid_index;

    for (u16 i = 0; i < mbo->level_count; i++) {
        (mbp->levels + i)->price = (mbo->levels + i)->price;
        (mbp->levels + i)->quantity = (mbo->levels + i)->quantity;
    }


    // however, we MIGHT also need to cascade to MBP 10. lets do it here
    // how to detect if top 10 levels in each direciton changed or not?

    // ah -nvm. all we have is price and quantity anyways

    // reserve could shift BS, so get it again

    void* old_mbp_raw = bs_get_no_ref(sc->mbp_bs, sc->last_mbp);
    MBP* old_mbp = (MBP*)old_mbp_raw;

    // compare top ten levels to new MBPo
    // yes this works with MAX_U16 hi bid index
    u16 previous_ask_count = (old_mbp->level_count - 1) - mbp->hi_bid_index;
    u16 new_ask_count = (mbp->level_count - 1) - mbp->hi_bid_index;
    u16 previous_bid_count = old_mbp->hi_bid_index + 1;
    u16 new_bid_count = mbp->hi_bid_index + 1;



    MBP10* old_mbp10 = (MBP10*)(bs_get_no_ref(sc->mbp10_bs, sc->last_mbp10));

    // if these changed and the new values is below 10 then we must have an update

    u8 mbp10_update = 0;

    if (previous_ask_count != new_ask_count && ((previous_ask_count < 10) || (new_ask_count < 10)))
        mbp10_update = 1;
    else if (previous_bid_count != new_bid_count && ((previous_bid_count < 10) || (new_bid_count < 10)))
        mbp10_update = 1;
    else {
        // those were the easy cases
        // athit this point we know the counts are unchanged or they were above 10 and thus we can't tell just by coutning levels
        for (u8 i = 0; i < 20; i++) {

            MBPI* old_mbp10_level = old_mbp10->level + i;

            u16 new_quantity = 0;
            u16 new_price = 0;
            // out of bounds check
            // target index in mbp level is (i-9 + hi_bid_index)
            // raw bounds forumlas:
            // if ( i - 9 + new_mbp->hi_bid_index < 0)
            // if (( i - 9 + new_mbp->hi_bid_index > new_mbp->level_count-1)


            // ok me think. 

            // this is just rearranged to avoid negatvies and simplified with alg
            // ah this is tricky cuz 0 + 65535 is just 65535, not < 9
            // put it on the toher side and i think we're good? if 
            if ( i < (i16)(9 - new_mbp->hi_bid_index)){
                // dont assign, it should be 0 in the next one
            } else if (( i  + new_mbp->hi_bid_index > new_mbp->level_count+8)){
                // 10 bids case:
                // i + 9 > 10 + 8, so only i = 10 will match
                // also dont assign, it should be 0 here

            } else {
                new_quantity = new_mbp->levels[i + new_mbp->hi_bid_index - 9].quantity;
                new_price = new_mbp->levels[i + new_mbp->hi_bid_index - 9].price;

            }
            if (new_quantity != (old_mbp10_level)->quantity) || (new_price != old_mbp10_level->price) {
                mbp10_update = 1;
                break;
            }
        }
    } 


    // hi bid index "-1"

    // i9 out of bounds
    // level 97 i10
    // level 98 i11
    // level 99 i12
    // level 100 i13
    // level 101 i14
    //i15

    //i - 9 + hi_bid_index




    // NOW we drop the old MBP
    bs_get(sc->mbp_bs, old_mbp_id);
    sc->last_mbp = next_last_mbp;

    if (mbp10_update) {
        mbp10_derive(sc);
    }
}


