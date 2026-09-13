/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <flox/common.h>
#include <flox/execution/order.h>

namespace flox
{

// Every live connector used to hardcode its wire time-in-force (Bybit:
// always "Limit" with no TIF field at all; Hyperliquid: always Gtc; Bitget:
// a fixed value from static config, not the order). This is the one place
// that decides what Order::timeInForce / ExecutionFlags::postOnly actually
// mean, so the three connectors stop disagreeing with each other and with
// SimulatedExecutor, which already honours all four TimeInForce values.
enum class NormalizedTif
{
  GTC,
  IOC,
  FOK,
  POST_ONLY,
  UNSUPPORTED,  // GTD: no venue here has a native "good till date" order
                // flag, and silently downgrading it to GTC or IOC would
                // make the connector approximate a lifetime the caller
                // asked for explicitly. Callers reject the order instead
                // of guessing.
};

inline NormalizedTif normalizeTif(TimeInForce tif, bool postOnlyFlag)
{
  if (postOnlyFlag || tif == TimeInForce::POST_ONLY)
  {
    return NormalizedTif::POST_ONLY;
  }
  switch (tif)
  {
    case TimeInForce::GTC:
      return NormalizedTif::GTC;
    case TimeInForce::IOC:
      return NormalizedTif::IOC;
    case TimeInForce::FOK:
      return NormalizedTif::FOK;
    case TimeInForce::GTD:
      return NormalizedTif::UNSUPPORTED;
    case TimeInForce::POST_ONLY:
      return NormalizedTif::POST_ONLY;
  }
  return NormalizedTif::UNSUPPORTED;
}

}  // namespace flox
