#include <stdlib.h>
#include "sch.h"
#include "pq.h"

SCH* sch_init(){
    SCH* sch = malloc(sizeof(SCH));

    sch->now = 0;
    sch->current_bucket = 0;
    sch->slow_bucket = pq_init();

    sch->buckets = malloc(SCH_BUCKETS * sizeof(PQ*));

    for (int i = 0; i < SCH_BUCKETS; i++) {
        sch->buckets[i] = pq_init();
    }

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

    pq_push(sch->slow_bucket, scheduled_event);
    
    return;
}


// the most important two

// we'll just have to toss some 
// idk about the types, cant even focus form thsi noise
void sch_schedule(SCH* sch, uint64_t event, uint64_t delta_ns) {
    //printf("schedule invoked\n");
    
    // when we actully do scheduling, it's more like that we pass in DELTA, not "realtime"
    // so how many ns in teh future this happens

    // we have a limit, based on the bits and buckets we choose

    // i swear to god I had a diagram, 
    // but we basically can't schedule it such that  it will land in the same bucket EARLIER
    // to be safe we bascially say we can't schedule > "bucket time span"  * (bucket count -1)

    // let's check this value though
    
    // p bits can handle up to.. 2^p bits nanoseconds
    
    // this shoudl be hardcoded but
    uint64_t max_delta = P_SPAN * (SCH_BUCKETS - 1);

    //printf("%llu max vs %llu delta\n", max_delta, delta_ns);

    // if it's equal, we MUST be in the privious bucket
    // if it's one over, it's not the case

    if (delta_ns > max_delta) {
        //printf("delta too high\n");
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

    //printf("sum is %llu\n", sum);
    //printf("shifted sum is %llu\n", sum << E_BITS);
    //printf("some value is %llu\n", (uint64_t)7 * (1 << 39));
    
    // and then we do something like 

    // the real trick is that if we have 2+ buckets, we can reuse them infinitely and keep cycling through them
    // but this might go past the end of the conceptual array of buckets

    // so just to be safe
    uint8_t resulting_bucket = (sch->current_bucket + (sum >> P_BITS) )& BUCKET_MASK;

    // what a beaut
    // even if this overflows it will be bounded to bucket bits
    // and that tells us exactly which bucket to schedule into

    // and this is where pq comes in!

    // ah this needs the full int64 event thing
    
    // somethign like...
    // we have a 39 bit number that we care about
    // earlier we SHIFTED it 39 to the right to see if we changed buckets
    // now we shift it (64-39)=25 bits the left to put it in the proper place in the int64
    // of course that leaves 25 bits of the event, so we can only take the bottom 25
    // so don't pass in anything else
    uint64_t scheduled_event = (sum << E_BITS) | (event & E_MASK);



    //printf("about to push event %llu to %d\n", scheduled_event, resulting_bucket);
    pq_push(sch->buckets[resulting_bucket], scheduled_event);
    //printf("pushed\n");
    
    return;
}



// next event
uint64_t sch_pop(SCH* sch) {

    // the real fun one

    // pop and advance

    // as we hold the slow bucket, we also need to handle the special event that is basically "pop the slow scheduler"

    // somethign like...

    // ... we actually need to peek the current bucket, could be empty
    // gtg now but this is the next work

    // slow events  must be converted to fast events when they get in range. 

    
    //printf("checking pq nubmer #%llu\n", sch->current_bucket & BUCKET_MASK);
    // see if we can remove this check
    if(pq_is_empty(sch->buckets[sch->current_bucket&BUCKET_MASK])) {
        // advance until something scheduled
        do {
            //printf("pq nubmer #%llu is empty\n", sch->current_bucket & BUCKET_MASK);
            // important note: current_bucket is actually like buckets since start
            // need to do &7 to get current bucket index, 
            // or >>3 to get number of cycles through all buckets
            sch->current_bucket++;
        } while(pq_is_empty(sch->buckets[sch->current_bucket & BUCKET_MASK]));
        
    }

    uint64_t next = pq_pop(sch->buckets[sch->current_bucket & BUCKET_MASK]);

    //printf("raw pop %llu\n", next);
    
    sch->now = next >> E_BITS;
    //printf("now set to %llu, returning %llu\n", sch->now, next);
    

    // i"m going to decide right now that the top 3 bits of the "event" are the event type
    // and that 111 is the "slow check" event type
    if (((next >> (E_BITS - T_BITS)) & ((1 << T_BITS) -1)) == 7) {


        // this needs to take the current time, turn it into seconds, 
        // and potentially push slow events into the fast scheduler

        //[   cycles    22?    ][B][                PRIORITY              ]
        uint64_t current_time = (sch->current_bucket << P_BITS) + (next >> E_BITS);

        //time in seconds, always a bit lower than theoretical ns max delta

        // ohhhh right. but i thought we handled this. 
        // if an event is between "now" and "now + max_delta, we reschedule
        uint64_t max_delta = P_SPAN * (SCH_BUCKETS - 1);

        uint64_t latest_threshold = (current_time + max_delta)/S_TO_NS;


        if (!pq_is_empty(sch->slow_bucket)){

            // the priority queue doesn't actually clear itself when we pop, it just stops tracking
            // thus the last event in the pqueue (the one slow event) never clears
            // is this a fault of peek?
            //lets check
            //uint64_t peek_ts = pq_peek(sch->slow_bucket) >> E_BITS;
            // both these conditions must hold. although a bit faster to do this...
              
            while (!pq_is_empty(sch->slow_bucket)) {
                uint64_t peek_ts = pq_peek(sch->slow_bucket) >> E_BITS;


                if (peek_ts > latest_threshold)
                    break;
                // if it's equal its ok to continue because latest_threshold is truncated

                //printf("need to reschedule event, with absolute s %llu\n", peek_ts);


                //yup - but why
                // there are some events that require special handling
                // however i think they are best handled as a "type 0 client toggle" 
                // but with the server id of 0 as the client id
                // then it will be up to the server to detect EOD or EOM
                // or even - we COULD schedule type 7 "slow check" events into the slow scheduler
                // but we'd have to be careful to make sure not to resch
                // 10101010101010 .... 111 _____________ month or day
                // type 7 events have lots of unused space in them
                // more on this later

                uint64_t pop = pq_pop(sch->slow_bucket);

                // with this, we need to modify the timestamp to fit in the slow scheduler
                // getting the event is easy
                // but the timestamp is... 

                uint64_t seconds = peek_ts * S_TO_NS;

                uint8_t bucket = (seconds >> P_BITS) & BUCKET_MASK;
                uint64_t priority = seconds & P_MASK;

                pq_push(
                        sch->buckets[bucket], 
                        (priority << E_BITS) | (pop & E_MASK));

                peek_ts = pq_peek(sch->slow_bucket) >> E_BITS;

            }
        }

        /// we're done rescheudling the slow events


        // just reschedule the slow chcker. it's once an hour so the cost is minimal
        //printf("next %llu and max d %llu\n", next, max_delta);
        sch_schedule(sch, next, max_delta);



        // if it lines up exactly with ns max delta, ie truncated_t * billion == current time
        // this is weird and relies on slow check == 7, but it will work
        // because we then schedule two events at the exact time, but the order doesn't actaully matter because slow checker won't actually modify market state

        // if a slow event lines up exactly with the truncated_s
        // we can mulitply seconds by a billion and put it into the fast scheduler



        // don't even worry about what we just got, return the slow event

        // probably do some bullshit like update time
        // these 7 type events need to be scheduled very carefully

        //next = pq_pop(sch->slow_bucket);

        // actually this is more complex
        // lets say the next slow event is like a year out
        // we need to reschedule the 7 type event
        // and probably get the next fast event

        //... come back here later for montly stuff
    }

    // technically nanoseconds into the bucket, possibly unneeded
    //sch->now = next >> E_BITS;
    //printf("now set to %llu, returning %llu\n", sch->now, next);

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
