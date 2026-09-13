// node/src/tsfn_util.h -- shared ThreadSafeFunction helpers.
//
// Every threaded hook (strategy dispatch, PnL/storage/recorder/executor/
// listener hooks) creates its ThreadSafeFunction with an unbounded queue
// (max_queue_size = 0) and calls NonBlockingCall without checking the
// returned status. Two consequences:
//
//  * unbounded queue: nothing pushes back on the producer thread, so a
//    burst of events queues up synchronously with no delivery -- 38 MB
//    RSS growth measured at 200k queued events, hard SIGABRT measured at
//    2M (the queue's own allocator gives up).
//  * unchecked status: NonBlockingCall's own cleanup, on a non-ok status,
//    only owns the wrapper it allocates internally -- not the caller's
//    heap payload. Every call site that ignored the status leaked its
//    `*CallData` on a full or closing queue.
//
// kTsfnMaxQueueSize bounds the queue so the producer gets backpressure
// (a non-ok status) instead of unbounded growth; tsfnCall frees the
// payload whenever that status is not napi_ok so a bounded queue does
// not turn into a bounded leak.

#pragma once
#include <napi.h>

#include <cstddef>

// Deliberately NOT wrapped in a namespace: the node addon aggregates
// headers under two different namespaces for historical reasons
// (`node_flox` in strategy.h/data_ops.h/..., `flox_node` in hooks.h), and
// both need these unqualified.

// ~13 MB worst case at the ~200 bytes/event measured for TradeCallData.
inline constexpr size_t kTsfnMaxQueueSize = 65536;

template <typename T, typename Callback>
inline void tsfnCall(Napi::ThreadSafeFunction& tsfn, T* data, Callback callback)
{
  if (tsfn.NonBlockingCall(data, callback) != napi_ok)
  {
    delete data;
  }
}
