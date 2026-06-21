#include <stdlib.h>
#include "sch.h"
#include "pq.h"
#include "constants.h"
#include "rand.h"
#include "types.h"

SCH* sch_init(u64* rand){
    SCH* sch = malloc(sizeof(SCH));

    sch->now = 0;
    sch->current_bucket = 0;
    sch->slow_bucket = pq_init();

    sch->buckets = malloc(SCH_BUCKETS * sizeof(PQ*));

    for (int i = 0; i < SCH_BUCKETS; i++) {
        sch->buckets[i] = pq_init();
    }

    sch->rand = rand;

    return sch;
}

// honestly could jsut be inlined, this is like 5 lines
void sch_schedule_slow(SCH* sch, uint64_t event, uint64_t delta_ns) {
    //hold on a sec

    // 2^64-1
    uint64_t max_ns = ((((uint64_t)1 << 63) - 1) << 1) + 1;
    uint64_t now_ns = sch_now_ns(sch);


    // have to do minus, if delta is large, delta + now will overflow
    if (max_ns - now_ns < delta_ns) {
        // out of time bud
        printf("Out of time bud, let's wrap it up\n");
        return;
    }

    uint64_t absolute_ns = now_ns + delta_ns;
    uint64_t absolute_s = absolute_ns / S_TO_NS;

    //printf("maxns %llu, now_ns %llu, absolute_ns %llu, absolute_s %llu\n", max_ns, now_ns, absolute_ns, absolute_s);

    // EZ
    uint64_t scheduled_event = (absolute_s << E_BITS) | (event & E_MASK);

    //printf("%llu scheduled in slow\n", scheduled_event);
    pq_push(sch->slow_bucket, scheduled_event);

    return;
}


// the most important two

// we'll just have to toss some 
// idk about the types, cant even focus form thsi noise
void sch_schedule(SCH* sch, uint64_t event, uint64_t delta_ns) {

    // when we actully do scheduling, it's more like that we pass in DELTA, not "realtime"
    // so how many ns in teh future this happens

    // we have a limit, based on the bits and buckets we choose

    // but we basically can't schedule it such that  it will land in the same bucket EARLIER
    // to be safe we bascially say we can't schedule > "bucket time span"  * (bucket count -1)


    // p bits can handle up to.. 2^p bits nanoseconds

    // this shoudl be hardcoded but
    uint64_t max_delta = P_SPAN * (SCH_BUCKETS - 1);


    // if it's equal, we MUST be in the privious bucket
    // if it's one over, it's not the case

    if (delta_ns > max_delta) {
        //printf("scheduling %llu for slow %llu\n", event, delta_ns);
        // first off, delta_ns is pretty big already
        // lose some resolution and switch to slow scheduler

        // note worth testing if faster to precompute the 1/S_TO_NS and multiply here
        sch_schedule_slow(sch, event, delta_ns);

        return;
    }

    // ok at this point we have determined that it will fit into the fast scheduler

    // which bucket though?
    // becasue these are powers of two we can do a & and a >>

    // maybe we need to scale, not sure how best to store "curent_ns"
    // but it's whatever the last popped event is
    uint64_t sum = sch->now + delta_ns;


    // the real trick is that if we have 2+ buckets, we can reuse them infinitely and keep cycling through them
    // but this might go past the end of the conceptual array of buckets

    uint8_t resulting_bucket = (sch->current_bucket + (sum >> P_BITS) )& BUCKET_MASK;

    // even if this overflows it will be bounded to bucket bits
    // and that tells us exactly which bucket to schedule into


    // we have a 39 bit number that we care about
    // earlier we SHIFTED it 39 to the right to see if we changed buckets
    // now we shift it (64-39)=25 bits the left to put it in the proper place in the int64
    // of course that leaves 25 bits of the event, so we can only take the bottom 25
    // so don't pass in anything else
    uint64_t scheduled_event = (sum << E_BITS) | (event & E_MASK);

    pq_push(sch->buckets[resulting_bucket], scheduled_event);

    return;
}



// next event
uint64_t sch_pop(SCH* sch) {

    // the real fun one

    // pop and advance

    // as we hold the slow bucket, we also need to handle the special event that is basically "pop the slow scheduler"


    // ... we actually need to peek the current bucket, could be empty

    // slow events  must be converted to fast events when they get in range. 


    // see if we can remove this check
    if(pq_is_empty(sch->buckets[sch->current_bucket&BUCKET_MASK])) {
        // advance until something scheduled
        do {
            //printf("checking pq nubmer #%llu\n", sch->current_bucket & BUCKET_MASK);
            // important note: current_bucket is actually like buckets since start
            // need to do &7 to get current bucket index, 
            // or >>3 to get number of cycles through all buckets
            sch->current_bucket++;
        } while(pq_is_empty(sch->buckets[sch->current_bucket & BUCKET_MASK]));

    }

    //printf("%llu %llu %llu %llu\n", sch->buckets[sch->current_bucket&BUCKET_MASK]->heap[0] , sch->buckets[sch->current_bucket&BUCKET_MASK]->heap[1] , sch->buckets[sch->current_bucket&BUCKET_MASK]->heap[2] , sch->buckets[sch->current_bucket&BUCKET_MASK]->heap[3] );
    //printf("size %u\n", sch->buckets[sch->current_bucket & BUCKET_MASK]->current);
    uint64_t next = pq_pop(sch->buckets[sch->current_bucket & BUCKET_MASK]);

    sch->now = next >> E_BITS;


    // i"m going to decide right now that the top 3 bits of the "event" are the event type
    // and that 111 is the "slow check" event type
    //if ((((next >> PARAM_BITS) & T_MASK) == CONTROL_TYPE) && (next & PARAM_MASK == CONTROL_PARAM_SLOW)){


    //printf("next is %llu, kill param is %llu\n", next, ((CONTROL_TYPE << PARAM_BITS) | (CONTROL_PARAM_KILL)));

    if ((next & E_MASK) == ((CONTROL_TYPE << PARAM_BITS) | CONTROL_PARAM_SLOW)) {

        // only slow special type will be handled here as the slow&fast are both here
        // other checks are for main.c

        uint64_t current_time = (sch->current_bucket << P_BITS) + (next >> E_BITS);

        // if an event is between "now" and "now + max_delta, we reschedule
        uint64_t max_delta = P_SPAN * (SCH_BUCKETS - 1);

        uint64_t latest_threshold = (current_time + max_delta)/S_TO_NS;

        if (!pq_is_empty(sch->slow_bucket)){

            while (!pq_is_empty(sch->slow_bucket)) {
                uint64_t peek_ts = pq_peek(sch->slow_bucket) >> E_BITS;
                //printf("rescheduling to fast bucket %llu\n", pq_peek(sch->slow_bucket) & E_MASK);


                if (peek_ts > latest_threshold)
                    break;

                uint64_t pop = pq_pop(sch->slow_bucket);

                uint64_t seconds = peek_ts * S_TO_NS;

                uint8_t bucket = (seconds >> P_BITS) & BUCKET_MASK;
                uint64_t priority = seconds & P_MASK;


                rand_next(sch->rand);
                priority += (((*sch->rand) & MAX_U32) >> 3);

                pq_push(
                        sch->buckets[bucket], 
                        (priority << E_BITS) | (pop & E_MASK));


            }
        }

        sch_schedule(sch, next, max_delta);
    } 

    if ((next & E_MASK) == ((CONTROL_TYPE << PARAM_BITS) | CONTROL_PARAM_KILL)){
        printf("kill event\n");
        exit(1);
    }


    // technically nanoseconds into the bucket, possibly unneeded
    //sch->now = next >> E_BITS;

    // might be better to return actually next & E_MASK, but maybe some EOM or EOD events need priority
    return next;
}


// only supports ~585 years 
uint64_t sch_now_ns(SCH* sch) {
    return (sch->current_bucket << P_BITS) + sch->now;
}


void sch_free(SCH* sch){
    pq_free(sch->slow_bucket);

    for (int i = 0; i < SCH_BUCKETS; i++) {
        pq_free(sch->buckets[i]);
    }

    free(sch->buckets);

    free(sch);
}
