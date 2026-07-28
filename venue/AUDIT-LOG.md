# Venue module — Audit & Fix Log

A durable catalogue of correctness defects found and fixed while hardening the
spot + derivatives venue (now the flox `venue/` module), with the regression test that locks each one. Every
fix listed here ships with a test proven to **fail before the fix and pass
after** (negative proof), and the whole set is green in flox CI and under
`venue/scripts/run_sanitizers.sh` (ASAN/UBSAN/TSAN).

Method: independent adversarial audits, one subsystem at a time. Each finding
was verified against the real code before fixing; anything that could not be
fixed safely with a catching test was **documented as a follow-up (below), not
shipped blind**.

## Verification-theory progression

Conservation-of-total is necessary but **not sufficient**. Each round exposed a
new invariant the previous ones missed:

1. total conservation → catches money created/destroyed in aggregate
2. **reserved-invariant** (drain book → every `reserved == 0`) → catches
   reservation leaks invisible to total conservation
3. **per-order** reservation consistency → catches an over-release masked by
   other orders' reservations in the same account's aggregate
4. **per-account equity + insurance-payout** → catches mis-socialization
   (right total, wrong recipient — e.g. ADL phantom credit)
5. **insertion-order independence** → catches determinism/HA forks (unordered
   iteration feeding an emitted event or a state-affecting decision)
6. **made-whole on non-fill** → catches frozen funds (last-look reject)
7. **compliance netting** → catches a debit balance masking a custody shortfall

Several defects **compounded**: funding driving `available` negative while a
position kept an account unliquidated (#14) made the states reachable that #20
(ADL phantom mint) and #22 (segregation netting) then exploited — one finding
opened the next.

## Fixes (each with its regression test)

| # | Class | Defect | Test |
|---|-------|--------|------|
| 1 | reservation | iceberg modify-shrink released against the displayed peak, not the full size → over-release + phantom resting liquidity | `test_iceberg_modify_shrink` |
| 2 | liquidation | force-close didn't cancel the account's other resting orders first → insurance paid out to a total-equity-solvent account | `test_perp_liquidation_frees_resting_collateral` |
| 3 | funding | funding to `available` couldn't trigger liquidation (equity check ignored wallet) → silent bad debt; added wallet-drag term | `test_funding_triggers_liquidation` |
| 4 | MD fidelity | iceberg hidden reserve leaked to the public feed on a partial-peak fill (`OrderExecuted` now carries `displayLeaves`) | `test_iceberg_hidden_from_md` |
| 5 | determinism/HA | ADL victim chosen by `unordered_map` layout on equal uPnl → replica event-hash fork; added total-order tie-break | `test_cross_margin_adl_deterministic_victim` |
| 6 | hostile-input | WS 64-bit extended length overflowed the guard → `resize(~2^64)` → process crash; + 4 GiB amplification on binary/TLS prefix; caps + overflow-safe check + connLoop try/catch | `test_websocket` (overflow/oversized/partial + hostile u32) |
| 7 | authorization | session never bound the account → any client could act as any account by writing a different id; `handle()` now stamps the session account | `test_session_account_binding` |
| 8 | arithmetic-UB | LULD band `raw * bps` int64 multiply overflowed (UBSAN) → garbage band; 128-bit + clamp | `test_luld` (high-price/wide-band) |
| 9 | money-creation | ADL claw-back credited the venue the full haircut while the all-or-nothing debit no-op'd on a negative-wallet winner → minted money | `test_cross_margin_adl_negative_available_conserves` |
| 10 | order-lifecycle | FOK rested as a GTC limit when STP removed the self-liquidity its precheck counted on; STP-aware precheck + residual-kill | `test_matcher` (FOK+STP) |
| 11 | compliance | segregation netted debit client balances → a real custody shortfall reported as fully-backed; sum of positive obligations | `test_segregation_no_debit_netting` |
| 12 | map-reuse | OCO `ocoMembers_` not cleaned on cancel/expiry/reject → a reused OrderId wrongly canceled + unbounded leak; `unlinkOco` + scope-guard | `test_matcher` (OCO cancel/reject + id reuse) |
| 13 | passive-mispricing | peg repriced off a book that still included its own order → creep to the opposite touch each submit; cancel-before-target | `test_matcher` (Mid-peg-no-creep) |
| 14 | risk-gate-bypass | triggered stop re-entered outside onNew → reduce-only stop opened an uncollateralized position; `perpRiskGate` on the trigger path | `test_perp_reduce_only_stop_cannot_open` |
| 15 | frozen-funds | last-look reject/timeout emitted only `FillRejected` → both parties' reservations stranded forever; `releaseHeldLeg` | `test_last_look_reject_releases_reservation` |
| 16 | risk-gate-bypass | perp modify re-enter skipped `perpRiskGate` → size-increase grew position past the cap | `test_perp_modify_respects_position_cap` |
| 17 | safety-flag | reduce-only dropped across a perp modify → could flip a position; `RestingOrder.reduceOnly` carried + re-capped | `test_perp_modify_preserves_reduce_only` |
| 18 | recovery-completeness | auction uncross fills + emergency cancel-all bypassed the journal → replay divergence; sequenced `AdminCmd` | `test_auction_recovery` |
| 19 | layout-independence | repeg/expire/cancel-all iterated `unordered_*` without the ADL sort standard (repeg is state-affecting) → cross-build HA fork; sort collected ids | `test_matcher` (mass-cancel ascending order) |

Earlier in the session (pre-audit hardening), the same discipline fixed: an
integer-overflow UB on hostile numeric JSON input; `reduceOnly` silently dropped
on the wire in OUCH/FIX/REST (a reduce-only order would have opened a position);
FIX CheckSum not validated; missing `LastPx` in exec reports; derivatives
recovery divergence (mark/funding promoted to journaled commands); and four
distinct reservation leaks on the residual-cancel / reject / stop-trigger paths.

## Known limitations (documented, deliberately NOT rushed)

These are real gaps whose correct fix is a cohesive design change in a
critical/concurrent path. Rushing them risks a worse defect than they cure, so
they are recorded here and in `README.md` as follow-ups rather than shipped
blind:

- **fill-time reduce-only re-check**: reduce-only is capped at submit / trigger /
  modify against the position at that instant, but not re-checked at each *fill*.
  If the position shrinks while a reduce-only order rests (or multiple reduce-only
  orders sum beyond the position), the remainder can over-reduce. Correct fix:
  cohesive per-account aggregate reduce-only accounting re-checked at fill time
  (position-aware matching).
- **async outbound bus + backpressure**: exec reports are delivered
  fire-and-forget via a synchronous `writeAll` on the `submit()` path. A slow
  client head-of-line-blocks the engine; a failed write silently drops a fill
  (client diverges) with no recovery. `ResendBuffer` exists but is unwired.
  Correct fix: per-connection async queue + wire `ResendBuffer` + teardown on
  write failure.
- **hardware clock in the live sequencer**: `now_` is a logical counter, so
  ns-scale timed features (`Journal::loadTimed`, and volume-tiered fees'
  30-day rolling window) need a real `steady_clock` feed.
- **volume-tiered fees**: single shared `FeeSchedule`, `recordFill` uncalled →
  flat (tier-0) only. Per-account VIP tiers need `bindAccount` + the real clock.
- **HMAC logon replay-nonce**: logon checks signature + ±5s skew but no
  single-use nonce → a captured logon is replayable within the window (needs a
  shared, time-evicted nonce store).
- **control-plane auth / value validation**: admin ops have no authentication
  and don't validate tick/lot/band values (assumed to run on an internal admin
  interface).

## Running the gate

```bash
cmake -S . -B build -DFLOX_BUILD_TESTS=ON
cmake --build build
cd build && ctest
../venue/scripts/run_sanitizers.sh
```
