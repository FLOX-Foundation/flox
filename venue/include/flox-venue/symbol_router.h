/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox::venue
{

inline SymbolId symbolOf(const InboundCommand& c) noexcept
{
  if (const auto* n = std::get_if<NewOrder>(&c))
  {
    return n->symbol;
  }
  if (const auto* x = std::get_if<CancelOrder>(&c))
  {
    return x->symbol;
  }
  if (const auto* m = std::get_if<ModifyOrder>(&c))
  {
    return m->symbol;
  }
  if (const auto* mc = std::get_if<MassCancel>(&c))
  {
    return mc->symbol;
  }
  if (const auto* q = std::get_if<Quote>(&c))
  {
    return q->symbol;
  }
  if (const auto* ll = std::get_if<LastLookDecision>(&c))
  {
    return ll->symbol;
  }
  if (const auto* sm = std::get_if<SetMark>(&c))
  {
    return sm->symbol;
  }
  if (const auto* af = std::get_if<ApplyFunding>(&c))
  {
    return af->symbol;
  }
  if (const auto* ad = std::get_if<AdminCmd>(&c))
  {
    return ad->symbol;
  }
  if (const auto* d = std::get_if<Deposit>(&c))
  {
    return d->symbol;
  }
  if (const auto* w = std::get_if<Withdraw>(&c))
  {
    return w->symbol;
  }
  if (const auto* li = std::get_if<ListInstrument>(&c))
  {
    return li->symbol;
  }
  if (const auto* sb = std::get_if<SetBands>(&c))
  {
    return sb->symbol;
  }
  if (const auto* st = std::get_if<SetTriggerRef>(&c))
  {
    return st->symbol;
  }
  if (const auto* tt = std::get_if<TimeTick>(&c))
  {
    return tt->symbol;
  }
  if (const auto* sg = std::get_if<SetStpGroup>(&c))
  {
    return sg->symbol;
  }
  if (const auto* fs = std::get_if<SetFundingSchedule>(&c))
  {
    return fs->symbol;
  }
  return 0;  // snapshot-only records never route by symbol (recovery-path only)
}

template <class Book = MatchingBook>
class SymbolRouter
{
 public:
  explicit SymbolRouter(size_t nShards) : nShards_(nShards == 0 ? 1 : nShards) {}

  size_t shardOf(SymbolId s) const noexcept { return std::hash<SymbolId>{}(s) % nShards_; }
  size_t shardCount() const noexcept { return nShards_; }
  bool has(SymbolId s) const noexcept { return engines_.count(s) != 0; }

  MatchingEngine<Book>& addSymbol(SymbolConfig cfg, EventSink sink, Book book = Book{})
  {
    auto eng = std::make_unique<MatchingEngine<Book>>(cfg, std::move(sink), std::move(book));
    auto& ref = *eng;
    engines_.emplace(cfg.id, std::move(eng));
    return ref;
  }

  void submit(const InboundCommand& cmd)
  {
    if (auto it = engines_.find(symbolOf(cmd)); it != engines_.end())
    {
      it->second->submit(cmd);
    }
  }

  // Cross-shard account view for reconnect: the account's per-symbol snapshots,
  // limited to symbols where it has activity (orders / stops / a position).
  std::vector<std::pair<SymbolId, typename MatchingEngine<Book>::AccountSnapshot>> snapshotAccount(
      uint64_t acct) const
  {
    std::vector<std::pair<SymbolId, typename MatchingEngine<Book>::AccountSnapshot>> out;
    for (const auto& [sym, eng] : engines_)
    {
      auto s = eng->snapshotAccount(acct);
      if (!s.openOrders.empty() || !s.pendingStops.empty() || s.positionQty != 0)
      {
        out.emplace_back(sym, std::move(s));
      }
    }
    return out;
  }

 private:
  size_t nShards_;
  std::unordered_map<SymbolId, std::unique_ptr<MatchingEngine<Book>>> engines_;
};

}  // namespace flox::venue
