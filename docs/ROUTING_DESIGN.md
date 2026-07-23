# Routing / Brokerage Infrastructure — Design

**Status:** design only, not yet implemented. This is engine feature **F15** (routing / internalization / PFOF), the last gate for tier **T11 Toll Booths**.

**Scope.** Wire up the *infrastructure* so a special "broker" client can receive its managed clients' order flow and act on their behalf. **Do not** write the toller/broker strategy itself (internalize-vs-forward logic, NBBO pricing, hedging) — only the engine hooks a strategy would use. **No hop-2** (exchange-to-exchange Reg-NMS routing / ISO sweeps) — explicitly out of scope.

---

## The idea (PFOF)

A retail broker (Robinhood) sells its customers' orders to a **wholesaler** (Citadel Securities, Virtu). The wholesaler **internalizes** — fills the order off its own book at or inside the NBBO, keeps the spread, and hedges the residual on the lit market. The customer's order often never reaches the public book. See TILTYARD_AGENTS **T11 Toll Booths** for the participant, and ENGINE_FEATURES **F15** for the feature stub.

The interesting simulated effect: enabling routing **diverts uninformed (T7 Tapper) flow off the lit book, thinning visible liquidity.**

---

## Current order flow (before routing)

```
strategy on_snapshot returns an order (Context.next_order_ptr, action bit 1)
  → main.c CLIENT_IN sets empty->client_id = <the emitting client>, schedules CLIENT_OUT
  → main.c CLIENT_OUT schedules a SERVER event after calculate_jitter(...)
  → server_arrival(order_id) → matching; the server settles on Order.client_id
```

Key facts:
- The strategy interface is `u8 on_snapshot(void* self, Context* ctx)` (bit 1 = order, bit 2 = wake).
- An order settles to `Order.client_id`.
- All `ClientSettings` live in one array indexed by `client_id` (`sc->client_settings`).
- `CLIENT_OUT` (main.c) is the single choke point where an emitted order is handed to the server — **this is where the broker hop goes.**

---

## The three engine primitives

### 1. `broker_id` on `ClientSettings`

```c
// a managed client routes its orders through this broker instead of straight to the book.
// default MAX_U32 = direct (no broker)
u32 broker_id;
```

**Default gotcha:** `ClientSettings` is `calloc`'d in `holder_init`, so a zeroed `broker_id` would read as "managed by client 0" (a real client). The sentinel must be **`MAX_U32`** (from `constants.h`), and `holder_init` must set it as the default for every client *before* calling `get_settings` (so a client's `get_settings` can still override it for a managed client). Do **not** hardcode `4294967295` — use `MAX_U32`.

### 2. The routing delivery (reuse `on_snapshot`)

New status bit, e.g. `ROUTE_BIT` (bits 23–31 of `Order.status` are free; 0–22 are used).

In **`CLIENT_OUT`**:
```
order = fl_get(order_id)
if client_settings[order.client_id].broker_id != MAX_U32:
    # managed: deliver to the broker instead of the server
    build a Response{ client_id = broker_id, status = 1<<ROUTE_BIT, order_id = order_id }
    schedule CLIENT_IN to the broker
else:
    # direct: existing path — schedule SERVER event
```

In **`CLIENT_IN`**, when `ROUTE_BIT` is set: expose the routed order on the `Context` (its id / side / price / qty and the beneficiary `client_id`) so the broker's `on_snapshot` can inspect it and return its own action (internalize, forward, or drop). This is the "reuse `on_snapshot`" hop from the original sketch.

### 3. On-behalf-of settlement (the beneficiary)

When the broker forwards or executes for the customer, the trade must settle to the **customer's** account, not the broker's. Today `CLIENT_IN` stamps `empty->client_id = <invoked client>` (the broker) on the returned order, which would misattribute it.

Primitive: let the broker's returned order carry a **beneficiary** `client_id` distinct from the submitting broker. Cleanest options (pick at implementation time):
- a `Context.beneficiary` field the broker sets, which `CLIENT_OUT`/emit respects instead of overwriting `client_id`; **or**
- the broker forwards the *original* routed order id straight through (its `client_id` is already the customer).

With the beneficiary in place, a forwarded order settles to the customer via the normal server path — no direct account poking required for the forward case.

### Broker access to managed accounts (internalization only)

For **internalization** (broker fills the customer from its own inventory), the broker must debit/credit both the customer and itself. Since all `ClientSettings` are one array and the broker knows its customers' ids (from the routed order), this only needs the interface to **expose the managed clients' settings** to the broker strategy (a `ctx` accessor or a managed-list helper). Defer until the toller strategy is built; the **forward** path (primitive #3) needs none of this.

---

## Implementation stages

1. **`broker_id` field + `MAX_U32` default** in `ClientSettings` / `holder_init`. (smallest, no behavior change)
2. **`ROUTE_BIT`** + the `CLIENT_OUT` broker hop + `CLIENT_IN` routed-order exposure on `Context`.
3. **Beneficiary** primitive so a broker order settles to the customer.
4. **Test:** assign a managed client a `broker_id`; assert its emitted order is delivered to the broker (not the server), and that a broker-forwarded order settles to the customer's account.

Deferred (need the toller strategy): internalize/forward decision, NBBO + price improvement, hedging, and exposing managed-account settings for internalization. Out of scope: Reg-NMS hop-2.

---

## Constraints carried from the session

- Small, individually-reviewable edits; short one-line comments; no em-dashes; if-bodies on their own lines; good names; use existing constants (`MAX_U32`), never magic numbers.
- Standalone sim only — no real broker/PFOF connections, no external coupling (TILTYARD hard rules).
- Build via `make test` / `make main`.
