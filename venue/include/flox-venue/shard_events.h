/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/messages.h"

#include <chrono>
#include <cstdint>

namespace flox::venue
{

// Steady clock for envelope stamps. Monotonic on purpose: these fields exist
// to be subtracted from each other, and a wall clock stepped by NTP turns a
// latency histogram into fiction. Never journaled, never compared with the
// UnixNanos timestamps the venue stores.
inline int64_t venueMonoNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct InboundCommandEvent;
struct ICommandListener
{
  virtual ~ICommandListener() = default;
  virtual void onCommand(const InboundCommandEvent& ev) = 0;
  // The ingress has been drained of what was available. Where a batched
  // durability barrier belongs: waiting past this point buys no more
  // amortisation and only adds latency.
  virtual void onBatchEnd() {}
};
struct InboundCommandEvent
{
  using Listener = ICommandListener;
  InboundCommand cmd{};
  uint64_t tickSequence = 0;  // gateway sequence number, stamped by the bus

  // Steady-clock stamps on the ENVELOPE, never inside the command: the journal
  // serializes the command alone, so these cost nothing to replay, the state
  // hash, or the journal format. Without them the venue can say how many
  // commands it processed but not where a slow one spent its time.
  //
  // recvMonoNs is when the frame came off the wire (0 = the producer did not
  // say); ingressMonoNs is when submit() enqueued it. The gap between the two
  // is decode plus admission; the gap from ingress to the consumer is queue
  // depth in time units, which no counter here exposed before.
  int64_t recvMonoNs = 0;
  int64_t ingressMonoNs = 0;
};

// ---- outbound: engine events fanned out to exec-report / market-data / ... ----
struct EngineEventMsg;
struct IEngineEventListener
{
  virtual ~IEngineEventListener() = default;
  virtual void onEngineEvent(const EngineEventMsg& ev) = 0;
};
struct EngineEventMsg
{
  using Listener = IEngineEventListener;
  OutboundEvent event{};
  uint64_t tickSequence = 0;

  // Provenance of the command that produced this event, copied from its
  // envelope, plus the moment the engine published it. An outbound consumer
  // holding these three can decompose end-to-end latency into wire->enqueue,
  // enqueue->published, published->delivered -- Metrics::observe does exactly
  // that. Zero means the producer did not stamp.
  int64_t causeRecvMonoNs = 0;
  int64_t causeIngressMonoNs = 0;
  int64_t publishMonoNs = 0;
};

}  // namespace flox::venue
