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

// yeah you need these two to initialize the server, cuz really it initalizes everything
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
    sc->executing = 0;
    sc->is_open = 0; // closed until the first open event rings the bell
    sc->day_orders = cb_init(sizeof(u32)); // drained every close, so ids alone suffice
    sc->gtd = pq_init(); // (date << 32 | id), lives across closes
    sc->price_pq = pq_init();
    sc->expire_cb = cb_init(sizeof(u64));
    sc->sw_queue = cb_init(sizeof(u32));
    sc->hw_queue = cb_init(sizeof(u32));
    sc->convert_holder = cb_init(sizeof(u32));

    sc->orders = fl_init(sizeof(Order), MIN_RESERVED_PACKET);
    sc->fills = cb_init(sizeof(Fill));
    sc->responses = fl_init(sizeof(Response), MAX_U32);
    sc->icebergs = fl_init(sizeof(Iceberg), MAX_U32);

    sc->rand = rand_init(seed);
    rand_next(sc->rand);
    sc->sch = sch_init(sc->rand);

    return sc;
}

void schedule_response(ServerContext* sc, u32 client_id, u32 status, u32 quantity_filled, u32 order_id, u16 price, u8 rej_reason) {
    // this assumes that all client are mbo subscribers
    bs_bump_refs(sc->mbo_bs, sc->last_mbo);
    Response r = {.snapshot_id = sc->last_mbo, .client_id = client_id, .status = status, .order_id = order_id, .quantity_filled = quantity_filled, .price = price, .rej_reason = rej_reason};
    u32 response_id = fl_insert(sc->responses, &r);
    u64 response_event = build_event(CLIENT_IN_TYPE, response_id);
    sch_schedule(sc->sch, response_event, calculate_jitter(sc->client_settings + (client_id), sc->rand));
    // so don't blast over websocket
    sc->client_settings[client_id].will_notify = 1;
}

// same as schedule_response, but carries both legs of an atomic pair in one delivery
void schedule_pair_response(ServerContext* sc, u32 client_id, u32 status, u32 order_id, u16 price, u32 quantity_filled, u32 second_order_id, u16 second_price, u32 second_quantity_filled, u8 rej_reason) {
    bs_bump_refs(sc->mbo_bs, sc->last_mbo);
    Response r = {.snapshot_id = sc->last_mbo, .client_id = client_id, .status = status, .order_id = order_id, .price = price, .quantity_filled = quantity_filled, .second_order_id = second_order_id, .second_price = second_price, .second_quantity_filled = second_quantity_filled, .rej_reason = rej_reason};
    u32 response_id = fl_insert(sc->responses, &r);
    u64 response_event = build_event(CLIENT_IN_TYPE, response_id);
    sch_schedule(sc->sch, response_event, calculate_jitter(sc->client_settings + (client_id), sc->rand));
    sc->client_settings[client_id].will_notify = 1;
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

    if (!is_active || is_rejected)
        return REJ_ORDER_ALREADY_DONE;

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

// the usual stuff
// 0 if the order can be worked, otherwise why it can't
// fillable, if non-null, comes back with how much crosses on arrival - the iceberg create
// path uses it to size a marketable slice. an iceberg prices the whole hidden total here,
// not just the visible chunk, so the walk covers everything it might cross
u8 add_precheck(Order* in, ClientSettings* cs, MBO* mbo, u16 mark, u32 reclaimed_cash, u32 reclaimed_shares, u32* fillable){

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
    u32 q_remain = order_q;

    // abstract sense of how much it costs to fill the order, either cash or shares
    u32 cost = 0;

    // on margin, the shares that just close what we already have are exempt from the BP
    // gate - a close reduces risk, and a broker always lets you out. everything past the
    // close is opening exposure and gets charged at what it actually fills at. attributed
    // chunk by chunk down the walk, so no total ever has to be divided back out
    u32 close_q = 0;
    if (!cs->is_cash_account) {
        if (direction == 1 && cs->shares < 0)
            close_q = (u32)(-cs->shares); // buying back a short
        else if (direction == 0 && cs->shares > (i64)cs->reserved_shares)
            close_q = (u32)(cs->shares - cs->reserved_shares); // selling longs we aren't already quoting
    }
    u32 open_notional = 0;

    // walk the same levels ob will, so what we charge is what actually fills
    u16 lo_ask_index = mbo->hi_bid_index == MAX_U16 ? 0 : mbo->hi_bid_index + 1;

    if (direction == 1){
        for (u16 run = lo_ask_index; run < mbo->level_count; run++) {
            MBOIndex* mboi = mbo->levels + run;
            if (!is_market && (q_remain > 0 && price < mboi->price))
                break;

            if (q_remain < mboi->quantity) {
                cost += q_remain * mboi->price;

                u32 closing = q_remain < close_q ? q_remain : close_q;
                close_q -= closing;
                open_notional += (q_remain - closing) * mboi->price;

                q_remain -= q_remain;
            } else if (q_remain >= mboi->quantity) {
                cost += mboi->quantity * mboi->price;

                u32 closing = mboi->quantity < close_q ? mboi->quantity : close_q;
                close_q -= closing;
                open_notional += (mboi->quantity - closing) * mboi->price;

                q_remain -= mboi->quantity;
            }
        }
    } else if (mbo->hi_bid_index != MAX_U16){
        // 0-- wraps to MAX_U16, the same "ran off the bottom" sentinel as hi_bid_index
        for (u16 run = mbo->hi_bid_index; run != MAX_U16; run--) {
            MBOIndex* mboi = mbo->levels + run;

            if (!is_market && (q_remain > 0 && price > mboi->price))
                break;

            if (q_remain < mboi->quantity) {
                cost += q_remain;

                u32 closing = q_remain < close_q ? q_remain : close_q;
                close_q -= closing;
                open_notional += (q_remain - closing) * mboi->price;

                q_remain -= q_remain;
            } else if (q_remain >= mboi->quantity) {
                cost += mboi->quantity;

                u32 closing = mboi->quantity < close_q ? mboi->quantity : close_q;
                close_q -= closing;
                open_notional += (mboi->quantity - closing) * mboi->price;

                q_remain -= mboi->quantity;
            }
        }
    }


    // whatever the walk consumed crossed on arrival; the rest would rest
    if (fillable)
        *fillable = order_q - q_remain;

    // 2. if liquidity isn't enough branch on GTC/IOC/FOK

    //is_gtc is_ioc is_fok

    if (q_remain) {
        if (is_gtc) {
            // we rest the remaining, continue to order book
        } else if (is_ioc) {
            // we discard the remaining, continue to order book
        } else if (is_fok) {
            // not enough liquidity for full order, drop it
            // yes put in separate method
            return CXL_FOK_KILLED;
        }
    }

    // 3. if cost is too much, reject

    // if we weant to rest the remainder, then we actually need to add that remainder to the cost

    if (is_gtc){
        // add the resting cost
        if (direction == 1)
            cost += q_remain * in->price;
        else
            cost += q_remain;

        u32 closing = q_remain < close_q ? q_remain : close_q;
        close_q -= closing;
        open_notional += (q_remain - closing) * in->price;
    }
    // for ioc, we just ignore the remaining quantity

    if (!cs->is_cash_account) {
        // reg t IS the gate, there is no second equity check. equity's job is maintenance
        if (client_bp(cs, mark, reclaimed_cash, reclaimed_shares) < open_notional)
            return direction == 1 ? REJ_NO_BUYING_POWER : REJ_NO_SHARES;
    } else if (direction == 1) {
        // we have enough cash to buy
        if (cs->cash - cs->reserved_cash + reclaimed_cash < cost)
            return REJ_NO_BUYING_POWER;
    } else {
        // we have enough shares to sell
        if (cs->shares - cs->reserved_shares + reclaimed_shares < cost)
            return REJ_NO_SHARES;
    }
    return REASON_NONE;

}

u8 canrep_precheck(Order* in, ClientSettings* cs, FL* orders, MBO* mbo, u16 mark) {
    // here, we will enforce a valid cancellation
    u8 cancel_reason = cancel_precheck(in, orders, mbo);
    if (cancel_reason)
        return cancel_reason;

    u32 reclaimed_cash = 0;
    u32 reclaimed_shares = 0;
    Order* cancel = (Order*)fl_get(orders, in->other_id);
    if ((cancel->status >> BUY_DIRECTION_BIT) & 1)
        reclaimed_cash += cancel->quantity * cancel->price;
    else
        reclaimed_shares += cancel->quantity;

    return add_precheck(in, cs, mbo, mark, reclaimed_cash, reclaimed_shares, 0);
}

// 0 if both legs can be worked, otherwise why they can't. the bid is in the primary
// fields and the ask in second_*, so this checks the legs against each other too
u8 pair_precheck(Order* in, Order* ask_in, ClientSettings* cs, FL* orders, MBO* mbo) {
    u8 is_can_rep = (in->status >> CAN_REP_BIT) & 1;
    u8 is_cancel = (in->status >> CANCEL_BIT) & 1;
    u8 ask_is_cr = (ask_in->status >> CAN_REP_BIT) & 1;
    u8 is_market = (in->status >> IS_MARKET_BIT) & 1;
    u8 is_gtc = !(((in->status >> IOC_BIT) & 1) | ((in->status >> FOK_BIT) & 1));

    // both legs of a pair are resting quotes, so a market or ioc/fok pair is nonsense
    if (is_market || !is_gtc)
        return REJ_BAD_QUALIFIER;

    // a replacing or cancelling leg needs a valid cancel. a stale / nonexistent / not-ours
    // cancel id fails cancel_precheck, and we hand back that leg's reason
    u8 bid_cancel_reason = (is_can_rep | is_cancel) ? cancel_precheck(in, orders, mbo) : REASON_NONE;
    if (bid_cancel_reason)
        return bid_cancel_reason;

    u8 ask_cancel_reason = (ask_is_cr | is_cancel) ? cancel_precheck(ask_in, orders, mbo) : REASON_NONE;
    if (ask_cancel_reason)
        return ask_cancel_reason;

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
    if (ask_is_cr) {
        Order* oc = (Order*)fl_get(orders, ask_in->other_id);
        if ((oc->status >> BUY_DIRECTION_BIT) & 1) freed_cash += oc->quantity * oc->price;
        else freed_shares += oc->quantity;
    }

    // bid draws cash, ask draws shares — separate pools, checked independently
    u32 bp = (cs->cash - cs->reserved_cash) + freed_cash;
    u32 sh = (cs->shares - cs->reserved_shares) + freed_shares;

    if ((u64)in->quantity * in->price > bp)
        return REJ_NO_BUYING_POWER;

    if (in->second_quantity > sh)
        return REJ_NO_SHARES;

    return REASON_NONE;
}


// much better
// the big driver of all market book stuff
// this mostly takes care of scheduling, then passes it off to server_order
void server_order(ServerContext* sc, u32 exec_order_id) {

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
    if (is_toggle_ws) {
        cs->ws = !(cs->ws);
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
    u8 ask_is_cr = is_pair ? ((ask_in->status >> CAN_REP_BIT) & 1) : 0;
    // create leaves second_id at MAX_U32; a minted slice carries its iceberg id
    u8 is_iceberg = (in->status >> ICEBERG_BIT) & 1;
    u8 is_iceberg_replenish = is_iceberg && in->second_id != MAX_U32;
    // gtc is the default: no tif bit set rests the remainder, like a plain limit always did
    u8 is_gtc = !(((in->status >> IOC_BIT) & 1) | ((in->status >> FOK_BIT) & 1));

    // a ping / ws toggle carries no quantity on purpose - it was never an order, so it skips
    // the prechecks and the book entirely. the ws side effect above already happened, and
    // nothing rests, so CONTROL_BIT tells the client to hand the slot straight back.
    // note is_toggle_ws is the masked bit, not 0/1, so these have to be logical ands
    if ((is_ping || is_toggle_ws) && in->quantity == 0 && !is_pair && !is_cancel && !is_can_rep) {
        status |= (1 << CONTROL_BIT);
        schedule_response(sc, in->client_id, status, 0, exec_order_id, 0, REASON_NONE);
        return;
    }

    void* old_mbo_raw = bs_get_no_ref(mbo_bs, sc->last_mbo);

    u32 fillable = 0; // how much an add crosses on arrival; the iceberg create sizes off it

    if (!sc->is_open) {
        rej_reason = REJ_MARKET_CLOSED;
    } else if (is_pair) {
        rej_reason = pair_precheck(in, ask_in, cs, orders, (MBO*)old_mbo_raw);
    } else if (is_can_rep) {
        rej_reason = canrep_precheck(in, cs, orders, (MBO*)old_mbo_raw, sc->mark);
        status |= (1 << CAN_REP_BIT);
    } else if (is_cancel) {
        rej_reason = cancel_precheck(in, orders, (MBO*)old_mbo_raw);
        status |= (1 << CANCEL_BIT);
    } else if (is_iceberg_replenish) {
        // a minted slice is already funded and reserved from create time; it just rests
    } else {
        rej_reason = add_precheck(in, cs, (MBO*)old_mbo_raw, sc->mark, 0, 0, &fillable);
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

    printf("[%llus] order #%u ", now_ns/S_TO_NS, exec_order_id);
    printf("client #%u [$%lld/$%u/%lldq/%uq] ", in->client_id, cs->cash, cs->reserved_cash, cs->shares, cs->reserved_shares);
    if (is_cancel) {
        printf("cancel order #%u ", in->other_id);
    } else {
        printf("%s %s %ush @ $%u ", ((in->status >> IS_MARKET_BIT) & 1) ? "market" : "limit", is_buy ? "buy" : "sell", in->quantity, in->price);
        if (is_can_rep) {
            printf("+ cancel order %u ", in->other_id);
        }
    }
    printf("\n");


    // ok at this point I think can now handle stop orders

    // the idea is do some quick validation on the stop order
    // let me sleep on this
    

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
        printf("TRADE buy %u p %u q %u id %u now %llu part %u\n", taker_is_buy, order->price, q, fill->order_id, now_ns, fill->partial);

        // the cancelled trade cannot show up here
        // in fact it shoudln't even be here
        // we should genuinely update teh order->quantity because cancels rely on it
        //printf("in order q before %u for id %u\n", in->quantity, exec_order_id);
        //printf("resting q before %u for id %u\n", order->quantity, fill->order_id);
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
    u32 filled = before_quantity - in->quantity;
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
        // should probably release too?
        // or should the client receive notification of this, like a fill?
        // no - this call succeeding implies it went through
    }

    // the block above retired the bid's replaced order; do the same for the ask leg.
    // ask_is_cr is the replace case, is_cancel the pull-both-quotes case
    if (is_pair && (ask_is_cr | is_cancel)) {
        Order* old_ask = (Order*)fl_get(orders, ask_in->other_id);

        // normally a sell, but dont assume - cancel_precheck only proved it was ours and resting
        if ((old_ask->status >> BUY_DIRECTION_BIT) & 1)
            cs->reserved_cash -= old_ask->quantity * old_ask->price;
        else
            cs->reserved_shares -= old_ask->quantity;

        old_ask->quantity = 0;
    }

    // ok unfortunately we do need to do a a few assertions to make sure of stuff before we refactor
    // this explicitly relies on the 2 client $10000000 1000sh initial setup


    // might as well create a new MBP here. all we need is the new mbo

    void* new_mbp_raw;
    u32 next_last_mbp = bs_reserve(sc->mbp_bs, mbp_derive_size(new_mbo_raw), 1, &new_mbp_raw);
    mbp_derive(new_mbp_raw, new_mbo_raw);

    bs_get(sc->mbp_bs, sc->last_mbp);
    sc->last_mbp = next_last_mbp;

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

        //if last_trade_price >= pq_peek(asks)
            // pop asks as market ordersinto the convert holder
        //if last_trade_price <= pq_peek(bids)
            // pop bids as market orders into the convert holder
        // yeah we need a new stop order type

        // price is priority, then the rest of the 32 bits can be like stop id.
        // lets use an FL for stops


        // their validation will be handled later
    }


    // i know its ugly

    // send special one to self
    //printf("scheduling response %u\n", exec_order_id);
    if (is_pair)
        // both legs, one delivery, each with its own filled quantity
        schedule_pair_response(sc, in->client_id, status, exec_order_id, in->price, filled, ask_order_id, ask_in->price, (before_ask_quantity - ask_in->quantity), rej_reason);
    else
        schedule_response(sc, in->client_id, status, filled, exec_order_id, in->price, rej_reason);

    // final broadcast send
    for (u32 ci = 0; ci < ho->num_clients; ci++){
        if (client_settings[ci].will_notify == 0 && client_settings[ci].ws) {
            schedule_response(sc, ci, 0, 0, MAX_U32, 0, REASON_NONE);
        }
        // reset
        client_settings[ci].will_notify = 0;
    }
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

    void* new_mbp_raw;
    u32 next_last_mbp = bs_reserve(sc->mbp_bs, mbp_derive_size(new_mbo_raw), 1, &new_mbp_raw);
    mbp_derive(new_mbp_raw, new_mbo_raw);

    bs_get(sc->mbp_bs, sc->last_mbp);
    sc->last_mbp = next_last_mbp;

    bs_get(mbo_bs, prev_last_mbo);
}

void server_market_open(ServerContext* sc) {
    sc->is_open = 1;
    printf("[%llus] MARKET OPEN\n", sch_now_ns(sc->sch)/S_TO_NS);

    sch_schedule(sc->sch, build_event(CONTROL_TYPE, CONTROL_PARAM_CLOSE), OPEN_TO_CLOSE_NS);
}

void server_market_close(ServerContext* sc) {
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

    // fridays jump the weekend too
    u64 gap = CLOSE_TO_OPEN_NS;
    if (today % 7 == FRIDAY_MOD)
        gap += WEEKEND_NS;
    sch_schedule(sc->sch, build_event(CONTROL_TYPE, CONTROL_PARAM_OPEN), gap);
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
    cb_free(sc->sw_queue);
    cb_free(sc->hw_queue);
    cb_free(sc->convert_holder);
    cb_free(sc->day_orders);
    cb_free(sc->expire_cb);
    pq_free(sc->gtd);
    pq_free(sc->price_pq);

    free(sc->rand);

    free(sc);
}

