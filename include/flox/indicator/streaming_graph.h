#pragma once

// flox/indicator/streaming_graph.h
//
// DEPRECATED: StreamingIndicatorGraph has been collapsed into IndicatorGraph.
// One DAG class, both batch (set_bars/require) and streaming (step/current)
// methods on the same instance. See REFACTOR_PLAN.md.
//
// This header is kept as a compatibility shim and now declares
// StreamingIndicatorGraph as an alias of IndicatorGraph. New code should
// include "flox/indicator/indicator_pipeline.h" and use IndicatorGraph
// directly.

#include "flox/indicator/indicator_pipeline.h"

namespace flox::indicator
{

using StreamingIndicatorGraph = IndicatorGraph;

}  // namespace flox::indicator
