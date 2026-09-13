---
code: E_DATA_003
title: Aggregator cannot run on partitioned workers
severity: error
since: 0.8.1
---

# E_DATA_003 — Aggregator cannot run on partitioned workers

`BinaryLogReader::run(aggregators, n_threads)` (`DataReader.run` in Python) splits each
segment at compressed-block boundaries and gives every worker a clone of the panel. Some
aggregators cannot be cloned that way: `BookSnapshotBinAggregator` rebuilds the order book
by applying deltas in order from the start of the tape, so a worker that begins in the
middle has no book to apply them to.

The reader asks each aggregator through `IAggregator::supportsParallel()` before it
partitions anything. If the panel holds one that says no and the caller explicitly asked
for more than one thread, the run stops here, before the first byte is read.

With `n_threads=0` (the default, "auto") there is no error: the reader resolves the run to
a single thread. Earlier versions let the request through and threw from inside a worker
partway along, which made the outcome depend on whether the tape happened to be compressed
into enough blocks to trigger partitioning at all.

## How to fix

Ask for one thread when the panel needs the whole tape in order.

=== "Python"
    ```python
    reader = flox_py.DataReader("./tape")
    reader.run([book_agg], n_threads=1)
    ```

=== "C++"
    ```cpp
    flox::replay::BinaryLogReader reader(cfg);
    std::array<flox::replay::IAggregator*, 1> panel{&book_agg};
    reader.run(panel, 1);
    ```

Or split the panel: run the order-dependent aggregator on its own single-threaded pass and
let the rest go parallel. Two passes over a compressed tape usually still beat one single-
threaded pass over everything.

## Writing an aggregator that says no

Override `supportsParallel()` when the result depends on event order across the whole tape:

```cpp
class MyBookAggregator : public flox::replay::IAggregator
{
 public:
  bool supportsParallel() const override { return false; }
  // cloneEmpty() / merge() may still throw as a backstop.
};
```

The default is `true`, so aggregators that accumulate independently per event
(counters, histograms, OHLC bins) need no change.
