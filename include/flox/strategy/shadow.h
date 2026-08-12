/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// Shadow mode: run a candidate strategy on the live feed with execution
// suppressed, next to a reference implementation of the same logic, and
// compare what they WOULD have done.
//
// EXPERIMENTAL scope: the comparison lives in an in-process std::deque with no
// serialization/export/binding, and ShadowSignalHandler::onSignal /
// records() are NOT synchronized -- a monitoring thread reading records()
// while signals arrive on the feed thread is a data race. Use it single-
// threaded for now; live-feed threading and a readable export are tracked in
// W25.
//
// Wiring: instead of strategy -> execution signal handler, wire
// strategy -> ShadowSignalHandler (records, forwards nowhere by default).
// Works for any strategy runtime -- C++, codon, python -- because the seam
// is ISignalHandler, below all of them.

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "flox/strategy/abstract_signal_handler.h"
#include "flox/strategy/signal.h"
#include "flox/util/performance/latency_contour.h"
#include "flox/util/performance/latency_histogram.h"

namespace flox
{

// Records every emitted signal with an emission timestamp. Execution is
// suppressed unless an inner handler is explicitly provided (canary mode).
class ShadowSignalHandler : public ISignalHandler
{
 public:
  struct Record
  {
    int64_t tsNs{0};
    SignalType type{SignalType::Market};
    SymbolId symbol{0};
    Side side{Side::BUY};
    int64_t priceRaw{0};
    int64_t qtyRaw{0};
  };

  explicit ShadowSignalHandler(size_t maxRecords = 1'000'000,
                               ISignalHandler* forwardTo = nullptr)
      : _maxRecords(maxRecords), _inner(forwardTo)
  {
  }

  void onSignal(const Signal& signal) override
  {
    Record r;
    r.tsNs = performance::monotonicNs();
    r.type = signal.type;
    r.symbol = signal.symbol;
    r.side = signal.side;
    r.priceRaw = signal.price.raw();
    r.qtyRaw = signal.quantity.raw();
    if (_records.size() >= _maxRecords)
    {
      _records.pop_front();
      ++_evicted;
    }
    _records.push_back(r);

    if (_inner)
    {
      _inner->onSignal(signal);
    }
  }

  const std::deque<Record>& records() const noexcept { return _records; }
  uint64_t evicted() const noexcept { return _evicted; }
  void clear() { _records.clear(); }

 private:
  size_t _maxRecords;
  ISignalHandler* _inner;
  std::deque<Record> _records;
  uint64_t _evicted{0};
};

// Compares two shadow streams -- candidate (e.g. the codon build) against
// reference (the C++ build) fed from the same event stream. Signals are
// matched by emission order; a divergence is any difference in type, symbol,
// side, price or quantity. Latency delta per matched pair (candidate minus
// reference emission timestamp) lands in a histogram: the answer to "is the
// codon path slower, and by how much, at p99".
class ShadowComparator
{
 public:
  struct Divergence
  {
    size_t index{0};
    std::string what;
  };

  struct Report
  {
    size_t referenceCount{0};
    size_t candidateCount{0};
    size_t matched{0};
    std::vector<Divergence> divergences;
    // candidate.tsNs - reference.tsNs per matched pair, clamped at zero for
    // the histogram (relative ordering jitter can make it negative).
    performance::LatencyHistogram* latencyDelta{nullptr};

    bool clean() const noexcept
    {
      return divergences.empty() && referenceCount == candidateCount;
    }
  };

  Report compare(const ShadowSignalHandler& reference,
                 const ShadowSignalHandler& candidate)
  {
    Report rep;
    const auto& a = reference.records();
    const auto& b = candidate.records();
    rep.referenceCount = a.size();
    rep.candidateCount = b.size();
    rep.latencyDelta = &_latencyDelta;

    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
      std::string diff = diffOne(a[i], b[i]);
      if (!diff.empty())
      {
        rep.divergences.push_back({i, std::move(diff)});
        continue;
      }
      ++rep.matched;
      const int64_t delta = b[i].tsNs - a[i].tsNs;
      _latencyDelta.record(delta > 0 ? delta : 0);
    }
    if (a.size() != b.size())
    {
      rep.divergences.push_back(
          {n, "signal count mismatch: reference=" + std::to_string(a.size()) +
                  " candidate=" + std::to_string(b.size())});
    }
    return rep;
  }

 private:
  static std::string diffOne(const ShadowSignalHandler::Record& a,
                             const ShadowSignalHandler::Record& b)
  {
    if (a.type != b.type)
    {
      return "type mismatch";
    }
    if (a.symbol != b.symbol)
    {
      return "symbol mismatch";
    }
    if (a.side != b.side)
    {
      return "side mismatch";
    }
    if (a.priceRaw != b.priceRaw)
    {
      return "price mismatch: " + std::to_string(a.priceRaw) + " vs " +
             std::to_string(b.priceRaw);
    }
    if (a.qtyRaw != b.qtyRaw)
    {
      return "quantity mismatch: " + std::to_string(a.qtyRaw) + " vs " +
             std::to_string(b.qtyRaw);
    }
    return {};
  }

  performance::LatencyHistogram _latencyDelta;
};

}  // namespace flox
