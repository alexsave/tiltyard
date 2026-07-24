#include <stdio.h>
#include <stdlib.h>

#include "server.h"
#include "types.h"
#include "constants.h"

#include "order.h"
#include "utils.h"
#include "client_settings.h"
#include "bs.h"
#include "fl.h"
#include "cb.h"
#include "sch.h"
#include "response.h"
#include "holder.h"
#include "ob.h"
#include "rand.h"
#include "fill.h"
#include "iceberg.h"

#include "mbp.h"
#include "trade.h"

// yeah you need these two to initialize the server, cuz really it initalizes everything
// write the captured run log out raw. no formatting at all - integer-to-decimal conversion for
// ~26M records was 12% of the whole run, and none of it is work the simulation needs done. the
// records go out exactly as they sit in memory and `logdump` turns them back into the old text
void server_log_dump(ServerContext* sc) {
    u32 n = cb_count(sc->log);
    if (!n)
        return;

    FILE* f = fopen(LOG_BIN_PATH, "wb");
    if (!f) {
        printf("could not open %s for the run log\n", LOG_BIN_PATH);
        return;
    }

    // append-only ring - nothing is ever dequeued from this one - so start is 0 and all n
    // records sit contiguous from the base. that makes the whole dump a single fwrite
    fwrite(cb_at(sc->log, 0), sizeof(LogRec), n, f);
    fclose(f);

    printf("run log: %u records -> %s\n", n, LOG_BIN_PATH);
}

// repack live_roster from stream_roster. every tier at once, because the packed layout means one
// tier's range shifts the next one's. scanning in roster order keeps each range ascending by
// client_id, which is the order the old ws-testing scan visited them in - so broadcasts are
// emitted in exactly the same sequence and the sim stays deterministic
static void stream_roster_rebuild(ServerContext* sc) {
    cb_clear(sc->live_roster);

    for (u8 t = 0; t < TIER_COUNT; t++) {
        sc->live_offset[t] = cb_count(sc->live_roster);

        u32 end = sc->tier_offset[t + 1];
        for (u32 i = sc->tier_offset[t]; i < end; i++) {
            u32 cid = sc->stream_roster[i];
            if (sc->client_settings[cid].ws)
                cb_queue(sc->live_roster, &cid);
        }
    }

    sc->live_offset[TIER_COUNT] = cb_count(sc->live_roster);
}

ServerContext* server_init(TypeMetadata* tm, u32 * client_allocations, u64 seed){
    ServerContext* sc = malloc(sizeof(ServerContext));

    /// ahhhh catch 22
    // we cant know how many clients to reserve BEFORE going through this
    //sc->client_settings = calloc(sc->ho->num_clients, sizeof(ClientSettings));
    sc->ho = holder_init(tm, client_allocations, &(sc->client_settings));

    sc->client_allocations = client_allocations;

    sc->mbo_bs = bs_init(32768);
    void* mbo_address = 0;
    sc->last_mbo = bs_reserve(sc->mbo_bs, sizeof(MBO), 1, &mbo_address);
    ((MBO*)mbo_address)->level_count = 0;
    ((MBO*)mbo_address)->hi_bid_index = MAX_U16;

    sc->mbp_bs = bs_init(32768);
    void* mbp_address = 0;
    sc->last_mbp = bs_reserve(sc->mbp_bs, sizeof(MBP), 1, &mbp_address);
    ((MBP*)mbp_address)->level_count = 0;
    ((MBP*)mbp_address)->hi_bid_index = MAX_U16;
    



    sc->mark = 0; // no trade yet, so nothing to mark against until one prints
    sc->exchange_cash = 0;
    sc->executing = 0;
    sc->is_open = 0; // closed until the first open event rings the bell
    sc->day_orders = cb_init(sizeof(u32)); // drained every close, so ids alone suffice
    sc->gtd = pq_init(); // (date << 32 | id), lives across closes
    sc->price_pq = pq_init();
    sc->expire_cb = cb_init(sizeof(u64));
    sc->buy_stops = pq_init(); // both (price << 32 | id), arrival order rides Order.ns
    sc->sell_stops = xpq_init();

    sc->wake_above = pq_init(); // both (price << 32 | id), no ordering within a price
    sc->wake_below = xpq_init();

    sc->auctioning = 0;
    sc->auction_frozen = 0;
    sc->imbalance_buy = 0;
    sc->imbalance_sell = 0;
    sc->auction_bids = pq_init(); // both limit-only, (price << 32 | id)
    sc->auction_asks = pq_init();
    sc->auction_market_bids = cb_init(sizeof(u32)); // market-only, arrival order
    sc->auction_market_asks = cb_init(sizeof(u32));
    sc->auction_arrivals = cb_init(sizeof(u32)); // all auction orders, arrival order
    sc->auction_bid_sorted = cb_init(sizeof(u64)); // scratch: heaps popped ascending for the cross
    sc->auction_ask_sorted = cb_init(sizeof(u64));
    sc->sw_queue = cb_init(sizeof(u32));
    sc->hw_queue = cb_init(sizeof(u32));
    sc->convert_holder = cb_init(sizeof(u32));

    sc->trades = cb_init(sizeof(Trade));
    sc->candles_sec = cb_init(sizeof(Candle));
    sc->candles_min = cb_init(sizeof(Candle));
    sc->candles_hr = cb_init(sizeof(Candle));
    sc->candles_day = cb_init(sizeof(Candle));
    sc->imbalances = cb_init(sizeof(Imbalance));
    sc->notified = cb_init(sizeof(u32));
    sc->log = cb_init(sizeof(LogRec));

    sc->orders = fl_init(sizeof(Order), MIN_RESERVED_PACKET);
    sc->fills = cb_init(sizeof(Fill));
    sc->responses = fl_init(sizeof(Response), MAX_U32);
    sc->icebergs = fl_init(sizeof(Iceberg), MAX_U32);

    sc->rand = rand_init(seed);
    rand_next(sc->rand);
    sc->sch = sch_init(sc->rand);

    // lesser subscription paths - this is the first fixed size one
    sc->mbp10_bs = bs_init(32768);
    void* mbp10_address = 0;
    sc->last_mbp10 = bs_reserve(sc->mbp10_bs, sizeof(MBP10), 1, &mbp10_address);
    ((MBP*)mbp10_address)->level_count = 0;
    ((MBP*)mbp10_address)->hi_bid_index = MAX_U16;

    sc->mbp1_bs = bs_init(32768);
    void* mbp1_address = 0;
    sc->last_mbp1 = bs_reserve(sc->mbp1_bs, sizeof(MBP1), 1, &mbp1_address);
    ((MBP*)mbp1_address)->level_count = 0;
    ((MBP*)mbp1_address)->hi_bid_index = MAX_U16;

    // the empty init blobs above ARE version 0's views, so the lazy derive starts satisfied
    sc->book_version = 0;
    sc->mbp_derived_version = 0;

    // Trades won't actually use blob store as they are will actually be stored for the entire duration
    // CB is fine for this and candles
    // kinda orthogonal stream

    // build the stream roster: client_ids bucketed by sub_tier, tier_offset marking each range.
    // built once; the ws-connected subset is derived from it into live_roster below
    u32 client_count = sc->ho->num_clients;
    sc->stream_roster = malloc(client_count * sizeof(u32));
    sc->tier_offset = malloc((TIER_COUNT + 1) * sizeof(u32));
    u32 write_idx = 0;
    for (u8 t = 0; t < TIER_COUNT; t++) {
        sc->tier_offset[t] = write_idx;
        for (u32 c = 0; c < client_count; c++)
            if (sc->client_settings[c].sub_tier == t)
                sc->stream_roster[write_idx++] = c;
    }
    sc->tier_offset[TIER_COUNT] = write_idx;

    sc->live_roster = cb_init(sizeof(u32));
    sc->live_offset = malloc((TIER_COUNT + 1) * sizeof(u32));
    stream_roster_rebuild(sc);

    // map each tier to its data structure so a broadcast's u8 tier resolves in one index
    sc->tier_source = malloc(TIER_COUNT * sizeof(void*));
    sc->tier_source[TIER_MBO] = sc->mbo_bs;
    sc->tier_source[TIER_MBP] = sc->mbp_bs;
    sc->tier_source[TIER_MBP10] = sc->mbp10_bs;
    sc->tier_source[TIER_MBP1] = sc->mbp1_bs;
    sc->tier_source[TIER_TRADE] = sc->trades;
    sc->tier_source[TIER_CANDLE_SEC] = sc->candles_sec;
    sc->tier_source[TIER_CANDLE_MIN] = sc->candles_min;
    sc->tier_source[TIER_CANDLE_HR] = sc->candles_hr;
    sc->tier_source[TIER_CANDLE_DAY] = sc->candles_day;
    sc->tier_source[TIER_IMBALANCE] = sc->imbalances;

    sc->leg_cost = malloc(sizeof(LegCost));
    sc->other_leg_cost = malloc(sizeof(LegCost));

    return sc;
}

// which blob a client's subscription entitles it to, and the id to pin for the trip. the
// tiers are all views of the same book - an mbo subscriber gets it order by order, an mbp1
// subscriber just the touch - so a response carries whichever one the client pays for, the
// same way a stream push does. anything outside the blob tiers (free, or a client that never
// set one) pins the mbo: delivery drops it unread, but the ref accounting stays uniform
static u8 client_blob_tier(ServerContext* sc, u32 client_id) {
    u8 tier = sc->client_settings[client_id].sub_tier;
    return tier <= TIER_MBP1 ? tier : TIER_MBO;
}

// the price views are derived lazily: book changes only tick book_version, and the actual
// mbp/mbp10/mbp1 cut happens here, at most once per version, the first time someone pins
// one. a cycle whose consumers are all mbo-tier (the apex churn) never pays for them
void mbp_ensure(ServerContext* sc) {
    if (sc->mbp_derived_version == sc->book_version)
        return;
    sc->mbp_derived_version = sc->book_version;
    mbp_derive(sc);
}

static u32 tier_blob_id(ServerContext* sc, u8 blob_tier) {
    if (blob_tier == TIER_MBP || blob_tier == TIER_MBP10 || blob_tier == TIER_MBP1) {
        mbp_ensure(sc);
        if (blob_tier == TIER_MBP)   return sc->last_mbp;
        if (blob_tier == TIER_MBP10) return sc->last_mbp10;
        return sc->last_mbp1;
    }
    return sc->last_mbo;
}

void schedule_response(ServerContext* sc, u32 client_id, u32 status, u32 quantity_filled, u32 order_id, u16 price, u8 rej_reason) {
    u8 blob_tier = client_blob_tier(sc, client_id);
    u32 blob_id = tier_blob_id(sc, blob_tier);
    bs_bump_refs((BS*)sc->tier_source[blob_tier], blob_id);
    Response r = {.tier = blob_tier, .snapshot_id = blob_id, .client_id = client_id, .status = status, .order_id = order_id, .quantity_filled = quantity_filled, .price = price, .rej_reason = rej_reason};
    u32 response_id = fl_insert(sc->responses, &r);
    u64 response_event = build_event(CLIENT_IN_TYPE, response_id);
    sch_schedule(sc->sch, response_event, calculate_jitter(sc->client_settings + (client_id), sc->rand));
    // so don't blast over websocket. the flag doubles as the dedup guard for the scratch
    // buffer - already set means already queued, so an id lands in there at most once
    if (!sc->client_settings[client_id].will_notify) {
        sc->client_settings[client_id].will_notify = 1;
        cb_queue(sc->notified, &client_id);
    }
}

// same as schedule_response, but carries both legs of an atomic pair in one delivery
void schedule_pair_response(ServerContext* sc, u32 client_id, u32 status, u32 order_id, u16 price, u32 quantity_filled, u32 second_order_id, u16 second_price, u32 second_quantity_filled, u8 rej_reason) {
    u8 blob_tier = client_blob_tier(sc, client_id);
    u32 blob_id = tier_blob_id(sc, blob_tier);
    bs_bump_refs((BS*)sc->tier_source[blob_tier], blob_id);
    Response r = {.tier = blob_tier, .snapshot_id = blob_id, .client_id = client_id, .status = status, .order_id = order_id, .price = price, .quantity_filled = quantity_filled, .second_order_id = second_order_id, .second_price = second_price, .second_quantity_filled = second_quantity_filled, .rej_reason = rej_reason};
    u32 response_id = fl_insert(sc->responses, &r);
    u64 response_event = build_event(CLIENT_IN_TYPE, response_id);
    sch_schedule(sc->sch, response_event, calculate_jitter(sc->client_settings + (client_id), sc->rand));
    if (!sc->client_settings[client_id].will_notify) {
        sc->client_settings[client_id].will_notify = 1;
        cb_queue(sc->notified, &client_id);
    }
}

// broadcast one tier's latest data to one client. index is a blob id (blob tiers) or a buffer
// offset (trade/candle), resolved at delivery so a store that moves mid-flight stays safe. blob
// tiers pin the blob with a ref for the trip; buffers need nothing (cb_at re-resolves).
void schedule_stream_response(ServerContext* sc, u32 client_id, u8 tier, u32 index) {
    if (tier <= TIER_MBP1)
        bs_bump_refs((BS*)sc->tier_source[tier], index);
    Response r = {.tier = tier, .snapshot_id = index, .client_id = client_id, .status = 1u << BROADCAST_BIT, .order_id = MAX_U32};
    u32 response_id = fl_insert(sc->responses, &r);
    u64 response_event = build_event(CLIENT_IN_TYPE, response_id);
    sch_schedule(sc->sch, response_event, calculate_jitter(sc->client_settings + client_id, sc->rand));
}

// after a book change: push each book tier's latest snapshot to its subscribers, then the latest
// print to trade subscribers. one loop per data shape so no per-client shape check is needed.
// skips clients that already got a direct response this cycle (will_notify), then clears it.
void server_stream(ServerContext* sc) {
    ClientSettings* cs = sc->client_settings;

    for (u8 t = TIER_MBO; t <= TIER_MBP1; t++) {
        u32 end = sc->live_offset[t + 1];
        u32 i = sc->live_offset[t];
        if (i == end)
            continue; // nobody streams this tier, so its view is never derived for it
        // resolving through tier_blob_id is what triggers the lazy mbp derive - only now,
        // when a subscriber is actually about to pin this tier's view of the changed book
        u32 blob = tier_blob_id(sc, t);
        for (; i < end; i++) {
            u32 cid = *(u32*)cb_at(sc->live_roster, i);
            if (cs[cid].will_notify)
                continue;
            if (!cs[cid].stream_in_flight) {
                cs[cid].stream_in_flight = 1;
                schedule_stream_response(sc, cid, t, blob);
            } else {
                // conflate: a delivery is already in the air, and by the time it lands only
                // the newest book is worth acting on - the intermediate never ships. swap
                // the pending pin to this snapshot and let main.c chain it when the one in
                // the air lands. arrival keeps latency honest: the newest change still
                // can't be seen before now + this client's own wire time
                if (cs[cid].stream_pending_valid)
                    bs_get((BS*)sc->tier_source[cs[cid].stream_pending_tier], cs[cid].stream_pending_snapshot);
                bs_bump_refs((BS*)sc->tier_source[t], blob);
                cs[cid].stream_pending_valid = 1;
                cs[cid].stream_pending_tier = t;
                cs[cid].stream_pending_snapshot = blob;
                cs[cid].stream_pending_arrival = sch_now_ns(sc->sch) + calculate_jitter(cs + cid, sc->rand);
            }
        }
    }

    if (!cb_is_empty(sc->trades)) {
        u32 offset = cb_count(sc->trades) - 1;
        u32 end = sc->live_offset[TIER_TRADE + 1];
        for (u32 i = sc->live_offset[TIER_TRADE]; i < end; i++) {
            u32 cid = *(u32*)cb_at(sc->live_roster, i);
            if (!cs[cid].will_notify)
                schedule_stream_response(sc, cid, TIER_TRADE, offset);
        }
    }

    // only the clients that were actually flagged, not every client in the sim. deduped on the
    // way in, so this is at most one pass over the handful that got a direct response
    u32 flagged = cb_count(sc->notified);
    for (u32 i = 0; i < flagged; i++)
        cs[*(u32*)cb_at(sc->notified, i)].will_notify = 0;
    cb_clear(sc->notified);
}

// checks the cancel order id valididtiy, 0 if it's good to cancel
u8 cancel_precheck(Order* in, FL* orders, MBO* mbo) {
    Order* to_cancel = (Order*)fl_get(orders, in->other_id);
    // easy checks
    u8 is_ours = to_cancel->client_id == in->client_id;
    u8 is_active = to_cancel->quantity > 0;
    u8 is_rejected = (to_cancel->status >> REJECT_BIT) & 1;

    // fast check
    if (!is_ours)
        return REJ_NOT_YOUR_ORDER;

    // a wake rests in a trigger heap with no quantity; live and ours is the whole check. a
    // cancelled one carries the reject bit and reads as already done just below
    if (((to_cancel->status >> WAKE_BIT) & 1) && !is_rejected)
        return REASON_NONE;

    if (!is_active || is_rejected)
        return REJ_ORDER_ALREADY_DONE;

    // an armed stop (stop-only or a combined order's minted leg) rests in a trigger heap,
    // not the book - live and ours is the whole check. a fired one already cleared the
    // bit, so it correctly falls through to the book walk and misses
    if ((to_cancel->status >> HAS_STOP_BIT) & 1)
        return REASON_NONE;

    u8 in_book = 0;
    // OH ALSO IS IT IN THE DARN BOOK
    // it is a bit of a fast hack to rely on the quantity being reset on rejected orders
    // but we can jump directly there
    for (u16 i = 0; i < mbo->level_count; i++) {
        MBOIndex * mboi = mbo->levels + i;
        if (mboi->price != to_cancel->price)
            continue;

        u32 offset = mboi->byte_offset;
        MBOLevel * mbol = (MBOLevel*)(mbo_data_start(mbo) + offset);
        for (u16 o = 0; o < mbol->order_count; o++){
            if (((MBOEntry*)(mbol->entries + o))->order_id != in->other_id)
                continue;

            // found em
            in_book = 1;
            break;
        }

        break;
    }

    return in_book ? REASON_NONE : REJ_UNKNOWN_ORDER;
}

// a visible iceberg slice just emptied - show the next one. returns the minted order id
// (for the fill response's second_order_id) or MAX_U32 when the iceberg is spent and freed.
// caller beware: fl_insert here can move the orders pool, so refetch order pointers after.
u32 iceberg_replenish(ServerContext* sc, u32 ice_id, u8 is_buy) {
    Iceberg* ice = (Iceberg*)fl_get(sc->icebergs, ice_id);

    if (ice->remaining == 0) {
        fl_release(sc->icebergs, ice_id);
        return MAX_U32;
    }

    // the last slice can be short
    u16 next = ice->chunk;
    if (ice->remaining < ice->chunk)
        next = (u16)ice->remaining;
    ice->remaining -= next;

    Order synth = {};
    synth.client_id = ice->client_id;
    synth.price = ice->price;
    synth.quantity = next;
    synth.status = (1 << ICEBERG_BIT) | (is_buy ? (1 << BUY_DIRECTION_BIT) : 0);
    synth.second_id = ice_id; // same iceberg, next slice
    u32 synth_id = fl_insert(sc->orders, &synth);
    cb_queue(sc->convert_holder, &synth_id);

    printf("ICEBERG slice #%u q %u @ $%u, %llu hidden\n", synth_id, next, ice->price, ice->remaining);
    return synth_id;
}

// reg t buying power = m * (equity - requirement), expanded so the /m cancels off:
//   m*(cash + LMV - SMV) - m*(LMV + SMV)/m  ==  m*cash + (m-1)*LMV - (m+1)*SMV
// cash carries no requirement so it's worth m. stock adds 1 but drags in 1/m, so m-1.
// a short subtracts 1 and still drags in 1/m, so -(m+1).
// signed until the floor: a margin loan makes m*cash negative, and in u32 that wraps to
// something that passes every check. floored because negative excess means restricted,
// which is not a margin call
u64 client_bp(ClientSettings* cs, u16 mark, u32 reclaimed_cash, u32 reclaimed_shares) {
    i64 m = cs->margin_mult;
    i64 lmv = cs->shares > 0 ? (i64)mark * cs->shares : 0;
    i64 smv = cs->shares < 0 ? (i64)mark * -cs->shares : 0;

    // an ask only commits BP where it runs past what we're long
    i64 longs = cs->shares > 0 ? cs->shares : 0;
    i64 asks = (i64)cs->reserved_shares - reclaimed_shares;
    i64 naked = asks > longs ? asks - longs : 0;

    i64 bp = m * cs->cash + (m - 1) * lmv - (m + 1) * smv
        - ((i64)cs->reserved_cash - reclaimed_cash)
        - (i64)mark * naked;

    return bp < 0 ? 0 : (u64)bp;
}

// equity under the client's maintenance %. driven by the mark moving, not by anything a
// client sent, so it can't live in a precheck. multiplied out rather than divided
u8 client_maint_call(ClientSettings* cs, u16 mark) {
    if (cs->is_cash_account)
        return 0;

    i64 lmv = cs->shares > 0 ? (i64)mark * cs->shares : 0;
    i64 smv = cs->shares < 0 ? (i64)mark * -cs->shares : 0;
    i64 gmv = lmv + smv;
    if (gmv == 0)
        return 0;

    return (cs->cash + lmv - smv) * 100 < gmv * cs->maint_pct;
}

// how much of a position an order in this direction is allowed to close for free. a close
// reduces risk and a broker always lets you out, so reg t charges only what opens exposure.
// a cash account has no such notion - it pays for everything out of the pools it holds
u32 client_close_q(ClientSettings* cs, u8 direction) {
    if (cs->is_cash_account)
        return 0;

    // buying back a short
    if (direction == 1 && cs->shares < 0)
        return (u32)(-cs->shares); 

    // selling longs we aren't already quoting
    if (direction == 0 && cs->shares > (i64)cs->reserved_shares)
        return (u32)(cs->shares - cs->reserved_shares); 

    return 0;
}

void leg_walk(LegCost* lc, u8 direction, u8 is_market, u8 is_gtc, u16 price, u32 order_q, MBO* mbo, u32 close_q) {
    //LegCost lc = { .cost = 0, .open_notional = 0, .q_remain = order_q };
    lc->cost = 0;
    lc->open_notional = 0;
    lc->q_remain = order_q;

    u16 lo_ask_index = mbo->hi_bid_index == MAX_U16 ? 0 : mbo->hi_bid_index + 1;

    if (direction == 1) {
        for (u16 run = lo_ask_index; run < mbo->level_count; run++) {
            MBOIndex* mboi = mbo->levels + run;
            if (!is_market && (lc->q_remain > 0 && price < mboi->price))
                break;

            u32 take = lc->q_remain < mboi->quantity ? lc->q_remain : mboi->quantity;
            lc->cost += take * mboi->price;

            u32 closing = take < close_q ? take : close_q;
            close_q -= closing;
            lc->open_notional += (u64)(take - closing) * mboi->price;

            lc->q_remain -= take;
        }
    } else if (mbo->hi_bid_index != MAX_U16) {
        // 0-- wraps to MAX_U16, the same "ran off the bottom" sentinel as hi_bid_index
        for (u16 run = mbo->hi_bid_index; run != MAX_U16; run--) {
            MBOIndex* mboi = mbo->levels + run;
            if (!is_market && (lc->q_remain > 0 && price > mboi->price))
                break;

            u32 take = lc->q_remain < mboi->quantity ? lc->q_remain : mboi->quantity;
            lc->cost += take; // a sell draws shares, not cash

            u32 closing = take < close_q ? take : close_q;
            close_q -= closing;
            lc->open_notional += (u64)(take - closing) * mboi->price;

            lc->q_remain -= take;
        }
    }

    // whatever didn't cross rests at the limit, and funds at it. ioc/fok discard it instead
    if (is_gtc) {
        lc->cost += direction == 1 ? lc->q_remain * price : lc->q_remain;

        u32 closing = lc->q_remain < close_q ? lc->q_remain : close_q;
        close_q -= closing;
        lc->open_notional += (u64)(lc->q_remain - closing) * price;
    }

    return;// lc;
}

// the usual stuff
// 0 if the order can be worked, otherwise why it can't
// fillable, if non-null, comes back with how much crosses on arrival - the iceberg create
// path uses it to size a marketable slice. an iceberg prices the whole hidden total here,
// not just the visible chunk, so the walk covers everything it might cross
u8 add_precheck(ServerContext* sc, Order* in, ClientSettings* cs, MBO* mbo, u16 mark, u32 reclaimed_cash, u32 reclaimed_shares, u32* fillable){

    // 0 - reclaim from cancelled order
    // 1 - figure out full fill "cost"

    // nothing has printed yet, so mark against what this order is willing to pay
    if (mark == 0)
        mark = in->price;

    u8 direction = (in->status >> BUY_DIRECTION_BIT) & 1;
    u8 is_market = (in->status >> IS_MARKET_BIT) & 1;
    u8 is_ioc = (in->status >> IOC_BIT) & 1;
    u8 is_fok = (in->status >> FOK_BIT) & 1;
    // gtc is the default: no tif bit set rests the remainder, like a plain limit always did
    u8 is_gtc = !(is_ioc | is_fok);
    u16 price = in->price;

    if (in->quantity == 0)
        return REJ_INVALID_QUANTITY;

    if (!is_market && price == 0)
        return REJ_INVALID_PRICE;

    // a market order has no price to rest at, so it must be told to drop what it cant fill
    if (is_market && is_gtc)
        return REJ_BAD_QUALIFIER;

    // an iceberg has to rest to hide, so a market or ioc/fok one is a contradiction.
    // second_quantity is the total; it has to cover the visible chunk (in->quantity)
    u8 is_iceberg = (in->status >> ICEBERG_BIT) & 1;
    if (is_iceberg) {
        if (is_market || !is_gtc)
            return REJ_BAD_QUALIFIER;
        if (in->second_quantity < in->quantity)
            return REJ_INVALID_QUANTITY;
    }

    // walk the whole iceberg total, so fillable reflects everything it could cross
    u32 order_q = is_iceberg ? in->second_quantity : in->quantity;

    leg_walk(sc->leg_cost, direction, is_market, is_gtc, price, order_q, mbo, client_close_q(cs, direction));

    // whatever the walk consumed crossed on arrival; the rest would rest
    if (fillable)
        *fillable = order_q - sc->leg_cost->q_remain;

    // an ioc discards what it couldn't fill and a gtc rests it, but a fok has to be whole
    if (sc->leg_cost->q_remain && is_fok)
        return CXL_FOK_KILLED;

    if (!cs->is_cash_account) {
        // reg t IS the gate, there is no second equity check. equity's job is maintenance
        if (client_bp(cs, mark, reclaimed_cash, reclaimed_shares) < sc->leg_cost->open_notional)
            return direction == 1 ? REJ_NO_BUYING_POWER : REJ_NO_SHARES;
    } else if (direction == 1) {
        // we have enough cash to buy
        if (cs->cash - cs->reserved_cash + reclaimed_cash < sc->leg_cost->cost)
            return REJ_NO_BUYING_POWER;
    } else {
        // we have enough shares to sell
        if (cs->shares - cs->reserved_shares + reclaimed_shares < sc->leg_cost->cost)
            return REJ_NO_SHARES;
    }
    return REASON_NONE;

}

// runs on the arriving stop order. the stop half lives in the second_* fields (trigger in
// stop_price); the primary fields are an optional NOW order that goes in immediately and
// validates through add_precheck like any other. only the stop's shape is checked here -
// it funds at trigger time, when it converts to a market/limit and rejoins the sw queue
u8 stop_precheck(Order* in) {
    if (in->second_quantity == 0)
        return REJ_INVALID_QUANTITY;

    if (in->stop_price == 0)
        return REJ_INVALID_PRICE;

    // stop limit converts to a limit at second_price, so there has to be one
    if (((in->status >> STOP_LIMIT_BIT) & 1) && in->second_price == 0)
        return REJ_INVALID_PRICE;

    // second_quantity is already the iceberg total - the fields can't serve two masters
    if ((in->status >> ICEBERG_BIT) & 1)
        return REJ_BAD_QUALIFIER;

    // oco on a combined order brackets its two halves, which only works if the NOW half
    // can rest - a market NOW filling would instantly pull its own protection
    if (((in->status >> OCO_BIT) & 1) && in->quantity > 0 && ((in->status >> IS_MARKET_BIT) & 1))
        return REJ_BAD_QUALIFIER;

    return REASON_NONE;
}

u8 canrep_precheck(ServerContext* sc, Order* in, ClientSettings* cs, FL* orders, MBO* mbo, u16 mark) {
    // here, we will enforce a valid cancellation
    u8 cancel_reason = cancel_precheck(in, orders, mbo);
    if (cancel_reason)
        return cancel_reason;

    u32 reclaimed_cash = 0;
    u32 reclaimed_shares = 0;
    Order* cancel = (Order*)fl_get(orders, in->other_id);
    // an armed stop reserved nothing, so replacing one reclaims nothing
    if ((cancel->status >> HAS_STOP_BIT) & 1) {
    } else if ((cancel->status >> BUY_DIRECTION_BIT) & 1) {
        reclaimed_cash += cancel->quantity * cancel->price;
    } else {
        reclaimed_shares += cancel->quantity;
    }

    // fidelity style: the replacement can carry a stop half, or be one outright
    if ((in->status >> HAS_STOP_BIT) & 1) {
        u8 stop_reason = stop_precheck(in);
        if (stop_reason)
            return stop_reason;

        // stop-only replacement: nothing executes now, so nothing more to fund
        if (in->quantity == 0)
            return REASON_NONE;
    }

    return add_precheck(sc, in, cs, mbo, mark, reclaimed_cash, reclaimed_shares, 0);
}

// 0 if both legs can be worked, otherwise why they can't. the bid is in the primary
// fields and the ask in second_*, so this checks the legs against each other too
u8 pair_precheck(ServerContext* sc, Order* in, Order* ask_in, ClientSettings* cs, FL* orders, MBO* mbo, u16 mark) {
    // nothing has printed yet, so mark against what this quote is willing to pay
    if (mark == 0)
        mark = in->price;

    u8 is_can_rep = (in->status >> CAN_REP_BIT) & 1;
    u8 is_cancel = (in->status >> CANCEL_BIT) & 1;
    u8 ask_is_can_rep = (ask_in->status >> CAN_REP_BIT) & 1;
    u8 is_market = (in->status >> IS_MARKET_BIT) & 1;
    u8 is_gtc = !(((in->status >> IOC_BIT) & 1) | ((in->status >> FOK_BIT) & 1));

    // both legs of a pair are resting quotes, so a market or ioc/fok pair is nonsense.
    // and second_* is the ask leg here, so a stop half has nowhere to live
    if (is_market || !is_gtc || ((in->status >> HAS_STOP_BIT) & 1))
        return REJ_BAD_QUALIFIER;

    // a replacing or cancelling leg needs a valid cancel. a stale / nonexistent / not-ours
    // cancel id fails cancel_precheck, and we hand back that leg's reason
    u8 bid_cancel_reason = (is_can_rep | is_cancel) ? cancel_precheck(in, orders, mbo) : REASON_NONE;
    if (bid_cancel_reason)
        return bid_cancel_reason;

    u8 ask_cancel_reason = (ask_is_can_rep | is_cancel) ? cancel_precheck(ask_in, orders, mbo) : REASON_NONE;
    if (ask_cancel_reason)
        return ask_cancel_reason;

    // both legs naming the same resting order is not two cancels, it is one asked for twice:
    // each leg's precheck ran before either wiped anything, so both pass, and ob_pair then
    // splices the same book entry out twice and overruns the size we reserved for the snapshot
    if ((is_can_rep | is_cancel) && (ask_is_can_rep | is_cancel) && in->other_id == ask_in->other_id)
        return REJ_BAD_QUALIFIER;

    // the pair book op can only splice book residents, so neither leg may pull a stop
    if ((is_can_rep | is_cancel) && ((((Order*)fl_get(orders, in->other_id))->status >> HAS_STOP_BIT) & 1))
        return REJ_BAD_QUALIFIER;
    if ((ask_is_can_rep | is_cancel) && ((((Order*)fl_get(orders, ask_in->other_id))->status >> HAS_STOP_BIT) & 1))
        return REJ_BAD_QUALIFIER;

    // a pure cancel pair just pulls both quotes, so it has nothing to order, size or fund
    if (is_cancel)
        return REASON_NONE;

    if (in->quantity == 0 || in->second_quantity == 0)
        return REJ_INVALID_QUANTITY;

    if (in->price == 0 || in->second_price == 0)
        return REJ_INVALID_PRICE;

    // the two legs must be ordered, bid strictly below ask
    if (in->price >= in->second_price)
        return REJ_CROSSED_PAIR;

    // whatever we cancel frees its reserve, which helps fund the new legs: a replace that
    // raises the bid can exceed current buying power yet still fit once the old bid's
    // reservation comes back. we only get here once the cancels validated, so a bogus
    // cancel id contributes nothing. key each to the pool matching its direction.
    u32 freed_cash = 0, freed_shares = 0;
    if (is_can_rep) {
        Order* oc = (Order*)fl_get(orders, in->other_id);
        if ((oc->status >> BUY_DIRECTION_BIT) & 1) freed_cash += oc->quantity * oc->price;
        else freed_shares += oc->quantity;
    }
    if (ask_is_can_rep) {
        Order* oc = (Order*)fl_get(orders, ask_in->other_id);
        if ((oc->status >> BUY_DIRECTION_BIT) & 1) freed_cash += oc->quantity * oc->price;
        else freed_shares += oc->quantity;
    }

    // the legs don't cross each other, but either one can still cross the book - quote both
    // sides above the best ask and the bid walks while the ask rests - so each prices the same
    // way a lone order would. both are limits and both rest what they don't fill
    leg_walk(sc->leg_cost, 1, 0, 1, in->price, in->quantity, mbo, client_close_q(cs, 1));
    leg_walk(sc->other_leg_cost, 0, 0, 1, in->second_price, in->second_quantity, mbo, client_close_q(cs, 0));

    // on margin there's one pool behind both legs, so the two charges add and gate once. this
    // is what lets a flat maker quote two sides: the ask opens a short rather than demanding
    // shares it doesn't hold. at most one close allowance is live, since shares is one sign
    if (!cs->is_cash_account) {
        if (client_bp(cs, mark, freed_cash, freed_shares) < sc->leg_cost->open_notional + sc->other_leg_cost->open_notional)
            return REJ_NO_BUYING_POWER;

        return REASON_NONE;
    }

    // cash account: bid draws cash, ask draws shares — separate pools, checked independently
    // signed throughout: a margin loan or a short narrowed to u32 wraps past every check
    i64 bp = (cs->cash - cs->reserved_cash) + freed_cash;
    i64 sh = (cs->shares - cs->reserved_shares) + freed_shares;

    if ((i64)sc->leg_cost->cost > bp)
        return REJ_NO_BUYING_POWER;

    if ((i64)sc->other_leg_cost->cost > sh)
        return REJ_NO_SHARES;

    return REASON_NONE;
}


// rest a canonical stop (params in its primary fields) in its trigger heap: buys fire on
// the way up, so a min heap surfaces the lowest trigger; sells fire on the way down, max
// heap. arrival order isn't in the key - the ns stamp settles ties at fire time. day/gtd
// bookkeeping is here because the resting paths below never see this order
void stop_rest(ServerContext* sc, u32 order_id) {
    Order* o = (Order*)fl_get(sc->orders, order_id);
    u64 entry = ((u64)o->stop_price << 32) | order_id;

    if ((o->status >> BUY_DIRECTION_BIT) & 1)
        pq_push(sc->buy_stops, entry);
    else
        xpq_push(sc->sell_stops, entry);

    if ((o->status >> DAY_BIT) & 1)
        cb_queue(sc->day_orders, &order_id);
    else if ((o->status >> GTD_BIT) & 1)
        pq_push(sc->gtd, ((u64)o->second_id << 32) | order_id);
}

// keep the batch in convert_holder sorted by arrival: if the newcomer isn't the newest,
// re-queue the current last entry to grow the ring (the first shift), slide the rest
// right until the gap is where the newcomer belongs, and write it once.
//
// batches are one trigger price's worth. that used to be assumed small; it is not - a
// stop cascade puts every stop sharing a trigger into ONE batch, and a run with two
// stop-armed retail populations produced 336 at a single price out of 1,378 in the wave.
// the insertion sort is fine at that size (a few tens of thousands of shifts is nothing),
// but the function should not be trusting its inputs to keep it in bounds
void stop_batch_insert(ServerContext* sc, u32 batch_start, u32 order_id) {
    CB* ch = sc->convert_holder;
    Order* o = (Order*)fl_get(sc->orders, order_id);
    u64 ns = o->ns;

    u32 n = cb_count(ch);
    // a batch_start past the end means the ring was drained under us. there is no batch to
    // sort into, so treat it as a fresh one rather than walking backwards out of the buffer
    if (batch_start > n)
        batch_start = n;

    if (n > batch_start) {
        u32 last = *(u32*)cb_at(ch, n - 1);
        Order* last_o = (Order*)fl_get(sc->orders, last);

        if (last_o->ns > ns) {
            // local copy - cb_queue can double the ring, freeing what cb_at pointed into
            cb_queue(ch, &last);

            u32 gap = n - 1;
            while (gap > batch_start) {
                u32 prev = *(u32*)cb_at(ch, gap - 1);
                Order* prev_o = (Order*)fl_get(sc->orders, prev);
                if (prev_o->ns <= ns)
                    break;
                *(u32*)cb_at(ch, gap) = prev;
                gap--;
            }
            *(u32*)cb_at(ch, gap) = order_id;
            return;
        }
    }

    // newest arrival (or first of the batch): it just goes on the end
    cb_queue(ch, &order_id);
}

// one popped heap entry. ids recycle, so the slot must still hold a live armed stop
// matching the price and side this entry fired under - anything else means someone else
// lives here now, and the entry just dies
void stop_fire(ServerContext* sc, u64 entry, u8 is_buy, u32 batch_start) {
    u32 order_id = (u32)(entry & MAX_U32);
    Order* o = (Order*)fl_get(sc->orders, order_id);

    if (o->quantity == 0 || ((o->status >> REJECT_BIT) & 1) || !((o->status >> HAS_STOP_BIT) & 1))
        return;

    if (o->stop_price != (u16)(entry >> 32) || (((o->status >> BUY_DIRECTION_BIT) & 1) != is_buy))
        return;

    // convert in place: a stop limit becomes a plain limit at its price, a plain stop a
    // market order - ioc implied, since a market order has nothing to rest at
    o->status &= ~(1 << HAS_STOP_BIT);
    if ((o->status >> STOP_LIMIT_BIT) & 1) {
        o->status &= ~(1 << STOP_LIMIT_BIT);
    } else {
        o->status |= (1 << IS_MARKET_BIT);
        if (!((o->status >> FOK_BIT) & 1))
            o->status |= (1 << IOC_BIT);
    }

    printf("STOP #%u fired, trigger $%u\n", order_id, o->stop_price);
    stop_batch_insert(sc, batch_start, order_id);
}

// one side of an oco pair is done - pull the other with a synthetic cancel through the
// convert path. the ns guard blocks recycled ids: a legit partner was placed at or before
// the oco order (its id had to exist to be pointed at, and a combined bracket stamps both
// halves at once), so a newer stamp is a stranger living in the freed slot
void oco_pull(ServerContext* sc, u32 done_id) {
    Order* done = (Order*)fl_get(sc->orders, done_id);
    u32 partner_id = done->other_id;
    Order* partner = (Order*)fl_get(sc->orders, partner_id);

    if (partner->quantity == 0 || ((partner->status >> REJECT_BIT) & 1))
        return;

    if (partner->ns > done->ns)
        return;

    Order cxl = {0};
    cxl.client_id = done->client_id;
    // control: the server minted this, so the ack hands the slot straight back
    cxl.status = (1 << CANCEL_BIT) | (1 << CONTROL_BIT);
    cxl.other_id = partner_id;
    u32 cxl_id = fl_insert(sc->orders, &cxl);
    cb_queue(sc->convert_holder, &cxl_id);

    printf("OCO #%u done, pulling #%u\n", done_id, partner_id);
}

// fired oco stops pull their partners at the hit, not the fill - walk the freshly fired
// group and queue a cancel for each linked order, behind the group itself. the bit comes
// off so the fill hook doesn't pull a second time when the conversion executes
void oco_sweep(ServerContext* sc, u32 batch_start) {
    u32 batch_end = cb_count(sc->convert_holder);
    for (u32 i = batch_start; i < batch_end; i++) {
        u32 fired_id = *(u32*)cb_at(sc->convert_holder, i);
        Order* fired = (Order*)fl_get(sc->orders, fired_id);
        if ((fired->status >> OCO_BIT) & 1) {
            fired->status &= ~(1 << OCO_BIT);
            oco_pull(sc, fired_id);
        }
    }
}

// a print at `last` fires every buy stop with trigger at or below it, and every sell stop
// at or above it. one price group at a time, each landing in convert_holder in arrival
// order - within a price the heap gives id order, and ids recycle, so it means nothing
void check_stops(ServerContext* sc, u16 last) {
    while (!pq_is_empty(sc->buy_stops)) {
        u16 price = (u16)(pq_peek(sc->buy_stops) >> 32);
        if (price > last)
            break;

        u32 batch_start = cb_count(sc->convert_holder);
        while (!pq_is_empty(sc->buy_stops)) {
            if ((u16)(pq_peek(sc->buy_stops) >> 32) != price)
                break;
            stop_fire(sc, pq_pop(sc->buy_stops), 1, batch_start);
        }
    }

    while (!xpq_is_empty(sc->sell_stops)) {
        u16 price = (u16)(xpq_peek(sc->sell_stops) >> 32);
        if (price < last)
            break;

        u32 batch_start = cb_count(sc->convert_holder);
        while (!xpq_is_empty(sc->sell_stops)) {
            if ((u16)(xpq_peek(sc->sell_stops) >> 32) != price)
                break;
            stop_fire(sc, xpq_pop(sc->sell_stops), 0, batch_start);
        }
    }
}

// rest a wake in its trigger heap, keyed like a stop: an above-wake (buy direction) fires
// when the print rises to its price, a below-wake when it falls. no tif - a wake just waits
// until it fires or the client cancels it
void wake_rest(ServerContext* sc, u32 order_id) {
    Order* o = (Order*)fl_get(sc->orders, order_id);
    u64 entry = ((u64)o->price << 32) | order_id;

    if ((o->status >> BUY_DIRECTION_BIT) & 1)
        pq_push(sc->wake_above, entry);
    else
        xpq_push(sc->wake_below, entry);
}

// one popped wake entry. ids recycle, so the slot must still hold a live wake at this price
// and side - a cancelled one carries the reject bit, a recycled slot fails the price/side
// match. a fired wake sends the client a snapshot and hands its slot straight back
void wake_fire(ServerContext* sc, u64 entry, u8 is_above) {
    u32 order_id = (u32)(entry & MAX_U32);
    Order* o = (Order*)fl_get(sc->orders, order_id);

    if (((o->status >> REJECT_BIT) & 1) || !((o->status >> WAKE_BIT) & 1))
        return;

    if (o->price != (u16)(entry >> 32) || (((o->status >> BUY_DIRECTION_BIT) & 1) != is_above))
        return;

    printf("WAKE #%u fired, price $%u\n", order_id, o->price);
    // control: the ack carries the book snapshot and hands the spent slot back
    schedule_response(sc, o->client_id, (1 << WAKE_BIT) | (1 << CONTROL_BIT), 0, order_id, o->price, REASON_NONE);
}

// a print at `last` fires every above-wake at or below it and every below-wake at or above
// it. no batch ordering (a wake has no market effect), so entries just pop and notify
void check_wakes(ServerContext* sc, u16 last) {
    while (!pq_is_empty(sc->wake_above)) {
        if ((u16)(pq_peek(sc->wake_above) >> 32) > last)
            break;
        wake_fire(sc, pq_pop(sc->wake_above), 1);
    }

    while (!xpq_is_empty(sc->wake_below)) {
        if ((u16)(xpq_peek(sc->wake_below) >> 32) < last)
            break;
        wake_fire(sc, xpq_pop(sc->wake_below), 0);
    }
}

// reserve a parked order's buying power so a client can't park more than it can fund: a buy
// holds cash at its price (or the mark, for a market order), a sell holds shares
void auction_hold(ClientSettings* cs, Order* o, u16 mark) {
    if ((o->status >> BUY_DIRECTION_BIT) & 1)
        cs->reserved_cash += (u32)o->quantity * (((o->status >> IS_MARKET_BIT) & 1) ? mark : o->price);
    else
        cs->reserved_shares += o->quantity;
}

// give back what auction_hold reserved (on cancel, or at the cross before settling)
void auction_release(ClientSettings* cs, Order* o, u16 mark) {
    if ((o->status >> BUY_DIRECTION_BIT) & 1)
        cs->reserved_cash -= (u32)o->quantity * (((o->status >> IS_MARKET_BIT) & 1) ? mark : o->price);
    else
        cs->reserved_shares -= o->quantity;
}

// all order handling during a call auction, kept apart from the continuous path. an add parks
// in auction_arrivals (arrival order); the split into bid/ask/market queues happens at the cross
void server_auction_order(ServerContext* sc, u32 order_id) {
    Order* in = (Order*)fl_get(sc->orders, order_id);
    u32 status = in->status;

    // a pure cancel tombstones its target so the cross drops it and reconciles book/reserves
    if (status & (1 << CANCEL_BIT)) {
        // once the window freezes, cancels are rejected - only adds still park
        if (sc->auction_frozen) {
            in->status |= (1 << REJECT_BIT);
            schedule_response(sc, in->client_id, (1 << CANCEL_BIT) | (1 << REJECT_BIT), 0, order_id, 0, REJ_BAD_QUALIFIER);
            return;
        }
        Order* tgt = (Order*)fl_get(sc->orders, in->other_id);
        u8 reason = REASON_NONE;
        if (tgt->client_id != in->client_id)
            reason = REJ_NOT_YOUR_ORDER;
        else if (tgt->quantity == 0 || ((tgt->status >> REJECT_BIT) & 1))
            reason = REJ_ORDER_ALREADY_DONE;
        if (reason) {
            in->status |= (1 << REJECT_BIT);
            schedule_response(sc, in->client_id, (1 << CANCEL_BIT) | (1 << REJECT_BIT), 0, order_id, 0, reason);
            return;
        }
        auction_release(sc->client_settings + tgt->client_id, tgt, sc->mark); // give the reserve back
                                                                              // back the cancelled quantity out of the running imbalance
        if ((tgt->status >> BUY_DIRECTION_BIT) & 1)
            sc->imbalance_buy -= tgt->quantity;
        else
            sc->imbalance_sell -= tgt->quantity;
        tgt->quantity = 0;
        tgt->status |= (1 << REJECT_BIT);
        schedule_response(sc, in->client_id, (1 << CANCEL_BIT), 0, order_id, 0, REASON_NONE);
        return;
    }

    // only plain limit/market adds may park - a stop/wake/pair/iceberg/canrep/oco is rejected.
    // a market needs no price, a limit does; both need a quantity
    u32 disallowed_mask = (1 << HAS_STOP_BIT) | (1 << WAKE_BIT)
        | (1 << ASK_BID_PAIR_BIT) | (1 << ICEBERG_BIT) | (1 << CAN_REP_BIT) | (1 << OCO_BIT);
    u8 is_market = (status >> IS_MARKET_BIT) & 1;
    u8 disallowed = (status & disallowed_mask) != 0;
    if (disallowed || in->quantity == 0 || (!is_market && in->price == 0)) {
        in->status |= (1 << REJECT_BIT);
        u8 why = disallowed ? REJ_BAD_QUALIFIER : (in->quantity == 0 ? REJ_INVALID_QUANTITY : REJ_INVALID_PRICE);
        schedule_response(sc, in->client_id, (1 << REJECT_BIT), 0, order_id, 0, why);
        return;
    }

    // closing window past the cutoff: a new add must relieve the imbalance, not grow it. only the
    // side opposite the current imbalance is accepted (NYSE offset-only orders). the free entry
    // that ran all day and into the pre-close established this imbalance, so it isn't empty here
    if (sc->auctioning && sc->is_open && sc->imbalance_buy != sc->imbalance_sell) {
        u8 add_is_buy = (status >> BUY_DIRECTION_BIT) & 1;
        u8 buy_heavy = sc->imbalance_buy > sc->imbalance_sell;
        if (add_is_buy == buy_heavy) {
            in->status |= (1 << REJECT_BIT);
            schedule_response(sc, in->client_id, (1 << REJECT_BIT), 0, order_id, 0, REJ_OFFSET_ONLY);
            return;
        }
    }

    // check and reserve buying power like a resting order would (no shorting/margin in the
    // auction for now). a buy needs cash at its price or the mark, a sell needs the shares
    ClientSettings* cs = sc->client_settings + in->client_id;
    if (status & (1 << BUY_DIRECTION_BIT)) {
        u32 cost = (u32)in->quantity * (is_market ? sc->mark : in->price);
        if (cost > cs->cash - cs->reserved_cash) {
            in->status |= (1 << REJECT_BIT);
            schedule_response(sc, in->client_id, (1 << REJECT_BIT), 0, order_id, 0, REJ_NO_BUYING_POWER);
            return;
        }
    } else if (in->quantity > cs->shares - cs->reserved_shares) {
        in->status |= (1 << REJECT_BIT);
        schedule_response(sc, in->client_id, (1 << REJECT_BIT), 0, order_id, 0, REJ_NO_SHARES);
        return;
    }
    auction_hold(cs, in, sc->mark);

    // fold into the running imbalance the feed publishes (a cancel below backs it out)
    if (status & (1 << BUY_DIRECTION_BIT))
        sc->imbalance_buy += in->quantity;
    else
        sc->imbalance_sell += in->quantity;

    in->ns = sch_now_ns(sc->sch); // arrival stamp, breaks ties at the marginal clearing price
    cb_queue(sc->auction_arrivals, &order_id);
    schedule_response(sc, in->client_id, (1 << AUCTION_BIT), 0, order_id, in->price, REASON_NONE);
}

// fill up to `shares` on one side at the clearing price, market orders first (fifo). the queue
// holds only live orders (cancelled ones, quantity 0, were filtered when the arrivals were
// split), so no guard here. fills settle against the clearing price - the auction is a pooled
// cross, so buyers and sellers each net `matched` shares and cash balances without pairing
void auction_fill_side(ServerContext* sc, u32 shares, u16 clearing, u8 is_buy) {
    CB* market = is_buy ? sc->auction_market_bids : sc->auction_market_asks;
    while (shares > 0 && !cb_is_empty(market)) {
        u32 id = *(u32*)cb_deque(market);
        Order* o = (Order*)fl_get(sc->orders, id);
        u32 q = o->quantity < shares ? o->quantity : shares;
        o->quantity -= q;

        ClientSettings* cs = sc->client_settings + o->client_id;
        if (is_buy) {
            cs->cash -= (i64)q * clearing;
            cs->shares += q;
        } else {
            cs->cash += (i64)q * clearing;
            cs->shares -= q;
        }
        shares -= q;

        u32 fstatus = (1 << FILL_BIT) | (1 << AUCTION_BIT) | (o->quantity > 0 ? (1 << PARTIAL_FILL_BIT) : 0);
        schedule_response(sc, o->client_id, fstatus, q, id, clearing, REASON_NONE);
    }

    if (shares == 0) // markets took it all, no limit fills to work out
        return;

    // then limits, best price first: bids from the top of the ascending buffer down, asks from
    // the bottom up. stop at the first non-crossing price - those orders are the remainder
    CB* sorted = is_buy ? sc->auction_bid_sorted : sc->auction_ask_sorted;
    u32 cnt = cb_count(sorted);
    u64* buf = (u64*)sorted->buffer;
    u32* arrivals = (u32*)sc->auction_arrivals->buffer; // entry low bits are an arrival index
    for (u32 k = 0; k < cnt && shares > 0; k++) {
        u64 entry = is_buy ? buf[cnt - 1 - k] : buf[k];
        u16 price = (u16)(entry >> 32);
        if (is_buy ? price < clearing : price > clearing)
            break;

        u32 idx = (u32)(entry & MAX_U32);
        u32 oid = arrivals[is_buy ? ~idx : idx]; // bids store ~arrival index (see server_auction)
        Order* o = (Order*)fl_get(sc->orders, oid);
        u32 q = o->quantity < shares ? o->quantity : shares;
        o->quantity -= q;

        ClientSettings* cs = sc->client_settings + o->client_id;
        if (is_buy) {
            cs->cash -= (i64)q * clearing;
            cs->shares += q;
        } else {
            cs->cash += (i64)q * clearing;
            cs->shares -= q;
        }
        shares -= q;

        u32 fstatus = (1 << FILL_BIT) | (1 << AUCTION_BIT) | (o->quantity > 0 ? (1 << PARTIAL_FILL_BIT) : 0);
        schedule_response(sc, o->client_id, fstatus, q, oid, clearing, REASON_NONE);
    }
}

// the resting book sits on one side of the clearing price, so at most one of these runs
void auction_consume_book(ServerContext* sc, u8 is_buy, u32 shares, u16 clearing) {
    BS* mbo_bs = sc->mbo_bs;
    FL* orders = sc->orders;
    CB* fills = sc->fills;

    // a synthetic limit-ioc at the clearing price eats exactly `shares` of book depth (never more
    // than crosses), leaving a fresh mbo with those orders spliced out or reduced
    Order taker = {0};
    taker.price = clearing;
    taker.quantity = shares;
    taker.status = (1 << IOC_BIT) | (is_buy ? (1 << BUY_DIRECTION_BIT) : 0);
    u32 taker_id = fl_insert(orders, &taker);

    u32 prev_mbo = sc->last_mbo;
    u32 old_size = mbo_bs->metadata[prev_mbo].size;
    void* new_mbo_raw;
    // consuming never grows the book
    u32 next_mbo = bs_reserve(mbo_bs, old_size, 1, &new_mbo_raw);
    // re-get: the reserve may have moved it
    void* old_mbo_raw = bs_get_no_ref(mbo_bs, prev_mbo);

    u32 new_size = ob_canrep(orders, taker_id, old_mbo_raw, new_mbo_raw, fills);
    sc->last_mbo = next_mbo;
    bs_resize(mbo_bs, new_size);

    // the post-cross book is a new version - the price tiers re-derive lazily the moment a
    // response below pins one, so an mbp subscriber still gets its fill next to the fresh touch
    sc->book_version++;

    // each maker settles at the clearing price (not its resting price), gives its reserve back,
    // and gets a fill notification just like a continuous maker would
    while (!cb_is_empty(fills)) {
        Fill* fill = (Fill*)cb_deque(fills);
        Order* maker = (Order*)fl_get(orders, fill->order_id);
        u32 q = fill->quantity_filled;
        u8 partial = fill->partial;
        u32 maker_client = maker->client_id;
        ClientSettings* mcs = sc->client_settings + maker_client;
        if ((maker->status >> BUY_DIRECTION_BIT) & 1) {
            // book bid: gives back cash held at its price
            mcs->reserved_cash -= q * maker->price;
            mcs->cash -= (i64)q * clearing;
            mcs->shares += q;
        } else {
            // book ask: gives back the reserved shares
            mcs->reserved_shares -= q;
            mcs->cash += (i64)q * clearing;
            mcs->shares -= q;
        }
        maker->quantity -= q;

        u32 fstatus = (1 << FILL_BIT) | (1 << AUCTION_BIT) | (partial ? (1 << PARTIAL_FILL_BIT) : 0);
        schedule_response(sc, maker_client, fstatus, q, fill->order_id, clearing, REASON_NONE);
    }

    // hand back the construction ref the superseded book was reserved with, exactly as the
    // continuous and prune paths do. without this the predecessor never reaches zero refs, and
    // since the metadata ring can only reclaim from its head, one leaked slot parks md_start
    // forever - the ring then fills to md_capacity and the next reserve is fatal
    bs_get(mbo_bs, prev_mbo);

    fl_release(orders, taker_id);
}

// distribute the parked arrivals (limits into the price heaps, markets into the fifos), pop the
// heaps into the ascending scratch, and walk the merged curve + resting book for the clearing
// price and matched volume. leaves the fifos/scratch populated for the caller's fill. release=1
// hands each order its park reserve back (the real cross settles next); release=0 leaves reserves
// intact, for the read-only indicative the NOII feed publishes. the caller clears the workspace
void auction_walk(ServerContext* sc, u8 release, u16* clearing_out, u32* matched_out) {
    // sort the parkers: limits into their side's price heap, markets into their side's fifo
    // (base demand/supply). arrivals stays intact for the remainder drain, read flat (no wrap)
    u32 base_demand = 0;
    u32 base_supply = 0;
    u32 total_demand = 0; // summed limit-bid shares (the market side is base_demand)
    u32 n = cb_count(sc->auction_arrivals);
    u32* arrivals = (u32*)sc->auction_arrivals->buffer;
    for (u32 i = 0; i < n; i++) {
        u32 id = arrivals[i];
        Order* o = (Order*)fl_get(sc->orders, id);
        if (o->quantity == 0)
            continue;

        // give the park reserve back: fills settle cash directly, the remainder re-reserves
        // when it rests, and cancelled orders already released above. the indicative pass keeps
        // reserves intact - nothing settles, the orders are still parked
        if (release)
            auction_release(sc->client_settings + o->client_id, o, sc->mark);

        u8 is_buy = (o->status >> BUY_DIRECTION_BIT) & 1;
        if ((o->status >> IS_MARKET_BIT) & 1) {
            cb_queue(is_buy ? sc->auction_market_bids : sc->auction_market_asks, &id);
            if (is_buy)
                base_demand += o->quantity;
            else
                base_supply += o->quantity;
        } else {
            // tie-break by arrival, not by a recycled id. both are min-heaps popped into ascending
            // buffers, but asks fill from the bottom and bids from the top - so asks store the
            // arrival index i, bids store ~i, both leaving earliest-first at the fill end.
            // arrivals[i] maps the index back to the order id at walk/fill time
            u32 tie = is_buy ? ~i : i;
            u64 entry = ((u64)o->price << 32) | tie;
            pq_push(is_buy ? sc->auction_bids : sc->auction_asks, entry);
            if (is_buy)
                total_demand += o->quantity;
        }
    }

    // pop the limit heaps into the ascending scratch buffers (reused for the walk and the fill)
    u32 bid_count = sc->auction_bids->current - 1;
    u32 ask_count = sc->auction_asks->current - 1;
    for (u32 i = 0; i < bid_count; i++) {
        u64 e = pq_pop(sc->auction_bids);
        cb_queue(sc->auction_bid_sorted, &e);
    }
    for (u32 i = 0; i < ask_count; i++) {
        u64 e = pq_pop(sc->auction_asks);
        cb_queue(sc->auction_ask_sorted, &e);
    }

    // walk ascending: supply gathers asks at or below the price, demand sheds bids as they fall
    // under it. matched is min(demand, supply), and the price where it peaks is the clearing
    // price. market orders have no price, so they ride under the whole curve as base demand/supply
    u64* bid_buf = (u64*)sc->auction_bid_sorted->buffer;
    u64* ask_buf = (u64*)sc->auction_ask_sorted->buffer;

    // the resting book joins the curves: levels climb in price, bids are [0, hi_bid], asks
    // above, and each level's aggregate quantity folds in without loading its orders
    MBO* mbo = (MBO*)bs_get_no_ref(sc->mbo_bs, sc->last_mbo);
    u16 hi_bid = mbo->hi_bid_index;
    u32 book_bid_total = 0;
    if (hi_bid != MAX_U16)
        for (u16 i = 0; i <= hi_bid; i++)
            book_bid_total += mbo->levels[i].quantity;

    u32 bids_at_or_above = total_demand + book_bid_total;
    u32 asks_at_or_below = 0;
    u32 matched = 0;
    u16 clearing = sc->mark;
    u32 bid_i = 0, ask_i = 0;
    u16 ask_start = hi_bid == MAX_U16 ? 0 : hi_bid + 1; // book ask levels are [ask_start, count)
    u16 book_bid_i = 0;
    u16 book_ask_i = ask_start;
    while (bid_i < bid_count || ask_i < ask_count || book_bid_i < ask_start || book_ask_i < mbo->level_count) {
        // lowest price still unprocessed across the four ascending sources
        u16 price = MAX_U16;
        if (bid_i < bid_count && (u16)(bid_buf[bid_i] >> 32) < price)
            price = (u16)(bid_buf[bid_i] >> 32);
        if (ask_i < ask_count && (u16)(ask_buf[ask_i] >> 32) < price)
            price = (u16)(ask_buf[ask_i] >> 32);
        if (book_bid_i < ask_start && mbo->levels[book_bid_i].price < price)
            price = mbo->levels[book_bid_i].price;
        if (book_ask_i < mbo->level_count && mbo->levels[book_ask_i].price < price)
            price = mbo->levels[book_ask_i].price;

        while (ask_i < ask_count && (u16)(ask_buf[ask_i] >> 32) == price)
            asks_at_or_below += ((Order*)fl_get(sc->orders, arrivals[(u32)(ask_buf[ask_i++] & MAX_U32)]))->quantity;
        while (book_ask_i < mbo->level_count && mbo->levels[book_ask_i].price == price)
            asks_at_or_below += mbo->levels[book_ask_i++].quantity;

        u32 bids_here = 0;
        while (bid_i < bid_count && (u16)(bid_buf[bid_i] >> 32) == price)
            bids_here += ((Order*)fl_get(sc->orders, arrivals[~(u32)(bid_buf[bid_i++] & MAX_U32)]))->quantity;
        while (book_bid_i < ask_start && mbo->levels[book_bid_i].price == price)
            bids_here += mbo->levels[book_bid_i++].quantity;

        u32 demand = base_demand + bids_at_or_above;
        u32 supply = base_supply + asks_at_or_below;
        u32 crossed = demand < supply ? demand : supply;
        if (crossed > matched) {
            matched = crossed;
            clearing = price;
        }
        // stepping above this price, its bids leave demand
        bids_at_or_above -= bids_here;
    }

    // no limit crossed, but two market sides can still trade - clear at the reference price
    if (matched == 0 && base_demand > 0 && base_supply > 0) {
        clearing = sc->mark;
        matched = base_demand < base_supply ? base_demand : base_supply;
    }

    *clearing_out = clearing;
    *matched_out = matched;
}

// run the call auction cross: find the single clearing price, fill markets first then limits,
// spill the remainder (limits rest, markets cancel)
void server_auction(ServerContext* sc) {
    u16 clearing;
    u32 matched;
    // release reserves - we settle below
    auction_walk(sc, 1, &clearing, &matched);
    printf("AUCTION cross: clearing $%u, matched %u\n", clearing, matched);

    // re-read the book for the fill; the walk already folded it into clearing/matched
    MBO* mbo = (MBO*)bs_get_no_ref(sc->mbo_bs, sc->last_mbo);
    u16 ask_start = mbo->hi_bid_index == MAX_U16 ? 0 : mbo->hi_bid_index + 1;

    // the book sits on one side of the clearing price (it can't self-cross), so at most one of
    // these is nonzero. that side of the book is consumed first, ahead of the auction arrivals
    u32 book_bids_ge = 0, book_asks_le = 0;
    for (u16 i = 0; i < ask_start; i++)
        if (mbo->levels[i].price >= clearing)
            book_bids_ge += mbo->levels[i].quantity;
    for (u16 i = ask_start; i < mbo->level_count; i++)
        if (mbo->levels[i].price <= clearing)
            book_asks_le += mbo->levels[i].quantity;
    u32 book_bid_fill = book_bids_ge < matched ? book_bids_ge : matched;
    u32 book_ask_fill = book_asks_le < matched ? book_asks_le : matched;

    if (book_bid_fill > 0)
        auction_consume_book(sc, 0, book_bid_fill, clearing); // a market sell eats book bids
    if (book_ask_fill > 0)
        auction_consume_book(sc, 1, book_ask_fill, clearing); // a market buy eats book asks

    // the auction arrivals fill the rest of matched at the clearing price - the book already
    // took its share of its side, so subtract it there. the lighter side clears fully
    auction_fill_side(sc, matched - book_bid_fill, clearing, 1);
    auction_fill_side(sc, matched - book_ask_fill, clearing, 0);

    // the cross is a real print - at the close, the biggest of the day and the official last
    // price. move the mark and put it on the tape, like any trade (no aggressor, so direction 0)
    if (matched > 0) {
        sc->mark = clearing;
        update_trade(sc, matched, clearing, 0);
    }

    cb_clear(sc->auction_bid_sorted);
    cb_clear(sc->auction_ask_sorted);
    cb_clear(sc->auction_market_bids);
    cb_clear(sc->auction_market_asks);

    // the cross cleared all closing interest, so the imbalance resets for the next session
    sc->imbalance_buy = 0;
    sc->imbalance_sell = 0;
    cb_clear(sc->imbalances); // drop this session's published snapshots

    // remainder, in arrival order: hand it back to continuous via the convert -> sw path, where
    // it re-reserves and rests (or bounces off the closed gate at a close). a stranded market
    // order re-prices to the clearing price - a working price, so it rests as bounded interest
    // instead of eating the book at any price. filled and cancelled orders (quantity 0) fall out
    u8 queued = 0; // did we actually add residual? convert_holder may already hold a fired stop
    // auction-only interest either crosses here or dies here, so this drain is the only place
    // that sees both outcomes - counted so a cross reports whether the moc path engaged at all
    u32 only_crossed = 0, only_cancelled = 0;
    while (!cb_is_empty(sc->auction_arrivals)) {
        u32 id = *(u32*)cb_deque(sc->auction_arrivals);
        Order* o = (Order*)fl_get(sc->orders, id);
        if (o->quantity == 0) {
            only_crossed += (o->status >> AUCTION_ONLY_BIT) & 1;
            continue;
        }

        // auction-only: cross or cancel, no continuous life to fall back to
        if ((o->status >> AUCTION_ONLY_BIT) & 1) {
            only_cancelled++;
            o->status |= (1 << REJECT_BIT);
            o->quantity = 0;
            schedule_response(sc, o->client_id, (1 << REJECT_BIT), 0, id, 0, CXL_IOC_UNFILLED);
            continue;
        }

        // a stranded market re-prices to the clearing price so it rests as bounded interest
        if ((o->status >> IS_MARKET_BIT) & 1) {
            o->price = clearing;
            o->status &= ~(1 << IS_MARKET_BIT);
        }
        cb_queue(sc->convert_holder, &id);
        queued = 1;
    }

    if (only_crossed || only_cancelled)
        // "residual" covers both the untouched and the partially crossed - either way the rest dies
        printf("AUCTION only-orders: %u fully crossed, %u cancelled with residual\n", only_crossed, only_cancelled);

    // cap our residual batch with a sentinel and schedule the drain, same as the continuous path.
    // only if we actually queued - convert_holder can already hold an unrelated fired-stop batch
    if (queued) {
        u32 sentinel = CONVERT_SENTINEL_VALUE;
        cb_queue(sc->convert_holder, &sentinel);
        sch_schedule(sc->sch, build_event(SERVER_TYPE, EXEC_TO_SW_ID), 100);
    }
}

// much better
// the big driver of all market book stuff
// this mostly takes care of scheduling, then passes it off to server_order
void server_order(ServerContext* sc, u32 exec_order_id) {

    Order* pending = (Order*)fl_get(sc->orders, exec_order_id);
    u8 pending_ws = (pending->status >> WS_BIT) & 1;
    u8 pending_ping = (pending->status >> PING_BIT) & 1;
    u32 real_order_mask = (1 << ASK_BID_PAIR_BIT) | (1 << CANCEL_BIT) | (1 << CAN_REP_BIT);

    // a ping / ws toggle was never an order, so it clears ahead of the auction diversion - it has
    // nothing to park at a cross, and the auction path would bounce it as an invalid quantity
    if ((pending_ws | pending_ping) && pending->quantity == 0 && !(pending->status & real_order_mask)) {
        ClientSettings* pcs = sc->client_settings + pending->client_id;
        u32 ctl = (1 << CONTROL_BIT) | (pending_ping << PING_BIT);
        u8 why = REASON_NONE;

        if (pending_ws && pcs->sub_tier == TIER_FREE) {
            // free tier gets no live stream, only last trade price on demand
            ctl |= (1 << REJECT_BIT);
            why = REJ_NO_WS_ACCESS;
        } else if (pending_ws) {
            pcs->ws = !(pcs->ws);
            stream_roster_rebuild(sc);
            ctl |= (1 << WS_BIT);
        }

        schedule_response(sc, pending->client_id, ctl, 0, exec_order_id, 0, why);
        return;
    }

    // auction-only orders (MOC/LOC, and their auction-only cancels) are held for the next cross
    // whenever they arrive - which cross is decided by where they land: pre-open ones cross at the
    // open, ones entered while the market is live cross at the close. everything else only diverts
    // during the pre-open window; the closing window keeps the book live so regular orders trade on
    u8 auction_only = (pending->status >> AUCTION_ONLY_BIT) & 1;
    if (auction_only || (sc->auctioning && !sc->is_open)) {
        server_auction_order(sc, exec_order_id);
        return;
    }

    // maybe I rename at least the struct names
    //u32 last_mbo = sc->last_mbo;
    ClientSettings* client_settings = sc->client_settings;
    BS* mbo_bs = sc->mbo_bs;
    SCH* sch = sc->sch;
    Holder* ho = sc->ho;
    FL* orders = sc->orders;
    CB* fills = sc->fills;
    u64 now_ns = sch_now_ns(sch);

    Order* in = (Order*)fl_get(orders, exec_order_id);

    ClientSettings* cs = (client_settings + in->client_id);

    u32 status = 0;
    u8 rej_reason = 0;

    // for now we'll just handle socket connections
    u8 is_toggle_ws = (in->status & (1 << WS_BIT));
    if (is_toggle_ws && cs->sub_tier == TIER_FREE) {
        // free tier gets no live stream, only last trade price on demand
        status |= (1 << REJECT_BIT);
        rej_reason = REJ_NO_WS_ACCESS;
    } else if (is_toggle_ws) {
        cs->ws = !(cs->ws);
        stream_roster_rebuild(sc);
        status |= (1 << WS_BIT);
    }
    u8 is_ping = (in->status >> PING_BIT) & 1;
    if (is_ping)
        status |= 1 << PING_BIT;

    // we should have prechecks for cancel orders too

    u8 is_pair = (in->status >> ASK_BID_PAIR_BIT) & 1;

    // an atomic pair arrives as one order: bid in the primary fields, ask in second_*.
    // materialize the ask as its own order so it gets a real id to rest / fill / cancel under.
    // everything past here is shared with the single-order path, branching only where it differs.
    u32 ask_order_id = MAX_U32;
    Order* ask_in = 0;
    if (is_pair) {
        Order ask_seed = {};
        ask_seed.client_id = in->client_id;
        ask_seed.price = in->second_price;
        ask_seed.quantity = in->second_quantity;
        // this (and the bid coutnerpart) should be set to invalid if we dont find it, but allow to proceed
        ask_seed.other_id = in->second_id;
        // ask is a sell; keep the replace/cancel intent, drop the pair bit (this leg is a plain canrep)
        ask_seed.status = in->status & ~((1 << BUY_DIRECTION_BIT) | (1 << ASK_BID_PAIR_BIT));
        ask_order_id = fl_insert(orders, &ask_seed);
        in = (Order*)fl_get(orders, exec_order_id); // fl_insert may have moved the pool
        ask_in = (Order*)fl_get(orders, ask_order_id);
        status |= (1 << ASK_BID_PAIR_BIT);
    }


    u8 is_can_rep = (in->status >> CAN_REP_BIT) & 1;   // for a pair, this is the bid leg
    u8 is_cancel = (in->status >> CANCEL_BIT) & 1;
    u8 ask_is_can_rep = is_pair ? ((ask_in->status >> CAN_REP_BIT) & 1) : 0;
    // create leaves second_id at MAX_U32; a minted slice carries its iceberg id
    u8 is_iceberg = (in->status >> ICEBERG_BIT) & 1;
    u8 is_iceberg_replenish = is_iceberg && in->second_id != MAX_U32;
    // still armed. a triggered stop comes back through here with the bit already cleared
    u8 is_stop = (in->status >> HAS_STOP_BIT) & 1;
    // gtc is the default: no tif bit set rests the remainder, like a plain limit always did
    u8 is_gtc = !(((in->status >> IOC_BIT) & 1) | ((in->status >> FOK_BIT) & 1));

    // a wake is like a ping - no quantity, never reaches the book - but instead of acking and
    // leaving it rests in a trigger heap keyed by its price and keeps its slot, so the client
    // can cancel it by id. the wake bit is exclusive: it owns every order carrying it, so a
    // cancel/replace/pair/stop/iceberg riding along is malformed and rejected here. to cancel
    // a wake the client sends a plain cancel (no wake bit) at its id, which skips this branch
    u8 is_wake = (in->status >> WAKE_BIT) & 1;
    if (is_wake) {
        u8 conflict = is_cancel | is_can_rep | is_pair | is_stop | is_iceberg;
        if (conflict || in->price == 0) {
            in->status |= (1 << REJECT_BIT);
            u8 why = conflict ? REJ_BAD_QUALIFIER : REJ_INVALID_PRICE;
            schedule_response(sc, in->client_id, (1 << WAKE_BIT) | (1 << REJECT_BIT), 0, exec_order_id, 0, why);
            return;
        }
        wake_rest(sc, exec_order_id);
        schedule_response(sc, in->client_id, (1 << WAKE_BIT), 0, exec_order_id, in->price, REASON_NONE);
        return;
    }

    void* old_mbo_raw = bs_get_no_ref(mbo_bs, sc->last_mbo);

    u32 fillable = 0; // how much an add crosses on arrival; the iceberg create sizes off it

    if (!sc->is_open) {
        rej_reason = REJ_MARKET_CLOSED;
    } else if (is_pair) {
        rej_reason = pair_precheck(sc, in, ask_in, cs, orders, (MBO*)old_mbo_raw, sc->mark);
    } else if (is_can_rep) {
        rej_reason = canrep_precheck(sc, in, cs, orders, (MBO*)old_mbo_raw, sc->mark);
        status |= (1 << CAN_REP_BIT);
        if (is_stop)
            status |= (1 << HAS_STOP_BIT);
    } else if (is_cancel) {
        rej_reason = cancel_precheck(in, orders, (MBO*)old_mbo_raw);
        status |= (1 << CANCEL_BIT);
        // a stray stop half on a pure cancel was never validated - ignore it
        is_stop = 0;
    } else if (is_iceberg_replenish) {
        // a minted slice is already funded and reserved from create time; it just rests
    } else if (is_stop) {
        rej_reason = stop_precheck(in);
        // quantity > 0 means there's a NOW half - a real order executing right now, so it
        // validates and funds like one. either half failing rejects the whole message
        if (!rej_reason && in->quantity > 0)
            rej_reason = add_precheck(sc, in, cs, (MBO*)old_mbo_raw, sc->mark, 0, 0, &fillable);
        status |= (1 << HAS_STOP_BIT);
    } else {
        rej_reason = add_precheck(sc, in, cs, (MBO*)old_mbo_raw, sc->mark, 0, 0, &fillable);
    }

    // honestly if we truly have a method that can handle everything, we can send it here
    // but we do need to do prechecks
    // which could certainly split out into other methods


    u8 is_buy = (in->status >> BUY_DIRECTION_BIT) & 1;
    u32 before_quantity = in->quantity;
    u32 before_ask_quantity = is_pair ? ask_in->quantity : 0;

    // next big challenge cancelreplace orders
    // so now we need like a cancel id to bundle into this
    // may as well use the padding we have

    // and we needyet another flag
    // more interestingly, what will this look like in the actual OB


    // you can have a "ping order", so order without the websocket stuff

    //u8 is_pure_get = !is_valid_quantity & is_ping;

    /*

       if(is_buy && !has_bp) 
       printf("REJ $%u > $%u\n", in_cost, cs->cash - cs->reserved_cash);

       if(!is_buy && !has_shares) 
       printf("REJ %u > %u\n", before_quantity, cs->shares - cs->reserved_shares);
     */

    if (rej_reason){
        status |= (1<<REJECT_BIT);
        // best practice to set q to 0
        in->quantity = 0;
        in->status |= (1<<REJECT_BIT);

        if (is_pair) {
            // undo the speculative ask leg; report both legs rejected in one response
            fl_release(orders, ask_order_id);
            schedule_pair_response(sc, in->client_id, status, exec_order_id, 0, 0, MAX_U32, 0, 0, rej_reason);
        } else {
            schedule_response(sc, in->client_id, status, 0, exec_order_id, 0, rej_reason);
        }
        return;
    }

    // arrival stamp - ids recycle, this doesn't. stop triggers sort same-price groups by it
    in->ns = now_ns;

    // a fresh iceberg. a marketable one swallows all the liquidity it crosses now and rests
    // a tip on top (fillable + chunk), the rest hidden - so the hidden half is always worked
    // from the resting side, never stranded. chunk is frozen before the cross shrinks it.
    // nothing rests only when the whole total crosses (fillable == total), so no struct then
    if (is_iceberg) {
        status |= (1 << ICEBERG_BIT);
        if (!is_iceberg_replenish) {
            u32 total = in->second_quantity;
            u32 chunk = in->quantity;
            u32 visible = fillable + chunk < total ? fillable + chunk : total;
            in->quantity = visible;
            before_quantity = visible;
            if (fillable < total) {
                Iceberg ice = { .client_id = in->client_id, .remaining = total - visible, .price = in->price, .chunk = chunk };
                in->second_id = fl_insert(sc->icebergs, &ice);
            }
        }
    }

    // captured, not printed - see LogRec. the account fields are snapshotted here because the
    // line reports them as of acceptance and cs moves on immediately afterwards.
    // -DLOG_TRADES_ONLY skips LOG_ORDER records entirely (most of the ~26M/run) when only the
    // LOG_TRADE lines are needed downstream; off by default so behavior is unchanged
#ifndef LOG_TRADES_ONLY
    LogRec lr = {
        .kind = LOG_ORDER,
        .now_ns = now_ns,
        .order_id = exec_order_id,
        .client_id = in->client_id,
        .cash = cs->cash,
        .reserved_cash = cs->reserved_cash,
        .shares = cs->shares,
        .reserved_shares = cs->reserved_shares,
        .other_id = in->other_id,
        .status = in->status,
        .price = in->price,
        .quantity = in->quantity,
        .stop_price = in->stop_price,
        .second_quantity = in->second_quantity,
        .second_direction = in->second_direction,
    };
    cb_queue(sc->log, &lr);
#endif


    // ok at this point I think can now handle stop orders

    // the idea is do some quick validation on the stop order
    // let me sleep on this

    // cancelling an armed stop or a wake needs no snapshot: no reserve to return and nothing
    // in the book. the stop has quantity to zero, the wake only its reject-bit tombstone -
    // either way the heap entry left behind dies on the fire guards
    if (is_cancel) {
        Order* cancelled = (Order*)fl_get(orders, in->other_id);
        if ((cancelled->status >> HAS_STOP_BIT) & 1) {
            cancelled->quantity = 0;
            schedule_response(sc, in->client_id, status, 0, exec_order_id, cancelled->stop_price, REASON_NONE);
            return;
        }
        if ((cancelled->status >> WAKE_BIT) & 1) {
            cancelled->status |= (1 << REJECT_BIT);
            schedule_response(sc, in->client_id, status, 0, exec_order_id, cancelled->price, REASON_NONE);
            return;
        }
    }

    // canrep whose target is an armed stop: same tombstone cancel as above, so the
    // replacement carries on below as a fresh order (plain, stop-only, or combined)
    if (is_can_rep) {
        Order* cancelled = (Order*)fl_get(orders, in->other_id);
        if ((cancelled->status >> HAS_STOP_BIT) & 1) {
            cancelled->quantity = 0;
            in->status &= ~(1 << CAN_REP_BIT);
            is_can_rep = 0;
        }
    }

    // stop-only replacement of a book order: the slot has to stay a cancel op for the
    // book walk to splice the old order out, so it demotes to a pure cancel and the stop
    // half mints its own slot below, same as a combined order's would
    u8 canrep_stop_leg = 0;
    if (is_stop && is_can_rep && in->quantity == 0) {
        in->status = (in->status & ~(1 << CAN_REP_BIT)) | (1 << CANCEL_BIT);
        is_can_rep = 0;
        is_cancel = 1;
        canrep_stop_leg = 1;
    }

    // combined stop: the stop half moves to its own slot in canonical form - params in
    // the primary fields - so the trigger can convert it in place later. the NOW half
    // carries on below as a plain order
    u32 stop_order_id = MAX_U32;
    if (is_stop && (in->quantity > 0 || canrep_stop_leg)) {
        Order seed = {};
        seed.client_id = in->client_id;
        seed.quantity = in->second_quantity;
        seed.price = in->second_price;
        seed.stop_price = in->stop_price;
        seed.second_id = in->second_id; // a gtd expiry date rides along
        seed.ns = in->ns;
        seed.status = (1 << HAS_STOP_BIT)
            | (in->second_direction ? (1 << BUY_DIRECTION_BIT) : 0)
            | (in->status & ((1 << STOP_LIMIT_BIT) | (1 << DAY_BIT) | (1 << GTD_BIT)));
        stop_order_id = fl_insert(orders, &seed);
        in = (Order*)fl_get(orders, exec_order_id); // fl_insert may have moved the pool
        in->status &= ~((1 << HAS_STOP_BIT) | (1 << STOP_LIMIT_BIT));

        // oco bracket: wire the now half and its leg to each other so whichever completes
        // first pulls the other. no now half (canrep_stop_leg) means nothing to bracket
        if (((in->status >> OCO_BIT) & 1) && in->quantity > 0) {
            in->other_id = stop_order_id;
            Order* leg = (Order*)fl_get(orders, stop_order_id);
            leg->status |= (1 << OCO_BIT);
            leg->other_id = exec_order_id;
        }

        stop_rest(sc, stop_order_id);
    }

    // stop-only: no NOW half, so normalize into this same slot (the client keeps one id),
    // rest it, ack, and get out - the book is never touched
    if (is_stop && in->quantity == 0 && !canrep_stop_leg) {
        in->quantity = in->second_quantity;
        in->price = in->second_price;
        if (in->second_direction)
            in->status |= (1 << BUY_DIRECTION_BIT);
        else
            in->status &= ~(1 << BUY_DIRECTION_BIT);

        stop_rest(sc, exec_order_id);
        schedule_response(sc, in->client_id, status, 0, exec_order_id, in->stop_price, REASON_NONE);
        return;
    }


    // mbo_dump + used to create next snapshot
    // us, plus at least the one who sent request?

    u32 prev_last_mbo = sc->last_mbo;

    //if(exec_order_id > 2000000){
    //mbo_dump(mbo);
    //exit(1);
    //}

    // this WILL modify the in Order, to modify quantity
    // it's either that or we mess with the return value somehow

    u32 old_size = mbo_bs->metadata[sc->last_mbo].size;
    // new resting order on new price level, requring an additional index, additional level header, and additional level entry
    // a pair can rest two brand-new levels (bid and ask), a single order at most one
    u32 levels_added = is_pair ? 2 : 1;
    u32 max_new_size = old_size + levels_added * (sizeof(MBOIndex) + sizeof(MBOLevel) + sizeof(MBOEntry));

    void* new_mbo_raw;
    u32 next_last_mbo = bs_reserve(mbo_bs, max_new_size, 1, &new_mbo_raw);
    // critically, a doubling operation could make the previous pointer point to fuck all
    // we need to get again
    old_mbo_raw = bs_get_no_ref(mbo_bs, prev_last_mbo);

    u32 new_size = is_pair
        ? ob_pair(orders, exec_order_id, ask_order_id, old_mbo_raw, new_mbo_raw, fills)
        : ob_canrep(orders, exec_order_id, old_mbo_raw, new_mbo_raw, fills);

    // a pair crosses the book on at most one leg (bid<ask forbids both), so whatever fills
    // ob_pair queued belong to that one taker. find it by the same best-bid/ask test ob uses.
    Order* taker = in;
    // so we can refetch taker if fl_insert moves the pool under us (iceberg replenish)
    u32 taker_id = exec_order_id;
    u8 taker_is_buy = is_buy;
    if (is_pair) {
        MBO* om = (MBO*)old_mbo_raw;
        u16 hbi = om->hi_bid_index;
        u16 lo_ask = (hbi == MAX_U16) ? 0 : hbi + 1;
        if (lo_ask < om->level_count && in->price >= om->levels[lo_ask].price) {
            taker = in; taker_is_buy = 1;            // bid lifts asks
        } else if (hbi != MAX_U16 && ask_in->price <= om->levels[hbi].price) {
            taker = ask_in;                          // ask hits bids
            taker_id = ask_order_id;
            taker_is_buy = 0;
        }
    }
    //printf("new size %u\n", new_size);
    //if (exec_order_id == 24){
    //printf("old\n");
    //mbo_dump(old_mbo_raw);

    //printf("new\n");
    //mbo_dump(new_mbo_raw);
    //exit(1);
    //}

    sc->last_mbo = next_last_mbo;

    bs_resize(mbo_bs, new_size);

    // the book moved: tick the version and let the price tiers re-derive lazily. every
    // response scheduled below pins its own subscription's view through tier_blob_id, which
    // derives on first touch of the new version - so a maker still gets the fresh touch
    // alongside the fill that moved it, and a pass whose consumers are all mbo pays nothing
    sc->book_version++;

    // ^ but this is just for our client of incoming order
    // we still need to go through and fill the orders we hit
    // just dont update the incoming order client after this "taker"

    // ok now we have fills and partial_id maybe
    // partial id will be filled last by definiton

    u32 last_trade_price = MAX_U32;

    while (!cb_is_empty(fills)){
        // this guy is actuall responsible for ensuring "orders" fl is updated

        Fill* fill = (Fill*)cb_deque(fills);
        // its filled, we dont need it anymore I think
        // we could probably release, but it's confusing right now
        //printf("order releasing due to fill %u\n", filled_order_id);
        Order* order = (Order*)fl_get(orders, fill->order_id);

        u32 q = fill->quantity_filled;//order->quantity;
        u32 cost = order->price * q;

        last_trade_price = order->price;

        // later for trade table: we just need direction, price, quantity, time
        LogRec tr = {
            .kind = LOG_TRADE,
            .now_ns = now_ns,
            .order_id = fill->order_id,
            .quantity_filled = q,
            .price = order->price,
            .partial = fill->partial,
            .taker_is_buy = taker_is_buy,
        };
        cb_queue(sc->log, &tr);
        update_trade(sc, q, order->price, taker_is_buy);

        // the cancelled trade cannot show up here
        // in fact it shoudln't even be here
        // we should genuinely update teh order->quantity because cancels rely on it
        //printf("in order q before %u for id %u\n", in->quantity, exec_order_id);
        //printf("resting q before %u for id %u\n", order->quantity, fill->order_id);
        // quantity is a u16. an over-fill wraps it to ~65k here and the u32 filled below to
        // ~4.29bn, and both sides then settle that: the client's position model and the
        // engine's own. the book and the fills disagreeing is not something to trade through
        if (q > taker->quantity) {
            printf("over-fill: taker #%u has %ush left, fill #%u is %ush\n",
                    taker_id, taker->quantity, fill->order_id, q);
            exit(1);
        }

        order->quantity -= q;
        // its either do it here, or have the ob file take on even more responsibility for handling stuff
        taker->quantity -= q;
        //printf("in order q after %u for id %u\n", in->quantity, exec_order_id);
        //printf("resting q after %u for id %u\n", order->quantity, fill->order_id);

        u32 maker = order->client_id;

        ClientSettings* mcs = (client_settings + maker);

        if (taker_is_buy){
            cs->cash -= cost;
            cs->shares += q;
            mcs->cash += cost;
            mcs->shares -= q;
            mcs->reserved_shares -= q;
        } else {
            cs->shares -= q;
            cs->cash += cost;
            mcs->shares += q;
            mcs->cash -= cost;
            mcs->reserved_cash -= cost;
        }

        // maker-taker: the aggressor pays a per-share fee, the resting maker earns a rebate,
        // the exchange banks the spread
        u64 taker_fee = (u64)q * TAKER_FEE_MILLS / MILLS_PER_DOLLAR;
        u64 maker_rebate = (u64)q * MAKER_REBATE_MILLS / MILLS_PER_DOLLAR;
        cs->cash -= taker_fee;
        mcs->cash += maker_rebate;
        sc->exchange_cash += taker_fee - maker_rebate;

        u32 fstatus = 1 << FILL_BIT;
        if (fill->partial) {
            fstatus |= 1 << PARTIAL_FILL_BIT;
        }

        // a resting iceberg slice that just emptied replenishes here. read what we need off
        // order first, because iceberg_replenish's fl_insert can move the pool out from under it
        if (((order->status >> ICEBERG_BIT) & 1) && order->quantity == 0) {
            u16 fill_price = order->price;
            u8 maker_is_buy = (order->status >> BUY_DIRECTION_BIT) & 1;
            u32 slice_id = iceberg_replenish(sc, order->second_id, maker_is_buy);
            taker = (Order*)fl_get(orders, taker_id);
            // second_order_id carries the next slice's id (MAX_U32 if the iceberg is spent)
            schedule_pair_response(sc, maker, fstatus | (1 << ICEBERG_BIT), fill->order_id, fill_price, q, slice_id, 0, 0, REASON_NONE);
        } else {
            //printf("scheduling response %u\n", fill->order_id);
            schedule_response(sc, maker, fstatus, q, fill->order_id, order->price, REASON_NONE);
        }

        // a resting oco order that just fully filled pulls its stop partner
        order = (Order*)fl_get(orders, fill->order_id);
        if (order->quantity == 0 && ((order->status >> OCO_BIT) & 1)) {
            order->status &= ~(1 << OCO_BIT);
            oco_pull(sc, fill->order_id);
            taker = (Order*)fl_get(orders, taker_id); // oco_pull's fl_insert may move the pool
        }
    }

    // an iceberg replenish above may have moved the pool, so in/ask_in could be stale
    in = (Order*)fl_get(orders, exec_order_id);
    if (is_pair)
        ask_in = (Order*)fl_get(orders, ask_order_id);

    // gtc - leave whatever we have in the order, it rested
    // fok - order should be fully filled, by above loop
    // ioc - ob already dropped the residual, so wipe it here to match. the client still learns exactly
    // how much filled from quantity_filled on the response, and leaving it on would reserve
    // cash/shares below for quantity that isn't in the book
    // clamped, not just subtracted: quantity is a u16 and this is a u32, so a taker that
    // somehow came back larger than it went in would report a multi-billion-share fill
    u32 filled = before_quantity > in->quantity ? before_quantity - in->quantity : 0;
    u32 ask_filled = (is_pair && before_ask_quantity > ask_in->quantity)
            ? before_ask_quantity - ask_in->quantity : 0;
    if (!is_gtc)
        in->quantity = 0;

    // only after the loop above has in->quantity actually come down. whatever is left rested = partial
    if (filled){
        status |= (1 << FILL_BIT);
        if (in->quantity > 0)
            status |= (1 << PARTIAL_FILL_BIT);
    }

    // an ioc/fok that found nothing is a no-op, so call it a reject - same as a fok failing its
    // precheck outright. the client takes it back, and main frees the order rather than leaking it
    if (!is_gtc && !filled) {
        status |= (1 << REJECT_BIT);
        in->status |= (1 << REJECT_BIT);
        rej_reason = ((in->status >> FOK_BIT) & 1) ? CXL_FOK_KILLED : CXL_IOC_UNFILLED;
    }

    // a minted slice was already reserved at create; reserving again would double it
    if (!is_cancel && !is_iceberg_replenish) {
        // create reserves the whole iceberg up front, hidden half included
        u32 reserve_q = in->quantity;
        if (is_iceberg)
            reserve_q += (u32)(in->second_quantity - before_quantity);

        if (is_buy)
            cs->reserved_cash += reserve_q * in->price;
        else
            cs->reserved_shares += reserve_q;
    }

    // whatever just rested gets remembered so its time in force can pull it later: day at
    // this session's close, gtd once its date (carried in second_id) comes up
    if (in->quantity > 0 && !is_cancel && !is_can_rep && !is_pair && !is_iceberg_replenish) {
        if ((in->status >> DAY_BIT) & 1)
            cb_queue(sc->day_orders, &exec_order_id);
        else if ((in->status >> GTD_BIT) & 1)
            pq_push(sc->gtd, ((u64)in->second_id << 32) | exec_order_id);
    }

    // the now half of a bracket that fully filled on arrival (as taker) pulls its stop leg
    if (((in->status >> OCO_BIT) & 1) && in->quantity == 0 && !is_cancel) {
        in->status &= ~(1 << OCO_BIT);
        oco_pull(sc, exec_order_id);
        in = (Order*)fl_get(orders, exec_order_id); // oco_pull's fl_insert may move the pool
    }

    // the pair's bid reserve is handled above (is_buy); add the ask leg's share reserve.
    // a pure cancel pair added no ask, so there is nothing to reserve for
    if (is_pair && !is_cancel)
        cs->reserved_shares += ask_in->quantity;

    // honorary wipe out of cancel, assuming it was cancelled successfully
    if (is_can_rep | is_cancel){
        Order* cancelled = (Order*)fl_get(orders, in->other_id);

        if ((cancelled->status >> BUY_DIRECTION_BIT) & 1){
            // release cost
            // it must be  self btw
            cs->reserved_cash -= cancelled->quantity * cancelled->price;
        } else {
            cs->reserved_shares -= cancelled->quantity;
        }

        // cancelling a tip kills the whole iceberg - the tip's reserve just came off above,
        // now give back the hidden half too and free the struct
        if ((cancelled->status >> ICEBERG_BIT) & 1) {
            Iceberg* ice = (Iceberg*)fl_get(sc->icebergs, cancelled->second_id);
            if ((cancelled->status >> BUY_DIRECTION_BIT) & 1)
                cs->reserved_cash -= (u32)(ice->remaining * ice->price);
            else
                cs->reserved_shares -= (u32)ice->remaining;
            fl_release(sc->icebergs, cancelled->second_id);
        }

        cancelled->quantity = 0;
        // the client is never told about this one - the canrep/cancel ack it does get is about
        // the new order - so nobody downstream is going to hand the slot back for us. ob_canrep
        // above already spliced it out of the book, so this is the last reference to it
        cancelled->status |= (1 << REJECT_BIT);
        fl_release(orders, in->other_id);
    }

    // the block above retired the bid's replaced order; do the same for the ask leg.
    // ask_is_can_rep is the replace case, is_cancel the pull-both-quotes case
    if (is_pair && (ask_is_can_rep | is_cancel)) {
        Order* old_ask = (Order*)fl_get(orders, ask_in->other_id);

        // a pair naming the same order in both legs passes both prechecks and lands here twice
        u8 already_retired = (old_ask->status >> REJECT_BIT) & 1;

        // normally a sell, but dont assume - cancel_precheck only proved it was ours and resting
        if ((old_ask->status >> BUY_DIRECTION_BIT) & 1)
            cs->reserved_cash -= old_ask->quantity * old_ask->price;
        else
            cs->reserved_shares -= old_ask->quantity;

        old_ask->quantity = 0;
        old_ask->status |= (1 << REJECT_BIT);
        // releasing twice would put the id in the free stack twice, and it would later be minted
        // to two live owners at once - fl_release does not refuse this, so the guard is here
        if (!already_retired)
            fl_release(orders, ask_in->other_id);
    }

    // ok unfortunately we do need to do a a few assertions to make sure of stuff before we refactor
    // this explicitly relies on the 2 client $10000000 1000sh initial setup


    // the mbp tiers were derived off this same book right after it was built, above

    bs_get(mbo_bs, prev_last_mbo);
    //mbo_dump(bs_get_no_ref(mbo_bs, sc->last_mbo));

    // ok at this point we might've triggered a few stop orders
    // i think this is the easy part
    // check last price (
    if (last_trade_price != MAX_U32) {
        // what margin marks LMV/SMV against, so it has to move before any maintenance check
        sc->mark = last_trade_price;

        // the mark moved, so anyone on margin could have just gone under.
        // TODO: liquidate. synthesize a market order against the position (sell if long,
        // buy to cover if short) into convert_holder, and let server_exec_to_sw feed it
        // back into the sw queue - same path the stops will take. for now just say who
        for (u32 ci = 0; ci < ho->num_clients; ci++) {
            ClientSettings* mc = client_settings + ci;
            if (client_maint_call(mc, sc->mark))
                printf("MARGIN CALL client #%u [$%lld/%lldsh] mark $%u\n", ci, mc->cash, mc->shares, sc->mark);
        }

        // pop triggered stops into the convert holder; exec_end sees it non-empty and
        // schedules the drain back into the sw queue, same trip the iceberg slices take
        u32 fired_start = cb_count(sc->convert_holder);
        check_stops(sc, (u16)last_trade_price);
        // a fired oco stop pulls its resting partner right here at the trigger
        oco_sweep(sc, fired_start);
        // same print wakes any price alert it crossed - no book effect, so it stands apart
        check_wakes(sc, (u16)last_trade_price);
    }


    // i know its ugly

    // send special one to self
    //printf("scheduling response %u\n", exec_order_id);
    if (is_pair)
        // both legs, one delivery, each with its own filled quantity
        schedule_pair_response(sc, in->client_id, status, exec_order_id, in->price, filled, ask_order_id, ask_in->price, ask_filled, rej_reason);
    else if (is_stop)
        // combined stop: the NOW half's result plus the resting leg's id in one delivery
        schedule_pair_response(sc, in->client_id, status, exec_order_id, in->price, filled, stop_order_id, in->stop_price, 0, rej_reason);
    else
        schedule_response(sc, in->client_id, status, filled, exec_order_id, in->price, rej_reason);

    // per-tier market-data fan-out (mbo/mbp/mbp10/mbp1 + trade) to their ws subscribers
    server_stream(sc);
}

// a resting order whose time in force just ran out: give back its reserve, tell the client,
// and queue it (keyed by price) for the batch book prune. guarded against stale ids - the
// slot must still hold a live order carrying tif_bit, and a gtd order must also match the
// date its heap entry fired under (second_id carries it), else the id was recycled
void collect_expire(ServerContext* sc, u32 order_id, u8 tif_bit, u32 date) {
    Order* o = (Order*)fl_get(sc->orders, order_id);

    if (o->quantity == 0 || ((o->status >> REJECT_BIT) & 1) || !((o->status >> tif_bit) & 1))
        return;

    if (tif_bit == GTD_BIT && o->second_id != date)
        return;

    // still armed: expire it out of its trigger heap. nothing was reserved and nothing is
    // in the book, so the response + tombstone is the whole job. the heap entry left
    // behind dies on the fire guards
    if ((o->status >> HAS_STOP_BIT) & 1) {
        schedule_response(sc, o->client_id, (1 << REJECT_BIT) | (1 << CANCEL_BIT) | (1 << HAS_STOP_BIT), 0, order_id, o->stop_price, CXL_SESSION_CLOSE);
        o->status |= (1 << REJECT_BIT);
        o->quantity = 0;
        return;
    }

    // a stop that fired at the last second: converted, but still riding the sw queue, so
    // there's no reserve to give back and nothing to prune. leave it alone - it bounces
    // off the closed-market gate by itself, and touching it here would respond twice.
    // resting orders are always in the book, so this is the only order this misses
    if (ob_queue_position(o->price, order_id, bs_get_no_ref(sc->mbo_bs, sc->last_mbo)) == MAX_U32)
        return;

    ClientSettings* cs = sc->client_settings + o->client_id;
    u8 is_buy = (o->status >> BUY_DIRECTION_BIT) & 1;
    if (is_buy)
        cs->reserved_cash -= o->quantity * o->price;
    else
        cs->reserved_shares -= o->quantity;

    // an expiring iceberg tip drags its hidden half back too, then frees the struct
    if ((o->status >> ICEBERG_BIT) & 1) {
        Iceberg* ice = (Iceberg*)fl_get(sc->icebergs, o->second_id);
        if (is_buy)
            cs->reserved_cash -= (u32)(ice->remaining * ice->price);
        else
            cs->reserved_shares -= (u32)ice->remaining;
        fl_release(sc->icebergs, o->second_id);
    }

    pq_push(sc->price_pq, ((u64)o->price << 32) | order_id);

    // rides on the reject bit until there's a cancelled bit, same as an ioc/fok pull
    schedule_response(sc, o->client_id, (1 << REJECT_BIT) | (1 << CANCEL_BIT), 0, order_id, o->price, CXL_SESSION_CLOSE);

    o->status |= (1 << REJECT_BIT);
    o->quantity = 0;
}

// one new snapshot with every collected expiry spliced out of the book
void server_prune_book(ServerContext* sc) {
    // heap pops come out ascending, and expire_cb starts empty, so its buffer ends up a
    // flat sorted array
    u32 n = sc->price_pq->current - 1;
    for (u32 i = 0; i < n; i++) {
        u64 key = pq_pop(sc->price_pq);
        cb_queue(sc->expire_cb, &key);
    }

    BS* mbo_bs = sc->mbo_bs;
    u32 prev_last_mbo = sc->last_mbo;

    // pruning only removes, so the old byte size is a safe ceiling
    u32 old_size = mbo_bs->metadata[prev_last_mbo].size;
    void* new_mbo_raw;
    u32 next_last_mbo = bs_reserve(mbo_bs, old_size, 1, &new_mbo_raw);
    void* old_mbo_raw = bs_get_no_ref(mbo_bs, prev_last_mbo);

    u32 new_size = ob_expire(sc->expire_cb, n, old_mbo_raw, new_mbo_raw);

    sc->last_mbo = next_last_mbo;
    bs_resize(mbo_bs, new_size);

    // leave expire_cb empty for the next close
    for (u32 i = 0; i < n; i++)
        cb_deque(sc->expire_cb);


    sc->book_version++;


    //void* new_mbp_raw;
    //u32 next_last_mbp = bs_reserve(sc->mbp_bs, mbp_derive_size(new_mbo_raw), 1, &new_mbp_raw);
    //mbp_derive(new_mbp_raw, new_mbo_raw);
    //
    //bs_get(sc->mbp_bs, sc->last_mbp);
    //sc->last_mbp = next_last_mbp;

    bs_get(mbo_bs, prev_last_mbo);
}

// start an accumulation window: adds park until the cross. it schedules only the freeze, which
// in turn schedules the bell - so each event in the chain schedules just the next one
void server_auction_accumulate(ServerContext* sc) {
    sc->auctioning = 1;
    sc->auction_frozen = 0;
    printf("[%llus] AUCTION ACCUMULATING\n", sch_now_ns(sc->sch)/S_TO_NS);

    // opening and closing windows differ; is_open tells them apart (still closed = opening)
    u64 window = sc->is_open ? AUCTION_CLOSE_WINDOW_NS : AUCTION_OPEN_WINDOW_NS;
    u64 freeze = sc->is_open ? AUCTION_CLOSE_FREEZE_NS : AUCTION_OPEN_FREEZE_NS;
    sch_schedule(sc->sch, build_event(CONTROL_TYPE, CONTROL_PARAM_AUCTION_FREEZE), window - freeze);
}

// the freeze: cancels stop, and it schedules the coming bell - a close if we're mid-session,
// otherwise the open. so accumulate -> freeze -> bell, one link at a time
void server_auction_freeze(ServerContext* sc) {
    sc->auction_frozen = 1;
    u32 bell = sc->is_open ? CONTROL_PARAM_CLOSE : CONTROL_PARAM_OPEN;
    u64 freeze = sc->is_open ? AUCTION_CLOSE_FREEZE_NS : AUCTION_OPEN_FREEZE_NS;
    sch_schedule(sc->sch, build_event(CONTROL_TYPE, bell), freeze);
}

void server_market_open(ServerContext* sc) {
    server_auction(sc); // cross the accumulated opening orders, then hand off to continuous
    sc->auctioning = 0;
    sc->auction_frozen = 0;
    sc->is_open = 1;
    printf("[%llus] MARKET OPEN\n", sch_now_ns(sc->sch)/S_TO_NS);

    // the closing accumulation starts a window before the close bell
    sch_schedule(sc->sch, build_event(CONTROL_TYPE, CONTROL_PARAM_AUCTION_CLOSE), OPEN_TO_CLOSE_NS - AUCTION_CLOSE_WINDOW_NS);
    // start the per-second candle-close loop; it self-reschedules until the market closes
    sch_schedule(sc->sch, build_event(CONTROL_TYPE, CONTROL_PARAM_CANDLE), S_TO_NS);
}

void server_market_close(ServerContext* sc) {
    server_auction(sc); // cross the accumulated closing orders before the book goes dark
    sc->auctioning = 0;
    sc->auction_frozen = 0;
    sc->is_open = 0;
    printf("[%llus] MARKET CLOSE\n", sch_now_ns(sc->sch)/S_TO_NS);

    // day orders last exactly one session
    while (!cb_is_empty(sc->day_orders)) {
        u32 order_id = *(u32*)cb_deque(sc->day_orders);
        collect_expire(sc, order_id, DAY_BIT, 0);
    }

    // gtd orders fire once their date is here. the heap is date-keyed, so stop at the
    // first one still in the future
    u32 today = sch_now_ns(sc->sch) / DAY_TO_NS;
    while (!pq_is_empty(sc->gtd)) {
        if ((pq_peek(sc->gtd) >> 32) > today)
            break;

        u64 entry = pq_pop(sc->gtd);
        collect_expire(sc, (u32)(entry & MAX_U32), GTD_BIT, (u32)(entry >> 32));
    }

    if (!pq_is_empty(sc->price_pq))
        server_prune_book(sc);

    // the next opening accumulation starts a window before the open bell. fridays jump the weekend
    u64 gap = CLOSE_TO_OPEN_NS - AUCTION_OPEN_WINDOW_NS;
    if (today % 7 == FRIDAY_MOD)
        gap += WEEKEND_NS;
    sch_schedule(sc->sch, build_event(CONTROL_TYPE, CONTROL_PARAM_AUCTION_OPEN), gap);
}

// monthly data fee by sub_tier, indexed by the TIER_* order in server.h. raw feeds are the real
// nyse proprietary access fees: mbo=Integrated $8400, mbp=OpenBook $5000, mbp1=BBO $1500,
// trade=Trades $1500 (mbp10 has no exact product, interpolated). candles are the vendor-derived
// aggregate range - polygon/databento run ~$199/mo for real-time bars down to ~$29 for eod.
// note: a free tier (no entry here) never joins a ws broadcast roster and pays nothing - it can
// still pull the last price on demand, like the robinhood app, but gets no streaming feed
static const u64 DATA_FEE_BY_TIER[] = {
    8400, // TIER_MBO   NYSE Integrated, full order-by-order depth
    5000, // TIER_MBP   NYSE OpenBook, aggregated depth
    3000, // TIER_MBP10 ten levels, interpolated
    1500, // TIER_MBP1  NYSE BBO, top of book
    1500, // TIER_TRADE NYSE Trades tape
    199,  // TIER_CANDLE_SEC real-time bars, polygon advanced tier
    120,  // TIER_CANDLE_MIN
    60,   // TIER_CANDLE_HR
    29,   // TIER_CANDLE_DAY eod bars, polygon starter tier
    0,    // TIER_IMBALANCE add-on, billed separately below - never a base sub_tier
    0,    // TIER_FREE   last trade price only, no charge
};

// NOII imbalance feed, the orthogonal add-on. real NYSE Order Imbalances access is $500/mo
static const u64 NOII_FEE_MONTHLY = 500;

// monthly market-data bill: each client pays for its subscription tier, owed whether or not it
// is connected right now. the free tier's fee is 0. flows to the exchange
void server_eom(ServerContext* sc) {
    for (u32 i = 0; i < sc->ho->num_clients; i++) {
        ClientSettings* cs = sc->client_settings + i;
        u64 fee = DATA_FEE_BY_TIER[cs->sub_tier];
        if (cs->noii) // add-on rides on top of the base subscription
            fee += NOII_FEE_MONTHLY;
        cs->cash -= fee;
        sc->exchange_cash += fee;
    }
}

// one calendar day of short-borrow interest on every short, marked at the last print, 360-day
// convention. flows from the short seller to the exchange
void server_eod(ServerContext* sc) {
    for (u32 i = 0; i < sc->ho->num_clients; i++) {
        ClientSettings* cs = sc->client_settings + i;
        if (cs->shares >= 0)
            continue;

        u64 short_value = (u64)sc->mark * (u64)(-cs->shares);
        u64 borrow = short_value * SHORT_BORROW_ANNUAL_BPS / (BPS_DIVISOR * BORROW_DAY_BASIS);
        cs->cash -= borrow;
        sc->exchange_cash += borrow;
    }
}

// a bar of `duration` just ended: if one actually formed in the period that ended (its bucket
// start is exactly now - duration), it's final, so hand it to that tier's subscribers.
static void emit_closed_candle(ServerContext* sc, CB* candles, u64 duration, u8 tier) {
    Candle* bar = (Candle*)cb_last(candles);
    if (!bar || bar->time != sch_now_ns(sc->sch) - duration)
        return; // no trade in the period that just ended, nothing new to close

    // the just-closed bar is the tail; hand its offset to every ws client subscribed to this tier
    u32 offset = cb_count(candles) - 1;
    u32 end = sc->live_offset[tier + 1];
    for (u32 i = sc->live_offset[tier]; i < end; i++)
        schedule_stream_response(sc, *(u32*)cb_at(sc->live_roster, i), tier, offset);
}

// fired once a second while the market is open. the second just ended, and any longer duration
// whose boundary we landed on ended too (a longer boundary is always a shorter one), so cascade.
// outside trading hours we stop rescheduling - server_market_open restarts the loop each session.
void server_candle_close(ServerContext* sc) {
    if (!sc->is_open)
        return;

    u64 now = sch_now_ns(sc->sch);
    emit_closed_candle(sc, sc->candles_sec, S_TO_NS, TIER_CANDLE_SEC);
    if (now % MIN_TO_NS == 0)
        emit_closed_candle(sc, sc->candles_min, MIN_TO_NS, TIER_CANDLE_MIN);
    if (now % H_TO_NS == 0)
        emit_closed_candle(sc, sc->candles_hr, H_TO_NS, TIER_CANDLE_HR);
    if (now % DAY_TO_NS == 0)
        emit_closed_candle(sc, sc->candles_day, DAY_TO_NS, TIER_CANDLE_DAY);

    // reuse this 1s tick to publish the NOII while the closing window runs (the opening window is
    // pre-open, so is_open gates it out for now). the running totals are already maintained, so
    // this is O(subscribers). indicative clearing price comes later, recomputed only on this tick
    if (sc->auctioning) {
        u32 buy = sc->imbalance_buy;
        u32 sell = sc->imbalance_sell;

        // read-only cross for the indicative clearing price and matched volume. release=0 leaves
        // reserves and orders untouched; auction_walk borrows the heaps/fifos/scratch, so give
        // them back empty (the pop drained the heaps, we clear the fifos and sorted scratch)
        u16 clearing;
        u32 matched;
        auction_walk(sc, 0, &clearing, &matched);
        cb_clear(sc->auction_market_bids);
        cb_clear(sc->auction_market_asks);
        cb_clear(sc->auction_bid_sorted);
        cb_clear(sc->auction_ask_sorted);

        Imbalance imb = {
            .ns = now,
            .ref_price = sc->mark,
            .clearing = clearing,
            .paired = matched,
            .imbalance = buy > sell ? buy - sell : sell - buy,
            .buy_side = buy >= sell,
        };
        cb_queue(sc->imbalances, &imb);
        u32 offset = cb_count(sc->imbalances) - 1;
        for (u32 c = 0; c < sc->ho->num_clients; c++)
            if (sc->client_settings[c].noii && sc->client_settings[c].ws)
                schedule_stream_response(sc, c, TIER_IMBALANCE, offset);
    }

    // snap to the next whole second - a slow-scheduled open lands off-boundary, and a flat +1s
    // would carry that jitter forever, so bar->time (bucket-aligned) never lines up with now
    sch_schedule(sc->sch, build_event(CONTROL_TYPE, CONTROL_PARAM_CANDLE), S_TO_NS - now % S_TO_NS);
}

void server_exec_end(ServerContext* sc) {
    SCH* sch = sc->sch;
    CB* sw_queue = sc->sw_queue;

    if (cb_is_empty(sw_queue)) {
        //really weird
        sc->executing = 0;
        return;
    }

    u32 exec_order_id = *(u32*)cb_deque(sw_queue);

    server_order(sc, exec_order_id);

    // server_order may have queued iceberg slices into convert_holder. cap the batch with a
    // sentinel and schedule the drain into the sw queue - the same path stops will use
    if (!cb_is_empty(sc->convert_holder)) {
        u32 sentinel = CONVERT_SENTINEL_VALUE;
        cb_queue(sc->convert_holder, &sentinel);

        u64 SW_TO_EXEC_DELAY = 100;
        sch_schedule(sch, build_event(SERVER_TYPE, EXEC_TO_SW_ID), SW_TO_EXEC_DELAY);
    }

    if (cb_is_empty(sw_queue)){
        sc->executing = 0;
    } else {
        u64 EXEC_TIME = 10;
        u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_END_ID & PARAM_MASK);
        sch_schedule(sch, socket_event, EXEC_TIME);
    }

}


void server_arrival(ServerContext* sc, u32 order_id) {
    //printf("order %llu arrives at server\n", order_id);
    cb_queue(sc->hw_queue, &order_id);

    u64 HW_TO_SW_DELAY = 10000;
    u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (HW_TO_SW_ID & PARAM_MASK);
    sch_schedule(sc->sch, socket_event, HW_TO_SW_DELAY);
}

void server_exec_start(ServerContext* sc) {
    // This relies on the server->executing state and
    // the fact that an EXEC_END_ID is scheduled are in sync

    // cb is empty and executing - on the last order
    // cb is empty and not executing - do nothing
    // cb not empty and executing - don't do anything
    // cb not empty and not executing - do somethign

    if (sc->executing || cb_is_empty(sc->sw_queue)) {
        // do nothing
        return;
    }

    //printf("exec started\n");

    sc->executing = 1;

    u64 EXEC_TIME = 10;
    u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_END_ID & PARAM_MASK);
    sch_schedule(sc->sch, socket_event, EXEC_TIME);
}

void server_hw_to_sw(ServerContext* sc) {
    if (cb_is_empty(sc->hw_queue)) {
        //weird
        return;
    }   
    u32 moving_order = *(u32*)cb_deque(sc->hw_queue); 
    // handle zero case
    //printf("hw to sw move requested for order %u\n", moving_order);

    // If it's not empty, someone has already scheduled an exec_start_id
    if (cb_is_empty(sc->sw_queue)) {
        u64 SW_TO_EXEC_DELAY = 100;
        u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_START_ID & PARAM_MASK);
        sch_schedule(sc->sch, socket_event, SW_TO_EXEC_DELAY);
    }   

    cb_queue(sc->sw_queue, &moving_order);
}

void server_exec_to_sw(ServerContext* sc){
    if (cb_is_empty(sc->convert_holder))
        return;

    u32 synth_order_id = *(u32*)cb_deque(sc->convert_holder);

    if (cb_is_empty(sc->convert_holder) || synth_order_id == CONVERT_SENTINEL_VALUE) {
        return;
    }

    if (cb_is_empty(sc->sw_queue)) { 
        u64 SW_TO_EXEC_DELAY = 100;
        u64 socket_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (EXEC_START_ID & PARAM_MASK);
        sch_schedule(sc->sch, socket_event, SW_TO_EXEC_DELAY);
    }

    // this will pop the last CONVERT_SENTINEL_VALUE becuase we deque before we check
    while(!cb_is_empty(sc->convert_holder) && synth_order_id != CONVERT_SENTINEL_VALUE) {
        cb_queue(sc->sw_queue, &synth_order_id);
        synth_order_id = *(u32*)cb_deque(sc->convert_holder);
    }       

}

// may as well

void server_free(ServerContext* sc) {
    holder_free(sc->ho);
    sch_free(sc->sch);
    bs_free(sc->mbo_bs);
    bs_free(sc->mbp_bs);
    free(sc->client_settings);
    fl_free(sc->responses);
    fl_free(sc->icebergs);
    fl_free(sc->orders);
    cb_free(sc->fills);
    cb_free(sc->trades);
    cb_free(sc->candles_sec);
    cb_free(sc->candles_min);
    cb_free(sc->candles_hr);
    cb_free(sc->candles_day);
    cb_free(sc->imbalances);
    cb_free(sc->notified);
    cb_free(sc->log);
    free(sc->stream_roster);
    free(sc->tier_offset);
    cb_free(sc->live_roster);
    free(sc->live_offset);
    free(sc->tier_source);
    cb_free(sc->sw_queue);
    cb_free(sc->hw_queue);
    cb_free(sc->convert_holder);
    cb_free(sc->day_orders);
    cb_free(sc->expire_cb);
    pq_free(sc->gtd);
    pq_free(sc->price_pq);
    pq_free(sc->buy_stops);
    xpq_free(sc->sell_stops);
    pq_free(sc->wake_above);
    xpq_free(sc->wake_below);
    pq_free(sc->auction_bids);
    pq_free(sc->auction_asks);
    cb_free(sc->auction_market_bids);
    cb_free(sc->auction_market_asks);
    cb_free(sc->auction_arrivals);
    cb_free(sc->auction_bid_sorted);
    cb_free(sc->auction_ask_sorted);
    free(sc->leg_cost);
    free(sc->other_leg_cost);

    free(sc->rand);

    free(sc);
}

