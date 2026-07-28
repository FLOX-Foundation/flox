# Verification

A venue moves real money, so on top of the unit suites the module is checked
against invariants over random command streams. Each invariant below was added
because it caught a class of defect the previous one missed.

Run it all:

```bash
cd build && ctest                    # unit suites + fuzzes
../venue/scripts/run_sanitizers.sh   # ASAN/UBSAN over parsers and fuzzes, TSAN over concurrency
```

## Differential fuzz

`test_venue_differential_fuzz` drives the map-reference `MatchingBook` and the
O(1) `LadderBook` in lockstep over a random stream of limits, markets, IOC,
post-only, cancels, modifies and pegs. After every command:

- both engines must have emitted an identical event stream (compared as a
  rolling hash), and
- neither book may be crossed.

The reference book is the oracle: any ladder divergence fails with the command
index. Depth is CI-friendly by default; set `FLOX_FUZZ_OPS` for a deep run
(2M commands verified).

## Conservation fuzz

`test_venue_conservation_fuzz` asserts money properties over a random stream
across five scenarios: spot, perp, perp with ADL, cross-margin, and multi-asset
collateral.

1. **Conservation of total.** Each asset's total across accounts plus the venue
   account never changes.
2. **The reserved invariant.** After the book is drained, every account's
   `reserved` must be zero.

## What each property catches

Each of these defects passed the check above it in the table, and each round of
hardening added the property that would have caught it:

| Property | Catches | Example defect it caught |
|---|---|---|
| conservation of total | money created or destroyed in aggregate | ADL crediting a winner while the deficit vanished |
| reserved invariant (drain, then `reserved == 0`) | buying power frozen: total is fine, the funds are stuck | residual reservations kept on IOC/FOK/reject paths |
| per-order reservation consistency | one order over-releasing, masked by other orders in the same account's aggregate | iceberg modify releasing against the displayed peak instead of the full size |
| per-account equity + insurance payout | mis-socialization: right total, wrong recipient | insurance paying out to an account that was solvent on total equity |
| insertion-order independence | a replica making a different choice from the same logical state | ADL victim chosen by hash iteration order |
| made-whole on non-fill | funds frozen when a fill does not happen | last-look reject stranding both sides' reservations |
| positive-obligation netting | a debit balance masking a real custody shortfall | segregation reporting a short custody balance as fully backed |

Several of these compound: funding driving a wallet negative while a position
kept the account alive made states reachable that the ADL and segregation
defects then exploited. Finding one exposed the next.

## Sanitizers

`venue/scripts/run_sanitizers.sh` builds and runs:

- **ASAN + UBSAN** over the parsers (hostile network input), both fuzzes, the
  venue harnesses, and the recovery/journal paths. UBSAN halts on the first
  diagnostic; it caught a signed overflow in the LULD band computation on a
  high-priced instrument.
- **TSAN** over the sequenced single-writer core and the multi-threaded
  gateways.

CI also runs the whole project, venue included, under all three sanitizers.

## Determinism gates

- `test_venue_engine` replays a journal into a fresh engine and requires an
  identical event-stream hash and ledger. `test_venue_venue` does the same for
  a stream containing an `AdminCmd` auction uncross, which is why auctions are
  sequenced commands.
- Tie-breaks are asserted directly: equal-score ADL candidates must resolve to
  the same victim under a reversed insertion order, and mass-cancel must emit
  in ascending order id regardless of insertion order.

## Book agreement

Market-data depth is compared against the engine's book over 300k commands,
periodically at full depth. The full-depth comparison is what surfaced an
iceberg's hidden reserve leaking into the public feed on a partial peak fill;
top-of-book checks had passed.
