#include "sch.h"

SCH* sch_init(){

    // idk
}

// the most important two

// we'll just have to toss some 
// idk about the types, cant even focus form thsi noise
void sch_schedule_fast(SCH* sch, uint64_t event, uint64_t delta_ns) {
    
    // when we actully do scheduling, it's more like that we pass in DELTA, not "realtime"
    // so how many ns in teh future this happens

    // we have a limit, based on the bits and buckets we choose

    // i swear to god I had a diagram, 
    // but we basically can't schedule it such that  it will land in the same bucket EARLIER
    // to be safe we bascially say we can't schedule > "bucket time span"  * (bucket count -1)

    // let's check this value though
    
    // p bits can handle up to.. 2^p bits nanoseconds
    
    // this shoudl be hardcoded but
    uint64_t bucket_span = 1 << P_BITS;
    uint64_t max_delta = bucket_span * (SCH_BUCKETS - 1);

    // if it's equal, we MUST be in the privious bucket
    // if it's one over, it's not the case

    if (delta_ns > max_delta) {
        // first off, delta_ns is pretty big already
        // lose some resolution and switch to slow scheduler
        
        // note worth testing if faster to precompute the 1/S_TO_NS and multiply here
        sch_schedule_slow(sch, event, delta_ns/S_TO_NS);
        
        return;
    }

    // ok at this point we have determined that it will fit into the fast scheduler

    // which bucket though?
    // becasue these are powers of two we can do a & and a >>
    
    // maybe we need to scale, not sure how best to store "curent_ns"
    // but it's whatever the last popped event is
    uint64_t sum = sch->current_ns + delta_ns;
    
    // and then we do something like 

    bucket_delta = sum >> P_BITS;
    
    // the real trick is that if we have 2+ buckets, we can reuse them infinitely and keep cycling through them
    // but this might go past the end of the conceptual array of buckets

    // so just to be safe
    resulting_bucket = (sch->current_bucket + bucket_delta )& ((1<<BUCKET_BITS)-1);

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

    pq_push(sch->buckets[resulting_bucket], scheduled_event);
    
    return;
}


// honestly could jsut be inlined, this is like 5 lines
void sch_schedule_slow(SCH* sch, uint64_t event, uint64_t delta_s) {
//hold on a sec

    // ok similar but there's only one bucket
    // to keep things consistent we will use the same amount of bits as the fast scheduler
    // so
    //bounds check

    
    uint64_t bucket_span = 1 << P_BITS;

    // again not sure about that current_s value.
    // seems annoying to have to check if we update the ns one every time
    uint64_t requested_s = delta_s + current_s;

    if (sch->current_s = delta_s > bucket_span){
        // at 39 bits this is 17432 years
        // 32 is a very respectable 136 years
        // longer than current stock market
        // so you better not be hitting this damn path
        // I could do some multi-bucket scheduling trickery there too, but honestly I think it should be fine
        return;
    }

    
    // EZ
    uint64_t scheduled_event = (requested_s << E_BITS) | (event & E_MASK);

    pq_push(sch->slow_bucket, scheduled_event);
    
    return;
}

// next event
uint_64 sch_pop(SCH* sch) {

    // the real fun one

    // pop and advance

    // as we hold the slow bucket, we also need to handle the special event that is basically "pop the slow scheduler"

    // somethign like...

    // ... we actually need to peek the current bucket, could be empty
    // gtg now but this is the next work
...

    uint64_t next = pq_pop(sch->buckets[current_bucket]);

    // i"m going to decide right now that the top 3 bits of the "event" are the event type
    // and that 111 is the "slow check" event type
    if ((next >> (E_BITS - 3)) & ((1 << 3) -1) == 7) {
        // don't even worry about what we just got, return the slow event
        
        // probably do some bullshit like update time
        // these 7 type events need to be scheduled very carefully
        
        //next = pq_pop(sch->slow_bucket);

        // actually this is more complex
        // lets say the next slow event is like a year out
        // we need to reschedule the 7 type event
        // and probably get the next fast event

        //... come back here later
    }   

    sch->now_ns = next >> E_BITS;
    // technically like nanoseconds into the bucket

    return next;
}




void sch_free(SCH* sch){
    //...
    free(sch);
}
