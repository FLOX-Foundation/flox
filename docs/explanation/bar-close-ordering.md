# Bar-close ordering on tied timestamps

When several `BarAggregator` instances close on the same wall-clock instant — for example, a 4-hour bar, a 1-hour bar, and a 5-minute bar all sharing a midnight UTC boundary — the order in which they fire `onBar` matters. A strategy that updates a coarse-context indicator on the 4-hour close and reads it on the 5-minute close depends on the coarse one firing first.

## Rule

flox dispatches bars in **aggregator registration order**. The first aggregator added to a `MultiTimeframeAggregator` (or subscribed to a `BarBus`) emits its tied-timestamp bar first; the next emits next; and so on.

For multi-TF strategies, this gives a simple authoring contract:

> Register coarsest-first.

```cpp
using namespace std::chrono_literals;

flox::MultiTimeframeAggregator<3> agg(&bus);
agg.addTimeInterval(4h);    // coarsest first → emits first on tied closes
agg.addTimeInterval(1h);
agg.addTimeInterval(5min);  // finest last → emits last on tied closes
```

The registration calls are typed by bar kind: `addTimeInterval(std::chrono::seconds)`, `addTickInterval(uint32_t)`, `addVolumeInterval(double)`. Each returns the slot index it took. Note that `addTimeInterval` takes **seconds**, whereas `BarEvent::barTypeParam` reports the interval in **nanoseconds** — do not pass an `_NS` constant to the registration call.

A strategy that does

```cpp
void onSymbolBar(SymbolContext& c, const BarEvent& ev) override {
  // barTypeParam is nanoseconds for Time bars; timeframe::H4.param is the
  // matching constant.
  if (ev.barTypeParam == flox::timeframe::H4.param) cacheTrend(ev.bar);
  else if (ev.barTypeParam == flox::timeframe::M5.param) maybeEnter(c, ev, lastH4Trend());
}
```

reads the up-to-date H4 context inside the M5 handler because the H4 callback fired first when both bars closed on the same wall-clock instant.

## Why not coarsest-first as an automatic rule

A previous draft considered automatic descending-duration sort inside `MultiTimeframeAggregator::onTrade`. Two reasons not to do it implicitly:

- The duration analog for non-Time bars (Tick, Volume, Range, Renko, Bps) does not have a single obvious sort key. A 200-volume threshold and a 5-minute interval are not directly comparable; a lexicographic policy would need to be invented and documented anyway.
- Authors who set up the aggregator know the coarsest-first ordering they want; making it explicit in the registration call is one line of code and keeps the dispatch path branch-free.

If a user really wants coarsest-first regardless of registration order, sort the timeframe list before registering:

```cpp
std::sort(intervals.begin(), intervals.end(), std::greater<>{});
for (auto seconds : intervals) agg.addTimeInterval(seconds);
```

## Replay determinism

Because the rule is registration order — not wall-clock arrival, not duration — replay produces the same dispatch sequence as the original run. The replay-equivalence gate exercises a multi-TF tied-close fixture; a regression that changes the iteration order in `MultiTimeframeAggregator::onTrade` (or in any `BarBus` fan-out) would surface there.

## See also

- [Multi-TF context helpers](../how-to/multi-tf-context.md). Read the most recent closed bar for any (symbol, timeframe) pair from a strategy.
- [Bar aggregation](../how-to/bar-aggregation.md). Pre-aggregate bars for fast backtesting.
