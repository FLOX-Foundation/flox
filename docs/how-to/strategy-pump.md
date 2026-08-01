# Sweep a signal over a tape (StrategyPump)

`StrategyPump` replays a tape into a strategy whose type is known at compile
time. That lets the compiler inline the handler into the replay loop and keep
the strategy's state in registers between events. For an EMA-cross handler
this runs 2-3.7x faster than delivering the same events through a virtual
interface, which matters when a parameter sweep replays the same tape a few
hundred times.

It is not a backtest. There is no order simulation, no fills, no positions,
no clocks. The pump answers one question: what does this signal do over this
tape. When the answer looks interesting, move to `BacktestRunner` and let it
price the orders.

## A strategy for the pump

Any type with `onTrade(const TradeEvent&)` and/or `onBar(const BarEvent&)`
works. No base class:

```cpp
#include "flox/backtest/strategy_pump.h"

struct EmaCross
{
  int64_t emaFast = 0;
  int64_t emaSlow = 0;
  int64_t flips = 0;
  int64_t lastSignal = 0;

  void onTrade(const flox::TradeEvent& ev)
  {
    const int64_t p = ev.trade.price.raw();
    emaFast += (p - emaFast) >> 4;
    emaSlow += (p - emaSlow) >> 7;
    const int64_t signal = emaFast > emaSlow ? 1 : -1;
    if (signal != lastSignal)
    {
      ++flips;
      lastSignal = signal;
    }
  }
};
```

## Run it over events in memory

```cpp
std::vector<flox::TradeEvent> tape = loadTape();

EmaCross strategy;
flox::StrategyPump<EmaCross> pump(strategy);
const auto stats = pump.runTrades(tape);
// stats.trades == tape.size(); results are in the strategy's own fields
```

`runBars` does the same for a `std::span<const BarEvent>`.

## Run it over a recorded .floxlog tape

```cpp
auto reader = flox::replay::createMultiSegmentReader(tapeDir);
EmaCross strategy;
flox::StrategyPump<EmaCross> pump(strategy);
const auto stats = pump.run(*reader);
```

Trade records convert exactly as `BacktestRunner` converts them, so a signal
tested on the pump sees the same prices it would see in a backtest. Book,
option-quote and pool-state records are counted in `stats.skipped` and not
delivered: book replay needs the pooled-event machinery, and the pump does
not carry it. A tape that is mostly book updates will spend most of its read
time on skipped records; the pump saves time on the strategy side, not the
IO side.

## Sweeping parameters

The pump holds a reference, so a sweep is a plain loop:

```cpp
for (int fastShift = 2; fastShift <= 6; ++fastShift)
{
  MyParamSignal s{fastShift};
  flox::StrategyPump<MyParamSignal> pump(s);
  pump.runTrades(tape);
  record(fastShift, s.score());
}
```

Each iteration reuses the tape already in memory. On a 2020s desktop core the
loop body runs at roughly a nanosecond per event for a light handler, so a
1M-event tape costs about a millisecond per parameter point.

## Where the speed comes from, and its limit

A virtual `onTrade` call forces the strategy's fields through memory on
every event, because the compiler cannot see past the call. When the
strategy is a template parameter the loop compiles down to a handful of
instructions per event with the EMAs living in registers. The same tape and
handler, measured both ways, is the `BM_Pump_*` pair in
`benchmarks/batch_delivery_benchmark.cpp`.

The limit: this only helps loops where the strategy is the main per-event
cost. Inside `BacktestRunner` the simulator and trackers dominate, which is
why the runner keeps its virtual strategy interface and the pump exists as a
separate tool.
