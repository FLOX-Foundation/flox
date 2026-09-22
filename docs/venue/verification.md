# Verification

A venue moves real money, so on top of the unit suites the module is checked
against invariants over random command streams. The invariants are layered:
each one targets a failure the ones above it let through.

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

## Golden replay, on both books

`test_venue_golden_replay` is the opposite kind of check from the differential
fuzz above: a fixed corpus of ~25 hand-written scenarios (spot, perp, last
look, STP, auctions, session transitions, checkpoint mid-stream, quote
ladders), not a random stream, each pinned to a recorded `stateHash`,
`configHash` and event-stream hash in `venue/tests/golden/replay_hashes.txt`.
It exists to catch a refactor that changes behaviour nobody wrote a narrower
test for.

The corpus is templated on the resting-book implementation
(`corpus<Book>()`), and `CorpusMatchesTheTable` runs it twice: once on
`MatchingEngine<MatchingBook>`, whose numbers are what the table records and
what `FLOX_UPDATE_GOLDEN=1` rewrites, and once on `MatchingEngine<LadderBook>`,
checked against the SAME table with no separate `replay_hashes_ladder.txt`.
That is a contract, not a convenience: `stateHash`/`configHash`
(`venue/include/flox-venue/engine/checkpoint.inl`) fold in only `SymbolConfig`
and the book's content through `Book::forEachOrder`'s canonical traversal
(price levels best-first, FIFO within each level) -- never a book's own
internal representation -- so for the same config and the same command
stream the two engines are required to land on byte-identical numbers,
provided the `LadderBook` instance is sized wide and deep enough to never
silently drop an order (see `makeBook<Book>()` in the test file). One table
is therefore the strongest available check: a second table could only ever
record the same numbers or a real divergence, and a real divergence is
exactly the failure this test exists to report.

This is what closes the coverage gap the differential fuzz's random stream
does not reach reliably: `LadderBook`'s two real-fill mutation points
(`fillBest`, `consumeById`) are exercised by unit tests, but before this,
nothing outside the differential fuzz's random 200k-command mix ran them
through the SAME hand-picked scenarios (an auction uncross's partial fill,
an iceberg refill, a pro-rata allocation) that the `MatchingBook` pass
already pins. A mutation at either point now reddens the `LadderBook` half
of `CorpusMatchesTheTable` while the `MatchingBook` half and every other
test stay green -- confirmed by mutating each of `fillBest` and
`consumeById` in turn.

## Conservation fuzz

`test_venue_conservation_fuzz` asserts money properties over a random stream
across six scenarios: spot, spot with last look, perp, perp with ADL,
cross-margin, and multi-asset collateral.

1. **Conservation of total.** Each asset's total across accounts plus the venue
   account never changes.
2. **The reserved invariant.** After the book is drained, every account's
   `reserved` must be zero.
3. **The position cap binds the result.** No account is ever carried past
   `maxPositionQty` by fills, however many orders built the position.

Features are fuzzed together, not one at a time: the last-look scenario also
runs self-trade prevention (a maker can be pulled from inside the matcher with a
hold open), and the perp scenario submits market, stop and reduce-only orders,
not limits only -- a perp market order is margined against the price band, and a
reduce-only order is re-measured when it fills.

## What each property catches

What each property catches, and the failure it rules out:

| Property | Catches | Concrete failure it rules out |
|---|---|---|
| conservation of total | money created or destroyed in aggregate | ADL crediting a winner while the deficit vanishes |
| reserved invariant (drain, then `reserved == 0`) | buying power frozen: total is fine, the funds are stuck | a reservation surviving an IOC/FOK expiry or a reject |
| per-order reservation consistency | one order over-releasing, masked by other orders in the same account's aggregate | an iceberg modify releasing against the displayed peak rather than the full size |
| per-account equity + insurance payout | mis-socialization: right total, wrong recipient | insurance paying an account that is solvent on total equity |
| insertion-order independence | a replica making a different choice from the same logical state | an ADL victim selected by hash iteration order |
| made-whole on non-fill | funds frozen when a fill does not happen | a last-look reject stranding both sides' reservations |
| positive-obligation netting | a debit balance masking a real custody shortfall | segregation reporting a short custody balance as fully backed |
| feature interaction in one stream | a hazard that exists only where two features meet | STP pulling a last-look maker and freeing the collateral its open hold must settle from |
| position cap over the result | limits enforced on the order instead of on the position | orders each under `maxPositionQty` settling into a position past it |

The fuzz runs features together rather than in isolation because these
failures depend on each other. Funding can drive a wallet negative while an
open position keeps the account alive, and that state is the precondition for
the ADL and segregation failures above. A stream exercising one feature at a
time never reaches it.

## Static checks

Two checks run against the code rather than against test cases, so they cover
paths nobody wrote a test for.

**Gate reachability.** `scripts/check_gate_reachability.py` reads the clang AST
and checks two things for each handler that reaches the matcher: on every path
from the handler's entry to the call, each gate it owes has already run
(branches that return, continue, break or throw are excluded), and the gate's
result is acted on rather than discarded. The set of functions calling into
matching is pinned, so a new path fails the check until it declares its gates.
It reads `compile_commands.json` and needs a configure, not a build.

**Recovery model.** `scripts/check_recovery_model.py` enumerates every
reachable state of a bounded model of the checkpoint protocol -- checkpoint,
publish failure, snapshot corruption and pruning, in every interleaving -- and
checks that recovery either reconstructs history back to zero or refuses to
start. Crash points between file operations are hard to cover with tests and
cheap to cover by enumeration. Run it with `--unguarded` to see the protocol
without the exhaustion guard, and the four-step counterexample the guard
removes.

## Minimised counterexamples

A divergence reported at command 47,213 is hard to act on. The command stream
is deterministic and replayable, so on failure the harness binary-searches the
shortest failing prefix and then removes chunks until no single command can be
dropped (`venue/tests/support/counterexample.h`). It runs only after a failure,
and it has its own test against a synthetic predicate with a known two-command
witness.

## Sanitizers

`venue/scripts/run_sanitizers.sh` builds and runs:

- **ASAN + UBSAN** over the parsers (hostile network input), both fuzzes, the
  venue harnesses, and the recovery/journal paths. UBSAN halts on the first
  diagnostic. That matters for the arithmetic: band and margin computations
  multiply fixed-point raws, and a high-priced instrument can push an
  intermediate past 64 bits with no visible symptom.
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
periodically at full depth. A top-of-book comparison would miss an iceberg's
hidden reserve leaking into the public feed, or a level going stale below the
touch.
