#include "trade.h"
#include "types.h"
#include "constants.h"
#include "server.h"
#include "cb.h"

void update_candle(Candle* last, u16 price, u32 quantity) {
    if (price > last->hi)
        last->hi = price;
    else if (price < last->lo)
        last->lo = price;
    // technically we dont need to set it here but its easier for now
    last->close = price;
    last->volume += quantity;
}

// update the current candle in place if the trade is still in its bucket, else roll a new one
void update_or_add_candle(CB* candles, Candle* c, u64 ns, u64 duration) {
    Candle* cur = (Candle*)cb_last(candles);
    u64 bucket = (ns / duration) * duration;

    if (cur && cur->time == bucket) {
        update_candle(cur, c->close, c->volume);
        return;
    }

    c->duration = duration;
    c->time = bucket;
    cb_queue(candles, c); // cb_queue copies, so one c can be reused across timeframes
}

void update_sec_candle(ServerContext* sc, Trade* t) {
    Candle c = {
        .open = t->price,
        .hi = t->price,
        .lo = t->price,
        .close = t->price,
        .volume = t->quantity
    };
    update_or_add_candle(sc->candles_sec, &c, t->ns, S_TO_NS);
    update_or_add_candle(sc->candles_min, &c, t->ns, MIN_TO_NS);
    update_or_add_candle(sc->candles_hr,  &c, t->ns, H_TO_NS);
    update_or_add_candle(sc->candles_day, &c, t->ns, DAY_TO_NS);
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
    update_sec_candle(sc, &t);
    cb_queue(sc->trades, &t);

}


