# Measure per-hop latency in production

Keep the latency contour on in production. One measured span costs about
35 ns including both clock reads (see `latency_contour_benchmark`), and in
return a p99.9 spike tells you which hop it came from instead of starting
an argument.

The contour is a set of named segments of the hot path (parse, bus,
strategy, risk, send), each backed by a log-bucketed histogram. Histograms
cover 1 ns to about 18 minutes with roughly 3% bucket error and report
p50/p99/p99.9/max. There is deliberately no mean: the expensive events live
in the tail, and a mean hides them.

## Wiring

```cpp
#include "flox/util/performance/latency_contour.h"

using Contour = flox::performance::LatencyContour<16>;

Contour contour;
const auto segParse = contour.registerSegment("parse");
const auto segBus = contour.registerSegment("bus");

// hot path:
const auto t0 = flox::performance::monotonicNs();
parseMessage(buf);
const auto t1 = flox::performance::monotonicNs();
contour.recordSpan(segParse, t0, t1);
```

Register segments at wiring time. Recording is wait-free and thread-safe.

## Reading the report

```text
parse:    n=1204321 p50=780 p90=1450 p99=3200 p99.9=18000 max=410000 (ns)
bus:      n=1204321 p50=310 p90=520 p99=900 p99.9=2100 max=95000 (ns)
strategy: n=1204321 p50=650 p90=1100 p99=2600 p99.9=310000 max=2100000 (ns)
```

What to look for:

- One segment has a fine p50 and an exploding p99.9. That segment
  allocates, faults, or takes a lock. In the sample above the strategy row
  is the suspect: 310 us at p99.9 against 650 ns at p50 usually means an
  allocation or a page fault.
- Every segment's tail degrades together. The cause is outside the
  process: CPU migration, C-states, a noisy neighbour on the core. Check
  pinning and the startup memory report.
- `max` sits far above p99.9 with a small count. Rare events (reconnect,
  snapshot rebuild) are leaking into the hot-path measurement and deserve
  their own segment.

## Receive timestamps

A software clock starts counting when your thread runs, but the packet
arrived earlier; scheduler wakeup time hides in between.
`flox/net/rx_timestamp.h` enables kernel receive timestamps on a socket
(`SO_TIMESTAMPING` on Linux, `SO_TIMESTAMP` on macOS) and extracts them
from `recvmsg` control messages, which turns "wire to parse" into a
measured segment. On Linux with a capable NIC you can get hardware
timestamps too; configuring the NIC (`ethtool -T`) is host setup and stays
outside flox.

## Caveats

Snapshots taken while writers are active are approximate because recording
uses relaxed atomics; quiesce writers when you need exact numbers. Reading
a quantile costs about 150 ns, so query from a monitoring thread, not from
the hot path.
