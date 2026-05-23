# W15 venue-stack reproducibility

Backtest results are only useful if they are reproducible. A
strategy backtested today must produce the same numbers when
re-run tomorrow on identical input. Silent non-determinism is the
worst class of backtest bug — results look plausible, decisions
get made, then re-runs disagree and every prior result becomes
suspect.

This page is the audit trail for every potential
non-determinism source introduced across W15 round 4 + 5 (28
PRs), with the resolution for each.

## Potential sources, audited

### 1. Iceberg refresh jitter (T029, T041)

`SimulatedExecutor::IcebergState` carries a per-engine RNG used
to randomise slice size when `setIcebergSizeRandomisationPct` is
non-zero. The seed is set via `setIcebergJitterSeed(seed)`, and
both the executor and the strategy author control it.

**Resolution:** deterministic — same seed → same jitter sequence.
Default seed when none is set is a stable constant
(`0xC0FFEEC0FFEEULL`).

### 2. Cross-margin liquidation worst-leg ranking (T037)

`LiquidationEngine::walkCrossAccount` ranks attached-account
positions by uPnL (ascending) with a deterministic tie-breaker
(absolute notional descending). Both the input vector and the
sort are stable.

**Resolution:** deterministic. No `unordered_map` lookups in the
ranking path; positions iterate from `account.positionsMut()`
which is a `std::vector` — insertion-ordered.

### 3. Mark-impact cascade (T038)

`LiquidationEngine::onMark` recomputes the mark from the post-fill
book mid and recurses. The recursion bound `_maxCascadeDepth` is
configurable; mark recomputation is a pure function of
`(tape_mark, book_mid, model, weight)`.

**Resolution:** deterministic. Identical book state → identical
recomputed mark → identical next-round behaviour.

### 4. Cross-account ADL candidate pool (T055)

`runInsuranceAndAdlPhase` builds a `std::vector<AdlCandidate>`
from orphan `_positions` followed by every attached account's
positions in attach order. Ranking is `std::sort` by score
descending — stable.

**Resolution:** deterministic. The
`std::unordered_map<Account*, std::vector<size_t>>` used to track
per-account close indices does NOT affect ordering — it only
batches erase-descending per account, and each account's per-erase
order is determined by `std::sort` over its own indices.

### 5. Funding settlement timestamps (T031, T047)

`FundingSchedule::tick` walks settlement boundaries in
`(lastTickNs, nowNs]` deterministically.

**Resolution:** deterministic. No RNG; pure timer arithmetic.

### 6. Rate-limit policy (T022, T049)

`RateLimitPolicy::tryConsume` checks bucket capacities against a
deterministic clock. Ban state is recorded with the timestamp of
the triggering action.

**Resolution:** deterministic given identical input action stream.

### 7. Venue-availability outage policy (T023, T046)

`VenueAvailability` supports scheduled + random outages. Random
mode uses a seeded RNG; scheduled mode is purely table-driven.

**Resolution:** deterministic when seed is set; non-deterministic
in `auto_random_outages` mode unless the seed is fixed (caller's
responsibility).

### 8. 30-day rolling notional eviction (T037, T059)

`Account::evictExpired` and `FeeSchedule::evictExpired` evict
fills with `tsNs <= cutoff`. Comparison is `<=` (deterministic
boundary behaviour); fills are stored in a `std::deque` insertion-
ordered.

**Resolution:** deterministic.

## Regression test

`tests/test_w15_reproducibility.cpp` (gated under
`FLOX_ENABLE_BACKTEST`) runs every VenueStack twice with identical
inputs and asserts every captured engine stat is bit-identical.
Scenarios:

- `HealthyWalkBitIdenticalAcrossRuns` — multi-symbol calm walk;
  100 ticks; no liquidations.
- `CascadeScenarioBitIdenticalAcrossRuns` — underwater account
  with opposing-side counterparty; cascade triggers liquidation +
  insurance + ADL.
- `MultipleVenuesEachReproducible` — same check across all four
  venue factories.

If the test fails, the failure surface narrows the regression to a
specific stat (`liquidations_count`, `insurance_fund_balance`,
etc.) which points at the responsible subsystem.

## Caveats

- The static `g_liquidationOrderIdCounter` in
  `liquidation_engine.cpp` is process-global. Within one process,
  consecutive runs see different liquidation order IDs. This does
  NOT affect the captured snapshot (we record stats, not order
  IDs), but it would matter for cross-process determinism. File
  a follow-up if needed.

- `VenueAvailability::auto_random_outages` is non-deterministic
  unless seeded. The default venue stacks do not enable random
  outages; researchers who do should set the seed explicitly.

- Floating-point operations use IEEE 754; results are deterministic
  on a single ISA / compiler. Different compilers or vector-width
  flags can produce different rounding. Builds on the same host
  configuration are guaranteed reproducible.
