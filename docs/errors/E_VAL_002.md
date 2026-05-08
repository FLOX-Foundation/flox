---
code: E_VAL_002
title: Invalid argument to a binding constructor or function
severity: error
since: 0.5.7
---

# E_VAL_002 — Invalid argument to a binding constructor or function

A value passed into a flox binding fell outside the accepted range.
Common cases:

- A latency model was constructed with a negative mean, stddev, or
  sample value.
- An `EmpiricalLatency` was constructed with all three sample arrays
  empty.
- A tape diff or other I/O-bound function received a path that is not
  a directory.

The exception message includes the field name and the bad value.

## How to fix

Inspect the failing call site and clamp or replace the offending
value before construction. A negative latency mean is almost always a
unit-conversion bug; an empty empirical sampler usually means the
calibration step ran on no data.

=== "Python"

    ```python
    from flox_py.latency_models import GaussianLatency

    # Bad — stddev cannot be negative.
    # GaussianLatency(feed_mean_ns=1000, feed_stddev_ns=-200, seed=42)

    # Good.
    GaussianLatency(feed_mean_ns=1000, feed_stddev_ns=200, seed=42)
    ```

=== "Node.js"

    ```javascript
    const flox = require('@flox-foundation/flox');

    // Bad — feedNs cannot be negative.
    // new flox.ConstantLatency({ feedNs: -1 });

    // Good.
    new flox.ConstantLatency({ feedNs: 100 });
    ```

## Common causes

- A microsecond value passed where nanoseconds are expected, then
  multiplied wrong and ending up negative.
- An empirical sampler initialised before any samples have been
  recorded.
- A typo passing the wrong field as the stddev (e.g. `-1` as a
  sentinel "unset" value).

## See also

- [Latency models](../how-to/backtest-with-latency.md)
- [Tape record and diff](../how-to/tape-record.md)
