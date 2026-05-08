# Hot-reload a strategy without dropping connections

Strategy logic changes more often than the engine does. A typical session: the engine boots, opens a WebSocket to the exchange, subscribes to a few symbols, and starts dispatching trades to a strategy. Half an hour in you want to tweak a parameter or swap to a different model. The naive path is to stop the engine, edit, restart. That tears down the WebSocket, drops in-flight orders, and depending on the exchange may take 30 seconds to come back.

`Runner.replace_strategy(index, new_strategy)` does the swap atomically. The bus subscriptions, the executor, the connector session, the in-flight orders all stay intact. The old strategy gets `on_stop`. The new strategy gets `on_start`. The next dispatched event lands on the new strategy.

## Lifecycle

The bridge between the engine and your strategy holds the subscriptions. When you replace the strategy, the engine swaps the user-facing object pointer atomically and fires the lifecycle callbacks in this order:

1. Old strategy `on_stop` (final chance to flush state).
2. Pointer swap. The next `on_trade` / `on_book` / `on_bar` will land on the new strategy.
3. New strategy `on_start` (typical place to seed indicators from a stored snapshot).

In-flight events that started dispatching before the swap finish on whichever strategy was current when they began. There is no torn state: a dispatch sees either the old or the new strategy, never a partially constructed mix.

## State carry-over

The engine does not migrate state for you. If your new strategy needs to pick up where the old one left off, persist what you need in `on_stop` and reload in `on_start`:

=== "Python"

    ```python
    import json
    from pathlib import Path

    SNAPSHOT = Path("/tmp/strategy_state.json")

    class EmaStrategy(flox_py.Strategy):
        def on_stop(self):
            SNAPSHOT.write_text(json.dumps({"prices": list(self.prices)}))

        def on_start(self):
            if SNAPSHOT.exists():
                self.prices = list(json.loads(SNAPSHOT.read_text())["prices"])

    runner.replace_strategy(0, EmaStrategy([sym], fast=12, slow=26))
    ```

=== "Node.js"

    ```javascript
    const fs = require('fs');
    const SNAPSHOT = '/tmp/strategy_state.json';

    function makeStrategy(label) {
      const s = { symbols: [sym], prices: [] };
      s.onStart = () => {
        if (fs.existsSync(SNAPSHOT)) {
          s.prices = JSON.parse(fs.readFileSync(SNAPSHOT)).prices;
        }
      };
      s.onStop = () => {
        fs.writeFileSync(SNAPSHOT, JSON.stringify({ prices: s.prices }));
      };
      s.onTrade = (_ctx, t) => { s.prices.push(t.price); };
      return s;
    }

    runner.replaceStrategy(0, makeStrategy('v2'));
    ```

The shape of the snapshot is your call. Pickle, JSON, msgpack, a binary buffer, whatever fits. The engine is not opinionated; it just guarantees that `on_stop` finishes before the swap is observable, and that `on_start` runs before the new strategy sees its first event.

## What it does not do

- **Replace the binding.** Codon is AOT-compiled; once a Codon strategy is built into the binary, swapping its logic at runtime would require recompiling and restarting. QuickJS strategies are registered through `flox.register()` at script load; the runner does not currently expose a re-register hook.
- **Migrate orders.** Open positions and in-flight orders belong to the executor, not the strategy. They survive the swap automatically because the engine never touches them.
- **Replay events.** If the new strategy needs to "see" the last hour of trades to warm up its indicators, run a brief replay through it before swapping it in.
- **Validate symbol overlap.** The new strategy is expected to watch the same symbols as the old one. If you need to change the symbol set, tear down and recreate the strategy slot.

## When to reach for it

- A parameter sweep where each iteration runs against live trades for a few minutes.
- An A/B test between two model variants, kept symmetrical by sharing the connector session.
- A bug fix shipped without a maintenance window.

## When to skip it

- Cold starts. If you can afford a 30-second drop and you already have `on_start` warmup logic, restarting is simpler.
- Symbol-set changes. Add or remove a strategy slot instead.
- Codon / QuickJS. Use the binding model that fits the runtime.

## See also

- [Strategy classes](strategy-classes.md). The cross-language Strategy model the swap operates on.
- [Reproducibility bundles](reproducibility-bundles.md). Pin the swap point with a tape so you can replay the before / after deterministically.
