#include "test_rand.h"
#include "test_pq.h"
#include "test_fl.h"
#include "test_bs.h"
#include "test_sch.h"
// test_ob.h is stale (references removed .flags / ob_limit) — left out until updated
//#include "test_ob.h"
#include "test_pair.h"
#include "test_book_invariants.h"
#include "test_fills.h"
#include "test_market.h"
#include "test_sessions.h"
#include "test_stops.h"
#include "test_wakes.h"
#include "test_auction.h"
#include "test_fees.h"
#include "test_imbalance.h"
#include "test_mbp.h"
#include "test_canrep_release.h"
#include "test_copy_range.h"


int main(int argc, char* argv[]){
    // Random testing section
    //test_random();

    test_pq();

    test_pair();

    test_book_invariants();

    test_fills();

    test_market();

    test_sessions();

    test_stops();

    test_wakes();

    test_auction();

    test_auction_book();

    test_auction_priority();

    test_auction_dualbook();

    test_auction_offset();

    test_auction_offset_sell();

    test_auction_cancel();

    test_auction_cross_timing();

    test_auction_indicative();

    test_auction_mark();

    test_fees();

    test_imbalance();

    test_mbp();

    test_canrep_releases_slot();

    test_pair_cancel_same_id_rejected();

    test_pair_cross_spares_co_leg_cancel();

    test_copy_range_no_cut();

    test_copy_range_cut_middle();

    test_copy_range_cut_first();

    test_copy_range_cut_last();

    test_copy_range_sole_cut_drops_level();

    test_copy_range_two_cuts_one_range();

    //test_fl();

    //test_bs();

    //test_sch();
    
    //test_ob();

    return 0;
}

