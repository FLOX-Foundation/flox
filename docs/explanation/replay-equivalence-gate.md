# The replay-equivalence CI gate

flox positions itself on three legs: deterministic backtest ↔ live replay equivalence, AI-controllable via MCP, polyglot strategy authoring. The first leg is the easiest to claim and the hardest to keep. Every engine release risks a subtle drift that makes a replayed backtest produce different fills than it did yesterday. The replay-equivalence CI gate is what turns that claim into a check that fails before merge.

## What it does

Every push runs `scripts/replay_equivalence_gate.py`:

1. Generate a small deterministic tape from a frozen sequence of synthetic trades (`tests/replay-equivalence/build_tape.py`).
2. Run every frozen scenario under `tests/replay-equivalence/scenarios/` through that tape against `flox_py.SimulatedExecutor`. Each scenario is a directory holding a `strategy.py` and its frozen `expected_output.json`.
3. Compare each captured output (`trade_count`, `fill_count`, the fill sequence, `total_filled_quantity`) field-by-field with the scenario's frozen expectation.

All scenarios match → exit 0. Any divergence → exit 1, with a per-field diff per scenario printed to stderr (nested structures produce paths like `fills[0].price: actual=100.0 expected=999.0`). The gate runs as a step of the `linux-gcc` CI job; divergence fails that job and blocks merge.

## Scenarios

| Scenario | Mechanics pinned |
|---|---|
| `market` | market entry on the first trade; the plain strategy → simulator → fill round trip |
| `stop_loss` | entry + protective SELL `stop_market` armed mid-tape; trigger evaluation (`price <= trigger`) and triggered-market conversion |
| `take_profit` | entry + SELL `take_profit_market`; the tape prints 101.25 against a 101.30 trigger before firing at 101.50, pinning the trigger boundary |
| `trailing_stop` | entry + SELL `trailing_stop` with a fixed 0.30 offset; the trigger ratchets behind the rally and fires on the pullback |

`stop_loss` and `trailing_stop` freeze the same fill sequence through different mechanics — a regression in ratchet math surfaces in one without the other.

## What divergence catches

- Engine event-ordering changes that shift fill prices.
- Slippage model regressions.
- Queue-tracking math drift.
- Conditional-order regressions: trigger comparison direction, trigger-price plumbing through the signal path, trailing-stop ratchet math, triggered-order conversion to market.
- C++/Python type round-trip changes that perturb fixed-point conversion.
- Unintentional behavior changes in `SimulatedExecutor` order matching.

## What divergence does not catch

The gate is intentionally small. It does not exercise:

- Resting-limit fills. Resting limits fill against book liquidity, not the trade feed, and the gate's tape is trades-only; a book-driven scenario needs a tape format that carries snapshots.
- Multi-strategy composition.
- Live ↔ backtest equivalence proper. A separate phase tracks that work: a captured live tape replayed through the engine, comparing against the live-side fill log.

When those land, this gate gets new fixtures alongside the existing ones.

## Running the gate locally

```bash
python3 scripts/replay_equivalence_gate.py
```

The script adds the built bindings to `sys.path` itself, preferring a build directory whose compiled extension matches the running interpreter (`build/python`, then `build-py312/python`). The interpreter you run the script with must be the one the bindings were built for: a `.so` built for a different Python version is invisible to the import system. `flox_py` still imports in that state (pure-Python surfaces such as `flox_py.cli` keep working), but the first access to a native name raises an ImportError pointing at the version mismatch. Rebuild the bindings with your interpreter or run the script with the matching one, e.g.:

```bash
.venv312/bin/python scripts/replay_equivalence_gate.py
```

## Regenerating the expected output

When an intentional engine change shifts fills (a slippage formula refinement, a queue-tracker fix, a deliberate change to which trade triggers a buy), regenerate the frozen output:

```bash
python3 scripts/replay_equivalence_gate.py --regen
```

Commit the resulting `expected_output.json` diffs alongside the engine change. The PR review should explain what shifted and why; reviewers should reject regenerations that are not justified. `--regen` rewrites every scenario, so an engine change that legitimately shifts one scenario and silently shifts another shows up as two diffs — both need explaining.

The fixtures (each scenario's `strategy.py` + `expected_output.json`, plus the shared `build_tape.py`) live together so that "what is the gate testing" is one directory listing away from "what does the gate expect".

## Why a synthetic tape

A captured live tape would be more honest, but it would also be larger, harder to commit deterministically, and tied to one exchange's micro-format. The synthetic tape is small (7 trades), fully reproducible from `build_tape.py`'s constants, and runs in a few hundred milliseconds. It catches the regressions the gate exists to catch.

A separate longer-tape integration test belongs in a slower job that runs on the release pipeline rather than every PR. That is the natural Phase 2 follow-up.

## See also

* [Reproducibility bundles](../how-to/reproducibility-bundles.md). The CLI form of what the gate runs end-to-end.
* [Paper trading](../how-to/paper-trading.md). Same `SimulatedExecutor`, same fill model.
* [Record and replay tapes](../how-to/tape-record.md). The on-disk format the gate's tape uses.
