# Model liquidation cascades, insurance fund, and ADL

A real perpetual venue has a three-step liquidation pipeline:

1. **Liquidation engine** takes over the underwater position at the
   bankruptcy price and walks the book to close it.
2. If the close generates a loss the trader's posted equity cannot
   cover, the **insurance fund** absorbs the deficit.
3. If the fund is depleted, **ADL** (auto-deleveraging) activates:
   profitable opposite-side traders are force-closed at the
   bankruptcy price, ranked by their PnL ratio.

flox's default backtest treats liquidation as a hard stop — the
position disappears at the maintenance-margin-breach price. For
research that needs to know whether a portfolio survives a 5%
adverse move vs cascades INTO a -8% liquidation, that's a
comforting fiction. `LiquidationEngine` models the full pipeline.

## Configure

A worked example (Python):

```python
--8<-- "examples/python_liquidation_engine.py"
```

=== "Python"

    ```python
    import flox_py as flox

    eng = flox.LiquidationEngine()
    eng.add_tier(min_notional=0,         mm_fraction=0.005)
    eng.add_tier(min_notional=250_000,   mm_fraction=0.01)
    eng.add_tier(min_notional=1_000_000, mm_fraction=0.025)
    eng.set_insurance_fund_capital(10_000_000)
    eng.set_adl_enabled(True)

    # Open a 5 BTC long at 50k, equity 1000 USDT.
    eng.open_position(account_id=42, symbol=1, quantity=5.0,
                       entry_price=50_000.0, equity=1000.0)

    # On each tick, feed the new mark price.
    outcome = eng.on_mark(symbol=1, mark_price=49_500.0)
    if outcome["liquidated"]:
        print("liquidated:", outcome["liquidated"])
    ```

=== "TypeScript (Node)"

    ```typescript
    import { LiquidationEngine } from "flox";

    const eng = new LiquidationEngine();
    eng.addTier(0, 0.005);
    eng.addTier(250_000, 0.01);
    eng.setInsuranceFundCapital(10_000_000);
    eng.setAdlEnabled(true);

    eng.openPosition(42, 1, 5.0, 50_000.0, 1000.0);
    const liquidated = eng.onMark(1, 49_500.0);
    ```

=== "TypeScript (QuickJS)"

    ```typescript
    const eng = __flox_liquidation_engine_create();
    __flox_liquidation_engine_add_tier(eng, 0, 0.005);
    __flox_liquidation_engine_set_insurance_fund_capital(eng, 10_000_000);
    __flox_liquidation_engine_open_position(eng, 42, 1, 5.0, 50000.0, 1000.0);
    const n = __flox_liquidation_engine_on_mark(eng, 1, 49500.0);
    ```

=== "Codon"

    ```python
    from flox.backtest import LiquidationEngine

    eng = LiquidationEngine()
    eng.add_tier(0.0, 0.005)
    eng.set_insurance_fund_capital(10_000_000.0)
    eng.open_position(42, 1, 5.0, 50000.0, 1000.0)
    n = eng.on_mark(1, 49500.0)
    ```

=== "C"

    ```c
    FloxLiquidationEngineHandle eng = flox_liquidation_engine_create();
    flox_liquidation_engine_add_tier(eng, 0.0, 0.005);
    flox_liquidation_engine_set_insurance_fund_capital(eng, 10000000.0);
    flox_liquidation_engine_open_position(eng, 42, 1, 5.0, 50000.0, 1000.0);
    uint32_t n = flox_liquidation_engine_on_mark(eng, 1, 49500.0);
    ```

## Canned profiles

| Profile               | Tiers | Insurance fund cap | Slippage |
|-----------------------|-------|--------------------|----------|
| `binance_um_futures`  | 6     | 900M USDT          | 15 bps   |
| `bybit_linear`        | 4     | 100M USDT          | 20 bps   |
| `okx_swap`            | 3     | 150M USDT          | 20 bps   |

Numbers approximate the published values; tune for your engagement.

## What the engine does on each tick

For every open position on the symbol being marked:

1. Compute notional = `|quantity| * mark_price` and resolve the
   maintenance-margin rate from the tier ladder.
2. If `equity + unrealized_PnL < notional * mm_fraction`, the
   position is liquidated at `mark * (1 - sign(qty) * slippage_bps/10000)`.
3. Compute realized loss vs entry; deficit beyond posted equity
   accumulates.
4. Insurance fund pays as much of the total deficit as it can.
5. If `adl_enabled` and deficit remains, rank profitable
   opposite-side positions by the configured **ADL ranking strategy**
   (default PnL ratio) and force-close from the top until the
   deficit is absorbed.

## ADL ranking strategies

Real venues compute the ADL queue with different formulas; pick the
one that matches the venue being modeled. Each canned profile sets
the strategy its venue actually uses.

| Strategy        | Formula                            | Used by                |
|-----------------|------------------------------------|------------------------|
| `pnl_ratio`     | `upnl / equity`                    | default; OKX preset    |
| `binance`       | `upnl × leverage` (lev = notional/equity) | Binance UM preset |
| `bybit`         | same as `binance` (alias)          | Bybit linear preset    |
| `position_size` | `|quantity|`                       | small DEX perps        |

Higher score = closer to the front of the ADL queue. Set via
`set_adl_ranking(...)` with a string name or the `AdlRanking` enum:

=== "Python"

    ```python
    liq.set_adl_ranking("binance")
    # or
    liq.set_adl_ranking(flox_py.AdlRanking.PositionSize)
    ```

=== "TypeScript (Node)"

    ```typescript
    liq.setAdlRanking("position_size");
    // or numeric: 0=pnl_ratio, 1=binance, 2=bybit, 3=position_size
    ```

Cumulative counters track liquidations / insurance payments / ADL
closeouts across the engine's lifetime.

## Cascade modelling

To reproduce a flash-crash cascade:

1. Open a portfolio of leveraged positions at various entry prices
   and equity levels.
2. Step the mark price down (or up) in small increments, calling
   `on_mark` each step.
3. Watch the cumulative counters: each tick that liquidates
   positions feeds back into the next as the insurance fund drains
   and ADL activates.

## Integrate with the order book

By default the engine closes underwater positions at a flat-bps
slippage from the mark — fine for portfolio-level research, but
the close price ignores the actual book depth.

Attach a `SimulatedExecutor` to route liquidation orders through
the matching engine. The liquidation becomes a real market order
that consumes book liquidity, pays venue fees, and samples
configured latency:

=== "Python"

    ```python
    exec = flox.SimulatedExecutor()
    # ... set up book updates, fees, latency on exec ...
    liq = flox.LiquidationEngine.binance_um_futures()
    liq.set_executor(exec)  # detach: pass None
    ```

=== "TypeScript (Node)"

    ```typescript
    const exec = new SimulatedExecutor();
    const liq = new LiquidationEngine();
    liq.loadProfile("binance_um_futures");
    liq.setExecutor(exec);
    ```

With an executor attached, the close price reflects the
post-walk-the-book average instead of a flat-bps haircut. When
the book is too thin to fill the entire position in one tick, the
remainder stays on the engine's books for the next `on_mark` tick
to retry.

Detach by passing `None` / `null`; engine falls back to flat-bps
behaviour.

## Notes

- The engine is per-venue (per-margin-pool). For cross-margin or
  multi-asset accounts, instantiate one engine per pool.
- The slippage knob is a flat bps haircut on the bankruptcy
  price; for venue-specific book-walk simulation, layer the
  existing `SlippageProfile` on top.
- ADL ranking is configurable per-engine; see the ranking table
  above. Custom ranking via a user-supplied callback is not in scope.
