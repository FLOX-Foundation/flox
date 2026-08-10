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

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace flox::venue
{

struct SeqMessage
{
  uint64_t seq{};
  int64_t tsNs{};
  OutboundEvent event{};
};

class ResendBuffer
{
 public:
  // Assign the next per-session sequence number, buffer the message, return seq.
  uint64_t append(uint64_t session, const OutboundEvent& e, int64_t tsNs)
  {
    auto& s = sessions_[session];
    const uint64_t seq = ++s.lastSeq;
    s.log.push_back(SeqMessage{seq, tsNs, e});
    return seq;
  }

  uint64_t lastSeq(uint64_t session) const
  {
    auto it = sessions_.find(session);
    return it == sessions_.end() ? 0 : it->second.lastSeq;
  }

  // Replay buffered messages with seq >= fromSeq, in order (gap-fill on reconnect).
  std::vector<SeqMessage> resend(uint64_t session, uint64_t fromSeq) const
  {
    std::vector<SeqMessage> out;
    auto it = sessions_.find(session);
    if (it == sessions_.end())
    {
      return out;
    }
    // The log is append-ordered by seq (append increments lastSeq; ackThrough
    // only trims the front), so binary-search the first seq >= fromSeq instead
    // of scanning the whole history.
    const auto& log = it->second.log;
    const auto lo = std::lower_bound(log.begin(), log.end(), fromSeq,
                                     [](const SeqMessage& m, uint64_t s)
                                     { return m.seq < s; });
    out.assign(lo, log.end());
    return out;
  }

  // Drop acknowledged history up to and including `throughSeq` (bounded memory).
  void ackThrough(uint64_t session, uint64_t throughSeq)
  {
    auto it = sessions_.find(session);
    if (it == sessions_.end())
    {
      return;
    }
    auto& log = it->second.log;
    size_t keep = 0;
    while (keep < log.size() && log[keep].seq <= throughSeq)
    {
      ++keep;
    }
    log.erase(log.begin(), log.begin() + static_cast<std::ptrdiff_t>(keep));
  }

 private:
  struct Session
  {
    uint64_t lastSeq{0};
    std::vector<SeqMessage> log;
  };
  std::unordered_map<uint64_t, Session> sessions_;
};

// Client-side gap detector: tracks the next expected sequence and reports the
// first missing seq when a message arrives out of order.
class GapDetector
{
 public:
  // Returns {gap, fromSeq}: gap=true means a resend for [fromSeq, seq) is needed.
  std::pair<bool, uint64_t> observe(uint64_t seq)
  {
    if (seq == expected_)
    {
      ++expected_;
      return {false, 0};
    }
    if (seq > expected_)
    {
      const uint64_t from = expected_;
      expected_ = seq + 1;
      return {true, from};
    }
    return {false, 0};  // duplicate / already seen
  }
  uint64_t expected() const noexcept { return expected_; }

 private:
  uint64_t expected_{1};
};

}  // namespace flox::venue
