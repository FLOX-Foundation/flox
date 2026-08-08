/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/util/performance/busy_backoff.h"

namespace flox::performance
{

// Guard for the hazardous configuration "real-time priority + never-sleeping
// backoff on a core that is not isolated". An RT thread spinning AGGRESSIVE
// on a shared core starves every SCHED_OTHER thread pinned there (yield under
// SCHED_FIFO does not cede the CPU to lower-priority threads), including,
// possibly, the very producer it waits for. Userspace has no preempt_disable:
// the combination is only safe on isolated cores.

enum class RtSpinPolicy
{
  DOWNGRADE,  ///< Keep RT priority, force the backoff to a sleeping mode (default)
  REFUSE      ///< Keep the requested backoff, refuse to elevate the thread to RT
};

struct RtSpinDecision
{
  bool applyRtPriority{true};
  BackoffMode backoffMode{BackoffMode::ADAPTIVE};
  bool guardTriggered{false};
};

// Pure decision function so the matrix is unit-testable without real cores.
inline RtSpinDecision resolveRtSpinGuard(bool rtRequested,
                                         bool onIsolatedCore,
                                         BackoffMode requested,
                                         RtSpinPolicy policy = RtSpinPolicy::DOWNGRADE)
{
  RtSpinDecision d{rtRequested, requested, false};

  const bool hazardous =
      rtRequested && !onIsolatedCore && requested == BackoffMode::AGGRESSIVE;
  if (!hazardous)
  {
    return d;
  }

  d.guardTriggered = true;
  switch (policy)
  {
    case RtSpinPolicy::DOWNGRADE:
      d.applyRtPriority = true;
      d.backoffMode = BackoffMode::ADAPTIVE;
      break;
    case RtSpinPolicy::REFUSE:
      d.applyRtPriority = false;
      d.backoffMode = requested;
      break;
  }
  return d;
}

}  // namespace flox::performance
