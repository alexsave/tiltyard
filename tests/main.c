#include "test_rand.h"
#include "test_pq.h"
#include "test_fl.h"
#include "test_bs.h"
#include "test_sch.h"
// test_ob.h is stale (references removed .flags / ob_limit) — left out until updated
//#include "test_ob.h"
#include "test_pair.h"


int main(int argc, char* argv[]){
    // Random testing section
    //test_random();

    test_pq();

    test_pair();

    //test_fl();

    //test_bs();

    //test_sch();
    
    //test_ob();

    return 0;
}

