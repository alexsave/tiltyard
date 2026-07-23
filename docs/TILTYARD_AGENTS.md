# TILTYARD — Agent Population Design Doc

**Scope:** the agent/client population layer only. Fourteen participant tiers, their parameterization, and the population builder that assembles them.

**Not in scope:** the engine. The matching engine, order book, event scheduler, snapshot architecture, and message transport are hand-built in C by the repo owner and are **not to be modified**. This doc describes code that *plugs into* that engine through the existing client interface.

---

## 0. Hard rules

Read these first. They are constraints, not preferences.

1. **Do not modify the engine.** Matching, order book, scheduler, snapshots, transport are owned and off-limits. If a tier needs an engine feature that does not exist yet (§2 lists them), **stop and report it** — do not implement it, do not work around it, do not stub the engine.
2. **Build mechanism, expose constants.** Every behavioral knob — wake cadence, response delay, maker/taker lean, order size, inventory limit, capital — goes in a named-constant parameter block that the owner can tune. **Never hardcode a behavioral value inline.** The code is your job; calibration is the owner's.
3. **Do not calibrate.** All parameter values in this doc are placeholders. Mark every default `/* UNCALIBRATED */`. Do not tune them to make anything "look right."
4. **Isolation guard.** Tiltyard is standalone and simulated-only. No dependency, import, network call, shared DB/queue, or coupling reaching toward **Crossbow**, **Cannon**, **Archer**, any CRM/billing/Stripe, or any third-party credentials. No real broker connections, no real market data ingestion, no real orders. All flow is synthesized. If a change would violate this, stop and flag it.
5. **Determinism.** Same seed + same inputs → same event ordering. Every agent carries its own RNG state; no shared global RNG.

---

## 1. Agent interface contract

Agents plug into the existing client API:

```c
void (*handler)(uint64_t sim_time, const mbo_event_t *ev, void *state, client_ctx_t *ctx);
```

`client_ctx_t` exposes: `submit_order`, `get_book` (latency-gated internally), `poll_inbox`, `schedule_wake`.

Every tier is implemented as three things, strictly separated:

- **`<tier>_params_t`** — const, tunable, loaded from config. Never mutated at runtime.
- **`<tier>_state_t`** — mutable per-agent state, lives in the `void *state` blob.
- **`<tier>_on_event()`** — the handler.

The params/state split is load-bearing: a sweep harness must be able to vary params across runs without touching code or recompiling agent logic.

### 1.1 Common parameter block

Every tier embeds this. Tier-specific params extend it.

```c
typedef enum {
    WAKE_EVERY_EVENT,      /* apex: every MBO update */
    WAKE_INTERVAL,         /* fixed period */
    WAKE_POISSON,          /* stochastic arrival rate */
    WAKE_CALENDAR,         /* scheduled: daily / monthly / quarterly */
    WAKE_SIGNAL,           /* triggered by exogenous signal */
    WAKE_PRICE_LEVEL       /* triggered when price crosses a threshold */
} wake_model_t;

typedef enum { ACCT_CASH, ACCT_MARGIN } account_type_t;
typedef enum { DATA_MBO, DATA_L2, DATA_L1, DATA_L0, DATA_NONE } data_tier_t;

typedef struct {
    wake_model_t   wake_model;
    uint64_t       wake_param_ns;      /* period, or mean interarrival */
    uint64_t       response_delay_ns;  /* see §1.2 — the load-bearing param */
    uint64_t       response_jitter_ns; /* delay is drawn, not fixed. see §1.2 */
    double         taker_probability;  /* 0.0 pure maker .. 1.0 pure taker */
    dist_t         order_size_dist;
    int64_t        inventory_min;      /* signed; negative => short allowed */
    int64_t        inventory_max;
    int64_t        cash;
    account_type_t account_type;
    data_tier_t    data_tier;          /* gates what get_book() may return */
    uint32_t       order_type_mask;    /* permitted order types */
    double         tod_modulation;     /* 0..1, strength of intraday U-curve */
    uint64_t       rng_seed;
} agent_common_params_t;
```

### 1.2 Response delay — the single most important parameter

Processing latency and network latency **sum into one number**: the offset added to `sim_time` between the event that triggers an agent and the moment its order lands in the matching engine. For the apex this is nanoseconds; for retail it is effectively hundreds of milliseconds; for the bottom tiers it is irrelevant because those agents are not reacting to ticks at all.

**Draw the delay from a distribution, do not use a fixed constant.** A deterministic per-agent offset is an exploitable regularity — a strategy-search loop will arbitrage the exact offset rather than learn anything about markets. `response_delay_ns` is the mean; `response_jitter_ns` is the spread.

### 1.3 Time-of-day modulation

One shared function, applied to every tier's wake rate: an intraday U-curve (heavy at open and close, quiet midday). Each tier sets `tod_modulation` for how strongly it applies. This single mechanism produces the intraday volume U-curve stylized fact (§4) across the whole population.

### 1.4 Population modes

Each tier is independently configurable as:

- **`POPULATED`** — instantiate real agent structs. Low-frequency tiers use the slow scheduler + active-client-list pattern (agents store `next_wake` in their state and are swept hourly rather than consuming fast-scheduler slots). This is what makes tens of millions of sleeping retail clients viable, and exercising that path is a deliberate architectural goal.
- **`AGGREGATE`** — collapse the tier into a calibrated arrival-rate generator. The matching engine cannot distinguish an order from a weekly Robinhood checker from an order from a noise process — once it is in the book, provenance is gone. For apex-focused experiments, collapsing the bottom tiers is a large speedup at zero fidelity cost.

Mode is **per-tier**, set in config (e.g. `TIER_TAPPERS_MODE=AGGREGATE`), not global.

---

## 2. Engine feature dependencies

Tiers are gated on engine features. Build tiers whose dependencies are satisfied; leave the rest behind feature flags that compile but refuse to run with a clear error. **Do not implement missing engine features.**

Status is as of the closing-auction lifecycle work (dual book during the window, hold-all-day MOC, the offset-only cutoff, the NOII imbalance feed) and the exchange-fee layer (maker-taker, short borrow, market-data subscriptions). `ENGINE_FEATURES.md` is the detailed catalog; this is the tier-facing summary.

- **P0 — Plumbing:** ✅ order handles/IDs returned at submit (prereq for any cancel/replace/modify); reschedule-wake API (`ctx->wake_delay_ns` + bit 2 of the action).
- **P1 — Apex feedback loop:** ✅ **except reject reasons.** Private fill/execution reports, fired **only on fills**, not on every event; marketable-limit + IOC residual flag (FOK did fall out of the same mechanism); paired atomic two-sided cancel-replace (mass quote), including the atomic **pull** of both quotes in one pass. **Gap:** a reject is still a single undifferentiated `REJECT_BIT` — no reason code, so a tier cannot tell "no buying power" from "bad price" and react differently. Note an IOC that fills nothing also comes back as a reject, so `REJECT_BIT` is carrying two meanings until F1 lands.
- **P2 — Apex intelligence:** ✅ **both halves.** Queue-position ✅ — as a client-side helper (`ob_queue_position`, read off the MBO) rather than piggybacked on the ack. Short/margin accounts ✅ — `cash`/`shares` are now `i64` (negative = loan / short) and margin clients gate on Reg T buying power (`client_bp` in `server.c`) instead of flat cash/share checks. Remaining margin work (forced liquidation, borrow interest, a live margin population) is fidelity, not a representational gap.
- **P3 — Realism engine:** ✅ engine-pushed news signal on the `Context` (`news_signal`, `last_news_ns`). Not subscribable — every client sees it, so tiers that must stay uninformed have to ignore it by construction. **The fundamental-value process is not built:** the signal is an abstract 0–255 company-health level and mapping it to a fair value is the client's job.
- **P4 — Triggered & hidden orders:** ✅ **complete.** hidden/iceberg (display qty < total qty) ✅ — `ICEBERG_BIT`, tip in `quantity` / total in `second_quantity`, replenished through the convert path, marketable ones fill-then-rest, cancel kills the whole order. Multi-session time-in-force ✅ — **DAY** (`DAY_BIT`) dies at the close, **GTD** (`GTD_BIT`, date in `second_id`) dies at its date's close, **GTC** is the default that survives; the close's expiries are pruned from the book in one snapshot (`ob_expire`), reserves released, `CXL_SESSION_CLOSE` sent. Stop / stop-limit ✅ — buy-stops in a min-heap, sell-stops in a max-heap (`XPQ`), checked against each trade print; a triggered stop converts in place (stop→market IOC, stop-limit→limit) and re-enters via the convert path, arrival-ordered by an `Order.ns` stamp so recycled ids don't scramble priority. A stop's params ride the `second_*` fields, so one order can carry a NOW half **and** a stop half (all four market/limit × market/limit combos), and canrep can cross the stop/book boundary Fidelity-style. **OCO** ✅ — `OCO_BIT` + mutual `other_id`; brackets (take-profit limit + protective stop, one message) pull the sibling whichever completes first, via the same convert path, with an `ns` recycled-id guard.
- **P5 — Market structure:** ⚠️ **session boundary + the full closing-auction lifecycle are in.** Sessions: 9:30–4 eastern (14:30–21:00 UTC, no DST), open/close reschedule each other, weekends skipped, orders while closed rejected `REJ_MARKET_CLOSED`. **Auction ✅:** a real uniform-price cross at open and close on the exact NYSE bell chain (6:30/9:29:55/9:30, 15:50/15:59/16:00), single clearing price, market-first fill, arrival-order tie-break, resting-book merge (consumed via one `ob_canrep`, makers settle at clearing), cash/shares conserved. **Closing-auction lifecycle ✅:** the book stays **live through the closing window** (dual book — regular orders trade continuously, only `AUCTION_ONLY_BIT` MOC/LOC park); MOC is **held from whenever it's entered** and joins the right cross by timing (pre-open → open, live → close); the **NOII imbalance feed** publishes the running buy/sell imbalance each second to `TIER_IMBALANCE` subscribers; and past the cutoff the **offset-only rule** (`REJ_OFFSET_ONLY`) accepts only interest that relieves the imbalance. Clients can read the session phase (`is_open`/`auctioning`/`auction_frozen`) and the last trade price off the `Context`. The NOII also carries the **indicative clearing price** (a read-only `auction_walk` that reuses the cross walk, so it matches what the cross settles at). Still ❌ halts, D-orders, the formal **DMM last-look** hook (the exchange handing a designated client the exact residual); routing + internalization + declared cross (two different parties at an agreed price with price-improvement rules — distinct from the self-cross already rejected at validation).

Gating summary: Flickerers need P0+P1+P2. Snipers need P0+P1+P2. Slicers need P0+P1. Suits and Oracles need P3. Degens and Setters need P4. Tides and DMMs need P5's auction + imbalance feed (shipped). Toll Booths need P5 routing (not shipped). Tappers, Metronomes, Glaciers need nothing new.

**Buildable today: Flickerers, Snipers, Slicers, Suits, Oracles, Tappers, Metronomes, Glaciers, Degens, Setters, Tides, DMMs** (Glaciers only once Slicers exist — it's a Slicer client, not a book participant). **Snipers are buildable in full** — P2's margin half landed, so a pure taker can short and cover instead of being stuck long-only. **Degens and Setters** were unblocked by P4 (stops + OCO). **Tides and DMMs are now unblocked** — P5's auction plus the NOII imbalance feed and offset-only cutoff shipped, so MOC flow crosses in a real closing auction and a DMM can offset the published imbalance with ordinary auction-only orders. **Only Toll Booths remain gated**, on P5 routing.

---

## 3. The fourteen tiers

Real-world counts are order-of-magnitude, US-centric where it matters. Firm counts and account counts are hard anchors; the retail behavioral archetypes (Degens, Setters, Oracles) are **penumbra estimates, not registry counts** — treat them as such. Sim instantiation counts are **starting points, uncalibrated**.

### T1 — Flickerers (HFT market makers)

**Real world:** ~20–50 firms that actually matter (Citadel Securities, Virtu, Jane Street, Jump, HRT, IMC, Optiver…); low hundreds counting all registered MMs and prop shops.
**Sim:** 5–20.
**Wake:** `WAKE_EVERY_EVENT` — every MBO update at or near their level.
**Response delay:** 50ns–5µs. FPGA tick-to-trade 10–100ns; software path single-digit µs; colocated cross-connect sub-µs, on the direct feed not the SIP.
**Lean:** `taker_probability ≈ 0.02–0.05`. 95%+ maker — continuous two-sided quotes, taking only to hedge inventory or flatten after being picked off.
**Capital:** firm base in the billions, but the binding constraint is a tight per-symbol inventory limit (±2k–5k shares). Risk is measured in inventory units, not cash.
**Order sizes:** small, at-touch, round lots (100–500).
**Order types:** LIMIT (post), MASS_QUOTE (paired atomic cancel-replace), marketable IOC for hedging.
**Data:** MBO (L3), full.
**Strategy:** inventory-skew two-sided quoting. Quote bid/ask around mid; skew *both* quotes away from inventory (long → lower both, inviting sells; short → raise both). Requote on queue-position slip. Widen or pull on detected sweep — being picked off during a sweep is exactly the adverse selection this agent exists to avoid.
**Params:** `quote_size`, `base_half_spread_ticks`, `inventory_limit`, `skew_coeff` (ticks per unit inventory), `requote_queue_slip_threshold`, `sweep_pull_threshold`, `max_quote_age_ns`.
**Deps:** P0, P1, P2.
**Notes:** generates the *majority of message traffic* in the sim despite tiny headcount; quote-to-trade ratios 100:1+. Known landmines from the prior MM implementation, all of which must be handled: ask quantity must be sized off `quote_size`, **not** off inventory; ask qty capped by held shares (cash acct) or margin; bid qty capped by buying power; guard integer truncation of the spread (produces a locked market); watch unsigned overflow in cash accounting.

### T2 — Snipers (HFT takers / latency arbs)

**Real world:** ~100s of firms, heavily **overlapping** T1 — the same desks often run both sides.
**Sim:** 10–30.
**Wake:** `WAKE_EVERY_EVENT`, but **fire only on signal** — far fewer messages than Flickerers, yet every one is aggressive.
**Response delay:** same or faster than T1 on the take path. 50ns–2µs. The race to hit a stale quote is won by nanoseconds.
**Lean:** `taker_probability ≈ 0.90–1.0`.
**Capital:** billions; holds near-zero inventory, flattens instantly.
**Order sizes:** matched to the target quote's displayed size.
**Order types:** MARKETABLE_LIMIT + IOC (take displayed size at displayed price, discard residual); FOK for arb legs that must fill complete. **Not plain market** — always price protection.
**Data:** MBO.
**Strategy:** detect a stale quote (resting order at a price the agent's fair-value estimate says is wrong), race to hit it. Fair value from the signal stream, or from microprice/book imbalance. Picks off Flickerers' stale quotes.
**Params:** `fair_value_source`, `staleness_threshold_ticks`, `min_edge_ticks`, `max_position`, `flatten_urgency`.
**Deps:** P0, P1, P2 (**must be able to short and cover** — this tier is broken in a cash-only account model).
**Notes:** the natural predator of T1. **Pairing T1 against T2 is what creates self-sustaining churn instead of a frozen book** — the original freeze happened because every agent was a maker resting liquidity with nobody crossing.

### T3 — Slicers (execution algos / systematic)

**Real world:** ~100s of algo providers (every bank, agency broker, vendors); ~1,000s counting quant/systematic funds as entities. Far more *parent orders in flight* than firms.
**Sim:** 50–150, or: N active parent orders.
**Wake:** `WAKE_INTERVAL` (every N ms–s) plus on fills, to track schedule.
**Response delay:** milliseconds. Not racing. Near-colo but nanosecond edge is unnecessary.
**Lean:** `taker_probability ≈ 0.5`, **urgency-dependent** — passive posting when on schedule, aggressive crossing when behind (the implementation-shortfall tradeoff).
**Capital:** the *client's*, not theirs — they are agents. Parent orders 100k–1M+ shares.
**Order sizes:** children = round lots to a few hundred, under a participation cap (e.g. 10% of volume).
**Order types:** LIMIT (passive), MARKETABLE_LIMIT (catching up).
**Data:** L1/L2 + own fills.
**Strategy:** TWAP / VWAP / POV / IS. Schedule the parent across a horizon. Each wake: compare filled vs scheduled; post passive if on-track or ahead, cross if behind; respect the participation cap.
**Params:** `parent_qty`, `parent_side`, `horizon_ns`, `algo_type`, `participation_cap`, `child_size_dist`, `aggression_curve`, `urgency`.
**Deps:** P0, P1 (fills are critical — they track progress *by* fills).
**Notes:** this is the tier that produces **order-flow autocorrelation** (persistent same-side child streams). It is also the channel through which Suit and Glacier flow actually reaches the book.
**Required flag — `slice_disabled`:** when set, the agent dumps the entire parent as a single marketable order (equivalently: `participation_cap = 1.0`, `horizon_ns = 0`). This is the fat-finger / "forgot to slice" experiment; wire it as a config flag on this tier rather than as a separate agent type. Most extreme configuration: a Glacier-sized parent (T13) routed through a `slice_disabled` Slicer.

### T4 — Suits (discretionary hedge funds)

**Real world:** ~10k hedge funds globally; tens of thousands counting all active institutional/long-only.
**Sim:** 50–100.
**Wake:** `WAKE_SIGNAL` (news/catalyst) or a few times a day.
**Response delay:** seconds to hours. Human-in-the-loop or slow-systematic — a PM decides, then hands to execution. Network latency is irrelevant at this cadence.
**Lean:** taker-leaning *when they decide to move*, but flow routes through Slicers and becomes sliced children. Directly, they are a low-frequency signal source.
**Capital:** AUM tens of millions to hundreds of billions. Position sizes are a meaningful fraction of daily volume.
**Order types:** emits **parent orders to a Slicer**, not direct book orders. Make routing configurable (`DIRECT | VIA_SLICER`); default `VIA_SLICER`.
**Data:** L1/L0 + the signal stream.
**Strategy:** on signal, form a view, size a position, hand to execution.
**Params:** `aum`, `signal_sensitivity`, `conviction_to_size_fn`, `position_limit`, `routing`, `decision_delay_ns`.
**Deps:** **P3.** Without a signal process this tier is inert — it has nothing to be informed *about*.
**Notes:** this is the **informed flow**. It is the adverse selection that makes Flickerer queue-modeling earn anything, and it is the tier that produces **volatility clustering and fat tails**. Highest-value tier for realism after the apex.

### T5 — Degens (WSB / day traders)

**Real world:** ~low millions genuinely active retail day-traders, with a much larger occasional penumbra.
**Sim:** 100s–1000s, or AGGREGATE.
**Wake:** `WAKE_SIGNAL` (news / social / price action). Intraday, bursty, clustered on volatile names and events — **herd flow is heavily correlated** (everyone hits the same name at once; implement via a shared signal or correlated RNG, not independent draws).
**Response delay:** human seconds-to-minutes reacting to a spike or a post, plus retail broadband 10–100ms to the broker, plus broker/PFOF routing — effectively **hundreds of ms wire-to-book**.
**Lean:** `taker_probability ≈ 0.8`.
**Capital:** thousands to low-hundreds-of-thousands. Concentrated, leveraged through options/margin.
**Order types:** MARKET, MARKETABLE_LIMIT, impatient LIMIT, **STOP** (protective stop-losses).
**Data:** L1 — BBO and last trade in a broker app.
**Strategy:** momentum. Buy strength, sell weakness.
**Params:** `capital`, `herd_correlation`, `momentum_lookback`, `stop_loss_pct`, `position_concentration`, `burst_rate`.
**Deps:** P4 (stops) ✅ — buildable now.
**Notes:** **the stop-loss population is the fuel for the cascade.** Stops must be lopsided to the sell side (most participants are long, so protective *sell*-stops sit below the price far more densely than buy-stops sit above it). This asymmetry is why real flash crashes are almost always *crashes* — the book's reflexive fuel is on the downside. A symmetric melt-up requires deliberately seeding a short-heavy population to create a buy-stop field.

### T6 — Apes (crypto leverage)

**Real world:** millions to tens of millions globally.
**Sim: OUT OF MODEL. Different venue entirely — never appears in a US-equity book.**
24/7 perpetuals with funding rates and liquidation cascades: a foreign microstructure. Taker-heavy momentum with reflexive liquidation-driven flow; tiny accounts at 10–100x leverage.
**Implementation:** stub only. Define the tier for taxonomic completeness, gate it behind a venue flag, and have it refuse to run against the equity book with a clear error. If a crypto venue is ever built, the liquidation-cascade dynamic is the whole personality and needs its own machinery (funding rate accrual, forced-liquidation injection) — that is a separate design doc, not this one.

### T7 — Tappers (Robinhood retail)

**Real world:** tens of millions. Robinhood alone ~25M funded accounts; total US retail accounts 100M+.
**Sim:** this is where the sleeping-client architecture earns its keep. `AGGREGATE` for apex experiments; `POPULATED` (millions, slow scheduler) for the full emulation and as an architecture stress test.
**Wake:** `WAKE_POISSON`, low per-agent frequency. Clustered around the open, news, and paydays. Strong `tod_modulation`.
**Response delay:** human seconds once decided; retail broadband plus PFOF routing to a wholesaler — hundreds of ms, and the order **often never touches the lit book** (see T11).
**Lean:** `taker_probability ≈ 0.95`. Near-pure taker — marketable orders hitting the displayed price.
**Capital:** small. Median account low thousands; fractional shares.
**Order types:** MARKET, MARKETABLE_LIMIT.
**Data:** L1/L0, sometimes delayed.
**Strategy:** essentially none. Weak sentiment drift, mild net-long bias.
**Params:** `wake_rate`, `order_size_dist`, `buy_bias`, `tod_modulation`.
**Deps:** none new.
**Notes:** the canonical **uninformed flow**. Wholesalers pay for it precisely because it carries no adverse selection — that property must hold in the model, so **do not give this tier predictive signal**. It is also the flow the informed tiers trade *against*; without it, informed flow has no one to be informed against.

### T8 — Setters (swing traders)

**Real world:** low-to-mid millions. A fuzzy behavioral slice, not a registry count.
**Sim:** 1000s, or AGGREGATE.
**Wake:** `WAKE_CALENDAR`, daily-ish — check after work, place orders for the next session.
**Response delay:** effectively irrelevant. They are *placing resting orders*, not reacting in real time.
**Lean:** `taker_probability ≈ 0.3`. More balanced than Tappers — they *set* limits and walk away (hence the name).
**Capital:** moderate. Disposable income; thousands to low-hundreds-of-thousands.
**Order types:** LIMIT (patient, away from the touch), **STOP**, and **GTC/GTD** — orders that survive across sessions.
**Data:** L1, EOD review.
**Strategy:** technical swing trading. Entry limits at support, stops below, hold days to weeks.
**Params:** `check_time_of_day`, `entry_offset_ticks`, `stop_loss_pct`, `hold_horizon_days`, `order_ttl`.
**Deps:** P4 — both stops **and multi-session TIF**, now both done (stops via the trigger heaps, TIF swept at the close). Buildable now.

### T9 — Oracles (value investors)

**Real world:** tens of thousands as an archetype — **very fuzzy**. This is a *behavior*, not a headcount. True concentrated value pickers are professionally rare; the number is larger if you mean retail buy-and-hold.
**Sim:** 10s–100s.
**Wake:** `WAKE_PRICE_LEVEL` (price crosses a fair-value threshold) or `WAKE_CALENDAR` (quarterly/annual). **They sleep for millions of events** and most of the time send nothing.
**Response delay:** irrelevant.
**Lean:** maker. Patient limit orders far from the touch; slow accumulation.
**Capital:** enormous (Berkshire) to modest retail. Concentrated positions held for years.
**Order types:** LIMIT (far from touch); optionally ICEBERG to hide size.
**Data:** `DATA_NONE` live — they act on the **fundamental value** component of the signal process, not on the price tape.
**Strategy:** **mean-reverting / contrarian.** Buy when price << fair value, sell when price >> fair value.
**Params:** `fair_value_estimate` (a noisy read of the true fundamental), `value_threshold_pct`, `accumulation_rate`, `capital`, `patience`.
**Deps:** P3 (needs a fundamental-value process to hold an opinion about).
**Notes:** **this is the arresting loop.** In the fat-order experiment, whether the move runs away or mean-reverts is decided by contrarian capital depth versus the cascade amplifiers (vanishing liquidity + stop cascade). Near-zero flow contribution normally; large position footprint when it finally acts. **The depth of Oracle capital is the single most important calibration knob for crash dynamics** — same trigger, opposite outcomes, and the difference is this parameter.

### T10 — Metronomes (401k / DCA)

**Real world:** **100M+ individuals — the largest tier by a wide margin, with near-zero flow each.** ~60% of US households hold stock (~80M households; per-person count is higher, per-active-intraday-trader count is dramatically lower — three different numbers, don't conflate them).
**Sim:** `AGGREGATE` strongly recommended. `POPULATED` only as a scaling stress test of the slow scheduler.
**Wake:** `WAKE_CALENDAR`, biweekly/monthly, payday-driven. Each agent wakes ~24×/year.
**Response delay:** irrelevant — automated and batched through fund administrators.
**Lean:** taker, via automated periodic buys. **Completely price-insensitive** — they buy regardless of level. Do not give this tier any price logic.
**Capital:** aggregate is the entire retirement system; per-person contribution is tiny.
**Order types:** MARKET.
**Data:** `DATA_NONE`. They watch nothing.
**Params:** `contribution_amount`, `contribution_period_ns`, `payday_phase`.
**Deps:** none.
**Notes:** collapses to a small, steady, predictable aggregate **buy-pressure rate**. Most of their flow in reality arrives **through Tides** — they buy index funds, which Tides then rebalance. Model as either (a) direct small buys, or (b) a demand stream feeding T12. **(b) is the more correct model**; implement (a) first, make (b) a config option.

### T11 — Toll Booths (PFOF wholesalers)

**Real world:** **a handful.** ~6–10 wholesalers that matter — and frequently the *same firms* as the Flickerers. Retail brokers in the dozens.
**Sim:** 1–5.
**Orthogonal to the other twelve: not directional investors, but intermediaries.**
**Wake:** continuously — process retail flow as it arrives, hedge in near-real-time.
**Response delay:** HFT-grade. They *are* HFT firms wearing a second hat.
**Lean:** maker to retail (fill at or inside the NBBO off their own book); maker/taker to the lit book (re-express residual inventory plus hedges).
**Capital:** billions.
**Strategy:** internalize the Tapper/Degen stream, fill at or inside NBBO, hedge residual inventory into the lit market. Economics: spread captured on uninformed flow − hedging cost − PFOF paid.
**Params:** `internalization_rate`, `price_improvement_ticks`, `hedge_threshold`, `pfof_rate`.
**Deps:** **P5** (routing/internalization layer + declared-cross primitive — two *different* parties at an agreed price with price-improvement rules; distinct from the self-cross that validation already rejects, since self-crossing always produces zero or negative spread and is never legitimately profitable).
**Notes:** **mostly garnish for a single-book sim.** Implement last, behind a feature flag. But note the real consequence when enabled: it **diverts Tapper flow away from the lit book**, which materially thins lit liquidity — that is the interesting effect, and the reason to build it at all.

### T12 — Tides (index / ETF rebalancers)

**Real world:** dozens of managers (Vanguard, BlackRock, State Street, Fidelity); hundreds-to-thousands of individual funds/ETFs rebalancing on schedules.
**Sim:** 5–50.
**Wake:** `WAKE_CALENDAR`, **highly predictable** — quarterly index reconstitutions, daily creation/redemption, end-of-day NAV matching.
**Response delay:** not latency-sensitive, but intensely **timing**-sensitive. They must hit the close.
**Lean:** taker at specific times. **Heavy market-on-close** — index funds trade the closing auction to match closing-price NAV.
**Capital:** enormous (trillions AUM), mechanically deployed against tracking error.
**Order types:** **MOC / MOO**; large parent orders routed via Slicers for the non-auction portion.
**Data:** index composition + closing price.
**Params:** `aum`, `rebalance_calendar`, `moc_fraction`, `tracking_error_tolerance`.
**Deps:** ~~**P5 (call auction + session phases)**~~ ✅ — the auction shipped, so MOC/MOO cross in a real closing auction. Buildable now.
**Notes:** their MOC flow is the **single biggest, most predictable liquidity event of the day**, and it is what produces the closing-auction volume spike (§4). Also the sink for Metronome contributions.

### T13 — Glaciers (pensions / SWF / endowments)

**Real world:** thousands to tens of thousands. US public pensions ~5,000+; SWFs ~100; large endowments in the hundreds.
**Sim:** 10s–100s.
**Wake:** `WAKE_CALENDAR` — **the slowest of all tiers.** A few times a year, on allocation/rebalance decisions. Sleeps longer than anything else in the sim.
**Response delay:** irrelevant. Cadence is committee-driven.
**Lean:** taker, but the slowest imaginable — huge orders worked over days or weeks **entirely through Slicers**, so *direct* flow is near-zero.
**Capital:** **the largest single pool.** CalPERS ~$500B; Norway's SWF ~$1.7T. Enormous positions, deployed patiently, impact-averse.
**Order types:** emits massive parent orders to Slicers; accepts slow fills.
**Data:** `DATA_NONE` directly — their executing brokers watch the tape.
**Params:** `aum`, `target_weight`, `rebalance_threshold`, `rebalance_frequency`, `execution_horizon_days`, `impact_aversion`.
**Deps:** Slicers (T3) must exist — this tier is a Slicer *client*, not a direct book participant.
**Notes:** pair with T3's `slice_disabled` flag for the maximal fat-finger scenario.

### T14 — Specialists (NYSE DMMs / designated market makers)

**Real world:** **a handful** — ~3–5 DMM firms (GTS, Citadel Securities, Virtu, Jane Street) cover every NYSE-listed name, one DMM assigned per stock. Nasdaq is pure-electronic with no DMM, so this tier is NYSE-specific.
**Sim:** 1 per symbol (so 1 here).
**Orthogonal to the twelve directional tiers, like Toll Booths: an obligated liquidity provider, not an investor.**
**Wake:** `WAKE_EVERY_EVENT` intraday; `WAKE_SIGNAL` on the **NOII imbalance feed** during the accumulation windows — the auction is its defining moment.
**Response delay:** HFT-grade. DMMs *are* electronic market-making firms wearing a franchise obligation.
**Lean:** maker. Continuous two-sided quotes with an affirmative obligation to sit at the NBBO a set fraction of the time; at the cross it supplies the **offsetting side of the imbalance**.
**Capital:** billions (frequently the same firms as Flickerers and Toll Booths).
**Order types:** LIMIT (continuous quotes) and **auction-only offset orders** (`AUCTION_ONLY_BIT`, contra to the published imbalance) into the closing/opening cross.
**Data:** MBO **+ the NOII imbalance feed** (`TIER_IMBALANCE`, the $500/mo add-on) — it trades *against* the published imbalance, so the feed is its core input.
**Strategy:** absorb the auction imbalance. Read the NOII each second; if the book is buy-heavy, post offsetting sell interest (and vice versa) so the cross clears near the reference price instead of gapping. Intraday, quote two-sided like a Flickerer but under an obligation floor.
**Params:** `imbalance_offset_fraction` (share of the published imbalance to absorb), `max_auction_commitment`, `quote_obligation_pct`, `intraday_half_spread_ticks`, `inventory_limit`.
**Deps:** **P5** — the call auction ✅ **and the NOII imbalance feed ✅**, both shipped, and the offset-only cutoff already enforces the contra-side rule. **Buildable now as a pure client**: it offsets the published imbalance with ordinary auction-only orders, no engine change required. The formal DMM **last-look** — the exchange handing the DMM the exact unpaired residual to fill at the clearing price against its obligation — is an engine hook that does **not** exist yet; per the hard rules, gate that behind a flag and report it rather than building it.
**Notes:** this is the tier that **dampens closing-auction gaps** — the counterweight to Tides. A Tides MOC imbalance with no DMM clears against whatever offset interest happens to exist and can print far from the reference; a DMM sized against the imbalance pins the close near fair value. **Pairing T12 (imbalance source) against T14 (imbalance sink) is the closing-auction analog of the T1/T2 pairing that keeps the continuous book from freezing.**

---

## 4. Acceptance criteria — stylized facts

The population is "correct" when the synthetic tape reproduces the stylized facts of a real busy instrument. **Build the measurement harness; do not tune the parameters.** The owner tunes; your job is to make tuning measurable.

Compute these from the tape and expose them as a scored report:

1. **Fat-tailed returns** — excess kurtosis far above Gaussian. *Produced by:* T4 + signal process.
2. **Volatility clustering** — positive, slowly-decaying autocorrelation of *absolute* returns, while signed returns remain uncorrelated. *Produced by:* T4, informed flow arriving in bursts. **This is the fact a naive sim fails first** — a book with no informed-flow signal produces near-Gaussian returns no matter how good the matching engine is.
3. **Spread and depth profile** — characteristic book shape; thins under stress. *Produced by:* T1.
4. **Order-flow autocorrelation** — signed flow is persistent; buys follow buys. *Produced by:* T3, parent-order slicing.
5. **Intraday volume U-curve** — heavy at open and close, quiet midday. *Produced by:* `tod_modulation` across all tiers.
6. **Closing-auction volume spike.** *Produced by:* T12 + P5 auction.

Deliver as `analysis/stylized_facts.{c,h}`, runnable against a recorded tape, emitting all six metrics.

---

## 5. Deliverables

```
agents/
  agent_common.h          /* param block, wake models, distributions, RNG */
  tier01_flickerers.{c,h}
  tier02_snipers.{c,h}
  ...
  tier13_glaciers.{c,h}
  tier14_dmms.{c,h}
  registry.{c,h}          /* tier id -> constructor */
  population.{c,h}        /* build a population from a config */
  aggregate_flow.{c,h}    /* AGGREGATE-mode arrival-rate generator */
config/
  population_baseline.h   /* named constants, ALL marked UNCALIBRATED */
  population_fat_finger.h /* T3 slice_disabled, Glacier-sized parent */
  population_latency.h    /* response_delay sweep harness config */
analysis/
  stylized_facts.{c,h}
```

## 6. Implementation order

Dependency-ordered and apex-first. The first four items produce a book that churns; everything after that adds realism.

1. `agent_common.h` + `registry` + `population` scaffolding.
2. **T1 Flickerers** (P0, P1, P2).
3. **T2 Snipers** (P0, P1, P2). *After 2 and 3 the book stops freezing: makers rest, takers cross, both get feedback.*
4. `aggregate_flow` — background uninformed noise, so there is something to trade against.
5. **T3 Slicers** (P1 fills).
6. Signal-stream consumer + **T4 Suits** (P3). *This is the largest single realism gain.*
7. **T9 Oracles** (P3).
8. **T5 Degens**, **T8 Setters** (P4 — now unblocked).
9. **T7 Tappers**, **T10 Metronomes** — AGGREGATE first, POPULATED second.
10. **T13 Glaciers** (needs T3).
11. **T12 Tides** (P5 auction).
12. **T14 DMMs** (P5 auction + NOII imbalance feed). *Pair against Tides — imbalance source vs sink, the closing-auction analog of T1/T2.*
13. **T11 Toll Booths** (P5 routing).
14. **T6 Apes** — stub, venue-gated, not implemented.

---

## 7. Non-goals

- Do not touch the engine, matching, order book, scheduler, snapshots, or transport.
- Do not implement missing engine features. Report them and stop.
- Do not hardcode behavioral constants.
- Do not calibrate, tune, or "make it look realistic."
- Do not add dependencies, network calls, external services, or any coupling to Crossbow / Cannon / Archer.
- Do not ingest real market data or connect to a real broker. All flow is fabricated.
