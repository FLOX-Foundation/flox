# Threaded Runner lifetime in Node

A `Runner` built with the threaded flag drives a live engine. The engine runs its
own consumer threads, and those threads may not touch V8, so every callback they
produce is queued onto a Node `ThreadSafeFunction` and delivered on the JS thread
at the next tick of the event loop.

That queue is two things at once, and the two have different lifetimes:

* a queue, which a C++ producer thread pushes work onto;
* a reference on Node's event loop, which keeps the process alive.

Neither half unwinds by itself, so the addon hands both back at points it picks.

## The rule

| When | What happens |
| --- | --- |
| `runner.start()` | The channel opens, just before the engine's threads are created. |
| `runner.stop()` | The engine stops and joins its threads, then the channel is released. Whatever is already queued still runs. |
| The runner is collected | The owner is retired first, then the channel is released. Queued items free their payload and return. |

Nothing holds the event loop open while no producer thread exists. Constructing a
threaded `Runner` and never starting it costs nothing, and a runner that has been
stopped lets the process exit.

```javascript
--8<-- "examples/node_threaded_runner_lifetime.js"
```

## Why stop() and not the destructor

Releasing the channel only from the destructor releases it never. The destructor
belongs to an object V8 owns, so it waits for garbage collection; garbage
collection waits for the event loop to go idle; and the loop cannot go idle while
the channel holds a reference on it. Under memory pressure the collector may break
the cycle by chance, which is worse than a reliable hang: the same script finishes
on one machine and sits forever on another.

## Why the queue is not emptied at release

Releasing a `ThreadSafeFunction` does not discard what is in it. Queued items are
still dispatched, on a later tick, possibly long after the object that queued them
has been destroyed. A consumer whose first move is to read through a pointer to
its owner would read freed memory.

Each queued payload therefore carries a share of a small liveness flag that the
owner retires in its destructor. A payload that arrives after that frees itself
and returns without calling into JS. Aborting the queue instead would leak, since
Node's own teardown path returns before the callback wrapper is freed and takes
the payload captured inside it along.

What this means for a strategy author: everything queued before `stop()` returns
is delivered normally, which is how a strategy's final `onStop` reaches JS. Events
still in flight when the runner itself goes away are dropped.

## Swapping a strategy on a live engine

`runner.replaceStrategy(index, newStrategy)` writes the new object's callbacks
from the JS thread while the engine's threads are running. Those threads never
read a callback reference. They read a small bitmask the JS thread publishes,
which says which handlers the current strategy provides, and the references
themselves stay on the thread that is allowed to touch them.

A swap is not instantaneous. An event can be queued against the outgoing set of
handlers and run against the incoming ones, which is what swapping a strategy on a
running engine means. The consumer re-reads the reference on the JS thread and
skips the call if the new strategy does not provide that handler.

## Backpressure

The queue is bounded. A producer that outruns the event loop gets a rejected call
rather than unbounded growth, and the rejected payload is freed on the spot.
Publishing far past the bound therefore costs delivery, not memory: expect the
number of events that reach JS to track the queue's capacity rather than the
number published.

## Hooks

`setPnlTracker`, `setStorageSink`, `setMarketDataRecorder` and `setExecutor`
attach objects whose callbacks reach JS on the thread that produced the signal. On
every path a threaded `Runner` exposes today that thread is the JS thread, because
signals originate from the emitter handed to `onTrade`, which already runs there.
So these hooks are called directly and synchronously. Pre-trade gates
(`setRiskManager`, `setKillSwitch`, `setOrderValidator`) have to return a verdict
to the caller and are supported on the synchronous `Runner` only.
