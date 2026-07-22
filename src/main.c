#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "rand.h"
#include "sch.h"
#include "fl.h"
#include "bs.h"
#include "cb.h"
#include "types.h"
#include "constants.h"
#include "response.h"
#include "client.h"
#include "server.h"
#include "client_settings.h"
#include "holder.h"
#include "order.h"
#include "utils.h"

#include "ob.h"

void log_full(uint64_t raw) {
    printf("raw: %llu, priority %llu, type %llu, params %llu\n", 
            raw, 
            raw >> E_BITS, 
            (raw >> (E_BITS-T_BITS)) & T_MASK, 
            raw & PARAM_MASK);
}

int main(int argc, char* argv[]){
    TypeMetadata* tm = get_types();
    u32 * client_allocations = malloc(tm->IMPLS_COUNT * sizeof(u32*));

    // now we can do
    client_allocations[tm->cz_index] = 0;
    client_allocations[tm->co_index] = 0;
    // t1 flickerers: 20-50 registered mms in us equities, but only a handful move the
    // needle - citadel securities ~25% of volume, virtu ~20%, then jane street, jump,
    // hrt, imc, optiver, two sigma
    client_allocations[tm->t1_index] = 8;
    // t2 snipers: nasdaq's hft dataset identifies ~120 firms; heavily overlapping t1, since
    // the same desks often run both sides. 12 is that overlap plus a few pure takers
    client_allocations[tm->t2_index] = 12;
    // t3 slicers: ~100s of algo providers and ~1000s of systematic funds, but the unit that
    // matters is parent orders in flight, not firms - one desk runs many at once
    client_allocations[tm->t3_index] = 40;
    // t4 suits: ~32k hedge funds worldwide but ~550 firms hold 86% of the capital, so the
    // handful that move a name is low hundreds. the informed flow - trades toward fundamental
    client_allocations[tm->t4_index] = 60;
    // t5 degens: ~450k active US day traders; doc says 100s-1000s in sim. momentum retail -
    // the crash amplifier, and the protective-sell-stop population that fuels the cascade
    client_allocations[tm->t5_index] = 200;
    // t9 oracles: true value pickers are professionally rare, doc says 10s-100s. deep
    // contrarian capital anchored to fundamental - THE arresting loop, the crash floor
    client_allocations[tm->t9_index] = 30;

    ServerContext* sc = server_init(tm, client_allocations, 603);

    // genuinely needed everywhere
    SCH* sch = sc->sch;

    uint64_t repeat_event = build_event(CONTROL_TYPE, CONTROL_PARAM_SLOW);
    sch_schedule(sch, repeat_event, 0);

    u64 kill_event = build_event(CONTROL_TYPE, CONTROL_PARAM_KILL);
    sch_schedule(sch, kill_event, 40 * DAY_TO_NS);

    u64 news_event = build_event(CONTROL_TYPE, CONTROL_PARAM_NEWS);
    sch_schedule(sch, news_event, 7 * DAY_TO_NS);

    // first daily short-borrow charge at the next midnight, then it reschedules every calendar day
    u64 eod_event = build_event(CONTROL_TYPE, CONTROL_PARAM_EOD);
    sch_schedule(sch, eod_event, DAY_TO_NS);

    // first monthly data-fee bill, at the end of the first calendar month. it reschedules itself
    u64 eom_event = build_event(CONTROL_TYPE, CONTROL_PARAM_EOM);
    sch_schedule(sch, eom_event, delay_to_next_month(0));

    // first opening accumulation, a window before the first bell. the bells chain from there
    u64 open_event = build_event(CONTROL_TYPE, CONTROL_PARAM_AUCTION_OPEN);
    sch_schedule(sch, open_event, FIRST_OPEN_NS - AUCTION_OPEN_WINDOW_NS);

    Holder* ho = sc->ho;
    ClientSettings* client_settings = sc->client_settings;
    for(u32 i = 0; i < ho->num_clients; i++) {
        uint64_t client_id = i; 

        uint64_t boot_event = build_event(CONTROL_TYPE, client_id);
        sch_schedule(sch, boot_event, client_settings[i].initial_wake);
    }

    FL* orders = sc->orders;
    FL* responses = sc->responses;
    BS* mbo_bs = sc->mbo_bs;

    // calloc, else news_signal is garbage until the first news event fires
    Context* context = calloc(1, sizeof(Context));

    while(1){
        rand_next(sc->rand);
        context->random = (*(sc->rand)) & MAX_U32;

        uint64_t next = sch_pop(sch);

        context->real_time_ns = sch_now_ns(sch);
        // per-event trace. one line per scheduler pop, so it dwarfs everything else once the
        // book is actually busy - 25GB by sim day 11 with the T3 population in
        //printf("NOW %llu ~%llus - ", context->real_time_ns, context->real_time_ns/1000000000);
        //log_full(next);

        uint8_t type = (next >> PARAM_BITS) & T_MASK;

        uint64_t params = next & PARAM_MASK;

        // different from waht is below
        if (type == SERVER_TYPE) {
            // something in the server
            //printf("server type\n");

            u64 order_id = params;


            // order arrives to server
            if (order_id < MIN_RESERVED_PACKET) {
                server_arrival(sc, order_id);
            } else if (order_id == HW_TO_SW_ID) {
                server_hw_to_sw(sc);
            } else if (order_id == EXEC_START_ID) {
                server_exec_start(sc);
            } else if (order_id == EXEC_END_ID) {
                //printf("NOW %llu ~%llus \n", now_ns, now_ns/1000000000);
                server_exec_end(sc);
            } else if (order_id == EXEC_TO_SW_ID) {
                // kinda broken for now
                server_exec_to_sw(sc);
            }
        } else if (type == CLIENT_IN_TYPE) {
            //printf("client in type\n");

            // now how do we handle a ping event

            u32 response_id = params;

            Response response = *(Response*)fl_release(responses, response_id);

            u32 client_id = response.client_id;
            u32 snapshot_id = response.snapshot_id;
            u32 status = response.status;

            Order tmp = {};
            u32 new_order_id = fl_insert(orders, &tmp);
            Order* empty = fl_get(orders, new_order_id);

            context->next_order_id = new_order_id;
            context->next_order_ptr = empty;

            context->status = status;
            context->quantity_filled = response.quantity_filled;
            context->price = response.price;
            context->mark = sc->mark; // last trade price, visible to every tier
            context->rej_reason = response.rej_reason;
            // atomic pair: the ask leg rides along in the same response (pair bit set in status)
            context->second_order_id = response.second_order_id;
            context->second_price = response.second_price;
            context->second_quantity_filled = response.second_quantity_filled;

            if ((status >> PING_BIT) & 1) {
                // the client can schedule these from themselves kinda
                // so this will not come with any knowledge of snapshot
                // and this is not in response to an order id 
                // this just wakes them up
                context->data_snapshot = 0;
                // schedule_response pinned the current mbo (snapshot_id) for every response it
                // sends; a ping ignores the book but must still hand that ref back, or the
                // snapshot never reaches zero refs and the blob-store ring fills to its cap.
                // a self-wake ping (snapshot_id == MAX_U32) never took a ref, so leave it alone
                if (snapshot_id != MAX_U32)
                    bs_get(mbo_bs, snapshot_id);
                // the wake we were holding has landed, so clear before the handler runs and it can
                // arm the next one. a re-arm leaves the one it beat still in the scheduler, and that
                // stale ping lands later with nothing pending - the compare keeps it from clearing
                // a newer wake. a control ack carrying PING_BIT is an order reply, not one of ours
                if (!((status >> CONTROL_BIT) & 1)
                        && context->real_time_ns >= client_settings[client_id].next_wake_ns)
                    client_settings[client_id].next_wake_ns = MAX_U64;
            } else if ((status >> BROADCAST_BIT) & 1) {
                // market-data push: response.tier picks the source, snapshot_id is the id/offset.
                // blob tiers resolve with bs_get (move-safe via metadata), buffers with cb_at
                if (response.tier <= TIER_MBP1)
                    context->data_snapshot = bs_get((BS*)sc->tier_source[response.tier], snapshot_id);
                else
                    context->data_snapshot = cb_at((CB*)sc->tier_source[response.tier], snapshot_id);
                context->order_id = response.order_id;
            } else if (client_settings[client_id].sub_tier == TIER_FREE) {
                // free tier gets no book, just the last trade price already on the context -
                // but still release the mbo ref schedule_response took, same as every other path
                bs_get(mbo_bs, snapshot_id);
                context->data_snapshot = 0;
                context->order_id = response.order_id;
            } else {
                // for some clients, they need to get the MBP. But that's later
                context->data_snapshot = bs_get(mbo_bs, snapshot_id);
                context->order_id = response.order_id;
                // they cant use it anyways
                //if (response.order_id != MAX_U32)
                //context->response_order_ptr = (Order*)fl_get(orders, response.order_id);
            }

            //printf("sending to client %u order %u\n", client_id, response.order_id);
            // order 7, which filled, seems to be ignored...
            // wait a minute, will this go out of scope. hopefully not
            // set after the clear above, so it's what the handler's own re-arm will be tested against
            context->next_wake_ns = client_settings[client_id].next_wake_ns;
            u8 action = holder_client_on_snapshot(ho, client_id, context);

            // they've read the response order id, so hand its slot back for any terminal
            // outcome that left nothing resting in the book under that id:
            //  - rejected, or a ping/ws control ack: the message never reached the book
            //  - fully filled as a taker: consumed on arrival, no residual rests
            //  - a pure cancel/pull: the target it names is freed server-side; the cancel
            //    message itself rests nothing, so its own slot has to come back here too.
            // a canrep is the exception - it carries CAN_REP_BIT and its new leg rests under
            // this same id, so it is freed later when that leg is cancelled/filled, not now.
            u8 rested_nothing_cancel = ((status >> CANCEL_BIT) & 1) && !((status >> CAN_REP_BIT) & 1);
            u8 terminal = ((status >> REJECT_BIT) & 1)
                | ((status >> CONTROL_BIT) & 1)
                | (((status >> FILL_BIT) & 1) && !((status >> PARTIAL_FILL_BIT) & 1))
                | rested_nothing_cancel;
            if (terminal)
                fl_release(orders, response.order_id);

            // a pulled two-sided quote retires two message slots at once: the ask leg was
            // materialized as its own order and, being a cancel too, also rests nothing - it
            // rode back under second_order_id. (a resting quote carries no cancel bit, so its
            // legs are left in place to be freed when they are each later cancelled.)
            // exception: a REJECTED pair already released its ask leg server-side (server.c:1464)
            // and reports second_order_id as MAX_U32, so only reap it when the pair was accepted.
            if (rested_nothing_cancel && ((status >> ASK_BID_PAIR_BIT) & 1) && !((status >> REJECT_BIT) & 1))
                fl_release(orders, response.second_order_id);

            if (action & 1){
                empty->client_id = client_id;
                //empty->flags = 0;
                u64 order_event = ((CLIENT_OUT_TYPE & T_MASK) << PARAM_BITS) | (new_order_id & PARAM_MASK);
                u64 delay = client_settings[client_id].processing_time;

                sch_schedule(sch, order_event, delay);
            } else {
                //printf("reeasing order id\n");
                fl_release(orders, new_order_id);
            }

            // second bit set, for a wakeup call
            // only lands if it beats the wake already pending: arming one per event is what breeds
            // the runaway, but a client that wants up sooner than it asked still gets to. dropped
            // quietly - it was never an order, so there's nothing to reject or hand back
            if (action & 2) {
                u64 delay = context->wake_delay_ns; // client chosen
                u64 fire_at = context->real_time_ns + delay;
                if (fire_at < client_settings[client_id].next_wake_ns) {
                    client_settings[client_id].next_wake_ns = fire_at;
                    sch_schedule(sch, build_event(CONTROL_TYPE, client_id), delay);
                }
            }


        } else if (type == CLIENT_OUT_TYPE) {
            //printf("client out type\n");

            u32 order_id = params;
            Order order = *(Order*)fl_get(orders, order_id);

            u64 out_event = ((SERVER_TYPE & T_MASK) << PARAM_BITS) | (order_id & PARAM_MASK);
            sch_schedule(sch, out_event, calculate_jitter(client_settings + (order.client_id), sc->rand));
        } else if (type == CONTROL_TYPE) {
            //printf("control type\n");
            // it will go here
            // and this will look a bit like the server event type

            // we should probalby actually you know wake up the client
            u32 control_id = params;
            if (control_id < MIN_CONTROL_PARAM) {
                u32 client_id = params;

                // idk I feel like this makes more sense as a response type

                // this self-wake ping is minted here, NOT through schedule_response, so it never
                // pinned an mbo snapshot. mark snapshot_id as "none" (MAX_U32) so the read side
                // knows not to release an mbo ref it never took - dropping one would free the live
                // current book out from under the next order's derivation
                Response r = {.client_id = client_id, .status = 1 << PING_BIT, .snapshot_id = MAX_U32};
                u32 response_id = fl_insert(responses, &r);
                u64 response_event = build_event(CLIENT_IN_TYPE, response_id);
                // really not sure about the delay here tbh
                sch_schedule(sch, response_event, calculate_jitter(client_settings + (client_id), sc->rand));


                // bump client, not necessarily for first tim
                // for now we'll just have them connect with a socket order
                // like we were doin earlier
                // obviously not all clieints will want to do WS connection

                //printf("boot event\n");
                // for now, le't sjust say we wena tto connect to the websocket

                //u64 delay = 300000000;//cz_postboot_socket(0);
                //Order p = { .flags = 1 << PING_BIT, .client_id = client_id };
                //u32 order_id = fl_insert(orders,&p);
                //u64 socket_event = ((CLIENT_OUT_TYPE & T_MASK) << PARAM_BITS) | (order_id & PARAM_MASK);

                //sch_schedule(sch, socket_event, delay);

            } else if (control_id == CONTROL_PARAM_SLOW) {
                // ignore, mostly handled by sch.c
            } else if (control_id == CONTROL_PARAM_EOD) {
                // charge one day of short-borrow interest, then reschedule for the next calendar day
                server_eod(sc);
                sch_schedule(sch, build_event(CONTROL_TYPE, CONTROL_PARAM_EOD), DAY_TO_NS);
            } else if (control_id == CONTROL_PARAM_EOM) {
                // charge clients their monthly data subscription, then reschedule at the next month end
                server_eom(sc);
                sch_schedule(sch, build_event(CONTROL_TYPE, CONTROL_PARAM_EOM), delay_to_next_month(sch_now_ns(sch)));
            } else if (control_id == CONTROL_PARAM_OPEN) {
                server_market_open(sc);
                // continuous trading. context is shared, so set the phase once here
                context->is_open = 1;
                context->auctioning = 0;
                context->auction_frozen = 0;
            } else if (control_id == CONTROL_PARAM_CLOSE) {
                server_market_close(sc);
                context->is_open = 0;
                context->auctioning = 0;
                context->auction_frozen = 0;
            } else if (control_id == CONTROL_PARAM_AUCTION_OPEN || control_id == CONTROL_PARAM_AUCTION_CLOSE) {
                // accumulation window opens - is_open keeps its value (closed pre-open, open pre-close)
                server_auction_accumulate(sc);
                context->auctioning = 1;
                context->auction_frozen = 0;
            } else if (control_id == CONTROL_PARAM_AUCTION_FREEZE) {
                server_auction_freeze(sc);
                context->auction_frozen = 1;
            } else if (control_id == CONTROL_PARAM_CANDLE) {
                server_candle_close(sc);
            } else if (control_id == CONTROL_PARAM_NEWS) {
                // randomly set context to some value, and do not change it until next

                context->news_signal = context->random & MAX_U8;
                context->last_news_ns = context->real_time_ns;

                // 1 to 32 days out, because 32 is easy
                // this is debatable, and could be any values or distribution
                u64 news_delay = (1+(context->random & 31))* DAY_TO_NS;

                u64 news_event = build_event(CONTROL_TYPE, CONTROL_PARAM_NEWS);
                sch_schedule(sch, news_event, news_delay);

            } else if (control_id == CONTROL_PARAM_KILL) {
                printf("kill event triggered\n");
                // gg 
                break;
            }
            //printf("control type done\n");
        } 

    }

    for(u32 i = 0; i < ho->num_clients; i++) {
        u32 agro = i;
        ClientSettings* cs = sc->client_settings + agro;
        printf("from client id #%u [$%lld/$%u/%lldsh/%ush]\n", agro, cs->cash, cs->reserved_cash, cs->shares, cs->reserved_shares);
    }

    mbo_dump(bs_get_no_ref(sc->mbo_bs, sc->last_mbo));

    server_free(sc);

    return 0;
}
