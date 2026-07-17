#include "trade.h"
#include "types.h"
#include "constants.h"
#include "server.h"
#include "cb.h"

void update_candle(Candle* last, u16 price) {
    if (price > last->hi)
        last->hi = price;
    else if (price < last->lo) 
        last->lo = price;
    // technically we dont need to set it here but its easier for now
    last->close = price;
}

void update_sec_candle(ServerContext* sc, Trade* t) {
    u16 price = t->price;
    u64 last_ns = 0;
    if (cb_is_empty(sc->trades)) {
        // maybe this should be done in some initialization so as to not check every trade?
        Candle c = {
            .duration = S_TO_NS,
            .time = (t->ns/S_TO_NS) * S_TO_NS,
            .open = t->price,
            .hi = t->price,
            .lo = t->price,
            .close = t->price
        };
        cb_queue(sc->candles_sec, &c);

        c.duration *= 60;
        // this will trunc to a minute
        c.time = (t->ns / c.duration) * c.duration;
        cb_queue(sc->candles_min, &c);

        c.duration *= 60;
        c.time = (t->ns / c.duration) * c.duration;
        cb_queue(sc->candles_hr, &c);

        c.duration *= 24;
        c.time = (t->ns / c.duration) * c.duration;
        cb_queue(sc->candles_day, &c);

        // harmless in terms of candles to set it to this
        last_ns = t->ns;
    } else {
        last_ns = (Trade*)(cb_at(sc->trades, cb_count(sc->trades)))->ns;
    }


    u64 now_s = t->ns / S_TO_NS;
    u64 last_s = last_ns / S_TO_NS;

    if (now_s > last_s) {
        // this trade does not belong to the current candle
        // start a new candle
        Candle sec = {
            .duration = S_TO_NS,
            .time = now_s*S_TO_NS, //trunced
            .open = t->price,
            .hi = t->price,
            .lo = t->price,
            .close = t->price
        }
        cb_queue(sc->candles_sec, &sec);

        // quick min check, may as well
        u64 now_min = now_s / 60;
        u64 last_min = last_s / 60;

        if (now_min > last_min) {
            Candle min = {
                .duration = MIN_TO_NS,
                .time = now_min*MIN_TO_NS, //trunced
                .open = t->price,
                .hi = t->price,
                .lo = t->price,
                .close = t->price
            }
            cb_queue(sc->candles_min, &min);

            u64 now_hr = now_min / 60;
            u64 last_hr = last_min / 60;

            if (now_hr > last_hr) {
                Candle hr = {
                    .duration = H_TO_NS,
                    .time = now_hr*H_TO_NS, //trunced
                    .open = t->price,
                    .hi = t->price,
                    .lo = t->price,
                    .close = t->price
                }
                cb_queue(sc->candles_hr, &hr);

                u64 now_d = now_hr / 24;
                u64 last_d = last_hr / 24;

                if (now_d > last_d) {
                    Candle d = {
                        .duration = DAY_TO_NS,
                        .time = now_d*DAY_TO_NS, //trunced
                        .open = t->price,
                        .hi = t->price,
                        .lo = t->price,
                        .close = t->price
                    }
                    cb_queue(sc->candles_day, &d);
                }
            }

        }


    } else {
        // in a previous project, SIMD really helped here
        Candle* last = (Candle*)cb_last(sc->candles_sec);
        update_candle(last, price);
        last = (Candle*)cb_last(sc->candles_min);
        update_candle(last, price);
        last = (Candle*)cb_last(sc->candles_hr);
        update_candle(last, price);
        last = (Candle*)cb_last(sc->candles_day);
        update_candle(last, price);
    }

    // unfortunately we now need to update all the current candles as we did not move candles
}

void update_trade(ServerContext* sc, u32 quantity, u16 price, u8 direction) {
    // we do need the last TS
    // cb_peek?

    u64 ns = sch_now_ns(sc->sch);

    Trade t = {
        .ns = ns,
        .quantity = quantity,
        .price = price,
        .direction = direction 
    };
    update_second_candle(sc, &t);
    cb_queue(sc->trades, &t);

}


