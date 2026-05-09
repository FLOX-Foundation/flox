# The replay-equivalence CI gate

flox positions itself on three legs: deterministic backtest ↔ live replay equivalence, AI-controllable via MCP, polyglot strategy authoring. The first leg is the easiest to claim and the hardest to keep. Every engine release risks a subtle drift that makes a replayed backtest produce different fills than it did yesterday. The replay-equivalence CI gate is what turns that claim into a check that fails before merge.

## What it does

Every push runs `scripts/replay_equivalence_gate.py`:

1. Generate a small deterministic tape from a frozen sequence of synthetic trades (`tests/replay-equivalence/build_tape.py`).
2. Run a frozen reference strategy (`tests/replay-equivalence/strategy.py`) through that tape against `flox_py.SimulatedExecutor`.
3. Compare the captured output (`trade_count`, `fill_count`, the fill sequence, `total_filled_quantity`) byte-for-byte with the frozen `tests/replay-equivalence/expected_output.json`.

Match → exit 0. Divergence → exit 1, with a per-key diff printed to stderr. The gate is wired into the `verify-docs-current` CI job, so divergence blocks the rest of the build matrix.

## What divergence catches

- Engine event-ordering changes that shift fill prices.
- Slippage model regressions.
- Queue-tracking math drift.
- C++/Python type round-trip changes that perturb fixed-point conversion.
- Unintentional behavior changes in `SimulatedExecutor` order matching.

## What divergence does not catch

The gate is intentionally small. It does not exercise:

- Stop / take-profit / trailing variants. Those are no-ops in `SimulatedExecutor.submit_order` today.
- Multi-strategy composition.
- Live ↔ backtest equivalence proper. A separate phase tracks that work: a captured live tape replayed through the engine, comparing against the live-side fill log.

When those land, this gate gets new fixtures alongside the existing one.

## Regenerating the expected output

When an intentional engine change shifts fills (a slippage formula refinement, a queue-tracker fix, a deliberate change to which trade triggers a buy), regenerate the frozen output:

```bash
python3 scripts/replay_equivalence_gate.py --regen
```

Commit the resulting `tests/replay-equivalence/expected_output.json` diff alongside the engine change. The PR review should explain what shifted and why; reviewers should reject regenerations that are not justified.

The fixtures (`strategy.py`, `build_tape.py`, `expected_output.json`) live together so that "what is the gate testing" is one directory listing away from "what does the gate expect".

## Why a synthetic tape

A captured live tape would be more honest, but it would also be larger, harder to commit deterministically, and tied to one exchange's micro-format. The synthetic tape is small (7 trades), fully reproducible from `build_tape.py`'s constants, and runs in a few hundred milliseconds. It catches the regressions the gate exists to catch.

A separate longer-tape integration test belongs in a slower job that runs on the release pipeline rather than every PR. That is the natural Phase 2 follow-up.

## See also

* [Reproducibility bundles](../how-to/reproducibility-bundles.md). The CLI form of what the gate runs end-to-end.
* [Paper trading](../how-to/paper-trading.md). Same `SimulatedExecutor`, same fill model.
* [Record and replay tapes](../how-to/tape-record.md). The on-disk format the gate's tape uses.
