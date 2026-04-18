# Slippage

The backtest engine applies slippage to market-style fills: market orders, triggered stop-market orders, and triggered trailing stops. Limit orders trade at their posted price; queue simulation handles their realism separately.

Slippage is configured per `BacktestConfig` with a default profile and optional per-symbol overrides.

## SlippageProfile

```cpp
enum class SlippageModel : uint8_t { NONE, FIXED_TICKS, FIXED_BPS, VOLUME_IMPACT };

struct SlippageProfile
{
  SlippageModel model{SlippageModel::NONE};
  int32_t ticks{0};         // FIXED_TICKS: number of ticks against the taker
  Price tickSize{};         // FIXED_TICKS: price per tick; zero falls back to one raw price unit
  double bps{0.0};          // FIXED_BPS: basis points against the taker
  double impactCoeff{0.0};  // VOLUME_IMPACT: coeff * (orderQty / levelQty)
};
```

| Model | Meaning |
|-------|---------|
| `NONE` | Fill at the book-reported best price. |
| `FIXED_TICKS` | Shift the fill by `ticks * tickSize` against the taker (buys pay more, sells receive less). Set `tickSize` to the venue's minimum price increment; leaving it zero falls back to one raw price unit, which is rarely what you want for real instruments. |
| `FIXED_BPS` | Shift the fill by `bps` basis points of the fill price. `bps = 10.0` is 10 bps = 0.10%. |
| `VOLUME_IMPACT` | Shift proportional to `impactCoeff * (orderQty / levelQty)`. Larger orders relative to top-of-book size move the price more. |

## Applying slippage

```cpp
BacktestConfig cfg;
cfg.defaultSlippage = {SlippageModel::FIXED_BPS, 0, Price{}, 5.0, 0.0};  // 5 bps default
cfg.perSymbolSlippage.emplace_back(
    kBtcUsd, SlippageProfile{SlippageModel::VOLUME_IMPACT, 0, Price{}, 0.0, 0.01});

BacktestRunner runner(cfg);  // config is forwarded to the simulated executor
```

For direct-executor use:

```cpp
SimulatedExecutor exec(clock);
exec.setDefaultSlippage({SlippageModel::FIXED_BPS, 0, Price{}, 5.0, 0.0});
exec.setSymbolSlippage(kBtcUsd,
                       {SlippageModel::FIXED_TICKS, 3, Price::fromDouble(0.25), 0.0, 0.0});
```

## Bindings

| Language | Entry point |
|----------|-------------|
| Python | `SimulatedExecutor.set_default_slippage(model, ticks, tick_size, bps, impact_coeff)` / `set_symbol_slippage(symbol, ...)` |
| C API | `flox_executor_set_default_slippage(exec, model, ticks, tick_size, bps, impact)` |
| Codon | `SimulatedExecutor.set_default_slippage(model, ticks, tick_size, bps, impact_coeff)` |
| JavaScript | `executor.setDefaultSlippage("fixed_bps", 0, 0, 5.0, 0)` |

All bindings accept the same model values (`none`, `fixed_ticks`, `fixed_bps`, `volume_impact`) either as strings (Python, JS, Codon enum constants) or as the `FloxSlippageModel` enum in the C API. Pass `tick_size = 0.0` for `NONE` / `FIXED_BPS` / `VOLUME_IMPACT`; it is only meaningful for `FIXED_TICKS`.
