# Tiltyard — a full market simulator

In C so very fast vroom.

One stock, `$TYD`. A hand-built matching engine, and a synthetic population of ~660
traders across 14 behavioral tiers all elbowing each other in the same order book.
You press go, ~40 sim-days go by in ~80 wall-clock seconds, and out the other end you
get a trade log you can turn into candles. Every order in the book was made up by something in this repo. It's a closed world you can rerun deterministically and poke at.

## what's actually in the box

- **a continuous order book** (`src/ob.c`) — price-time priority, matched one order at a
  time. limit + market orders, IOC/FOK/GTC/DAY/GTD, cancel, cancel-replace, atomic
  two-sided quotes, stops + stop-limits, OCO/brackets, hidden/iceberg. it's a real one.
- **an event scheduler** (`src/sch.c`) — everything is an event on a timeline: an order
  arriving, a client waking up, a bell ringing. the main loop in `src/main.c` just pops
  the next event forever until the kill event fires.
- **sessions + auctions** — 9:30–4 eastern, weekends skipped, opening and closing call
  auctions with a real uniform-price cross and a NYSE-style bell chain, plus a live
  imbalance (NOII) feed during the closing window.
- **exchange economics** — maker/taker fees, short-borrow interest, monthly market-data
  subscriptions. the venue takes its cut.
- **14 tiers of traders** (`src/strategy/`) — HFT market makers, snipers, order slicers,
  fundamental funds, retail degens, pension-fund glaciers, etc. each tier is its own file
  and its own little brain. `docs/TILTYARD_AGENTS.md` is the design doc for the whole
  population; `docs/ENGINE_FEATURES.md` is the running list of what the engine can and
  can't do yet.

Prices are plain integers (ticks). The plot script reads them as cents, so a price of
`100` shows up as `$1.00` — but internally it's just an int, treat it however you like.

## build + run

You need `gcc`  and `make`. That's it for the sim itself — it's pure C, no libraries.

```
make main
```

That compiles everything and immediately runs `./tiltyard`. It'll print some noise as it
goes — auction crosses, market opens, stops firing, the odd "doubling time" as a buffer
grows — and then a per-client balance sheet at the very end. The thing you actually want,
though, is the file it drops:

```
tiltyard.bin
```

That's the run log: every accepted order and every trade, written as raw fixed-size
records instead of text (formatting them inline cost ~12% of the runtime, so it's
deferred). It's a couple GB.

Other targets:

- `make test` — the unit-test suite (`tests/`). run this after any engine change.
- `make debug` — same as `main`, kept around for profiling:
  ```
  make debug
  xcrun xctrace record --template "Time Profiler" --launch -- ./tiltyard
  ```

## decoding the output

`tiltyard.bin` is binary. `logdump` turns it back into text — byte-for-byte the same
lines the sim used to print inline, so any grep/awk you'd have run on stdout still works:

```
make logdump
./logdump > f
```

Now `f` is human-readable. Two kinds of line:

```
[52200s] order #1 client #3 [$1000000000/$0/0q/0q] limit buy 200sh @ $98
TRADE buy 1 p 102 q 200 id 2 now 52268639722359 part 0
```

The `TRADE` lines are the ones where a trade actually happened. Field order is
`side side_n p <price> q <qty> id <order_id> now <ts_ns> part <n>` — pull those and you
have a tape you can bucket into candles by the nanosecond timestamp.

To plot: `ui/stocks.py` does exactly that — reads a file of trade lines, rolls them into
1-second / 1-minute / 1-hour candles, and throws up a plotly chart with volume bars. It
expects the trades in a file called `f` one directory up, so:

```
./logdump | grep TRADE > f
cd ui && python stocks.py     # needs plotly
```

## the layout, if you want to change something

The split that matters: **the engine** vs **the traders**.

- `src/` (minus `strategy/`) + `include/` — the engine. order book, scheduler, matching,
  the server that owns everyone's cash/shares, the snapshot + market-data machinery. this
  is the load-bearing part and it's fussy about invariants; `tests/` is the guardrail.
- `src/strategy/` + `include/strategy/` — the traders. one file per tier. each one is
  three pieces: a **params** struct (tunable knobs, all the interesting numbers live
  here), a **state** struct (what that agent remembers), and an **on_event** handler (what
  it does when it wakes up). they plug into the engine through one narrow interface
  (`include/client.h`) — they can submit orders, read whatever market data their
  subscription tier entitles them to, and schedule their own next wake. that's it. they
  can't reach into the engine.

So: want to add a trader? copy the closest tier in `src/strategy/`, register it in the
`IMPLS` list in `include/client.h`, and give it a headcount in `src/main.c` (the big
`client_allocations` block near the top). If you want to change *how many* of each kind, the
sim length, the seed, that's all right there in `src/main.c` too.

A few things worth knowing before you start pulling threads:

- **it's deterministic.** same seed (`server_init(..., 603)` in `main.c`) + same code →
  the exact same run, event for event. every agent carries its own RNG; there's no shared
  global random. this is on purpose — it's what makes a change's effect legible.
- **nothing here touches the outside world.** it's standalone and simulated-only by
  design. no sockets, no credentials, no real market data. keep it that way and it stays
  reproducible.
- the parameter values in the strategy files are mostly `/* UNCALIBRATED */` placeholders
  — they make the sim *run*, not necessarily *look like any real market*. calibrating
  them is the open-ended part.

If you're feeding this repo to an LLM to adapt it: point it at `docs/TILTYARD_AGENTS.md`
for how the trader population is meant to fit together, `docs/ENGINE_FEATURES.md` for
what the engine actually supports, and `include/order.h` for the order format everything
speaks. Those three plus `src/main.c` are the map.
