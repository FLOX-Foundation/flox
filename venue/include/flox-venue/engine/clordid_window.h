/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/engine/sorted_keys.h"
#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/messages.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace flox::venue
{

// clientOrderId dedup index, per account.
//
// A client repeating the SAME id is rejected at O(1) and costs nothing, which
// is the case the dedup exists for -- but a client with a broken id generator
// pours in DISTINCT ids, and a single never-pruned set keeps every one of them
// for the life of the process: memory, snapshot size, checkpoint pause and
// recovery time all growing without a bound. Measured: a million ids on one
// account is ~35 MiB of set.
//
// Hence two generations rotated in halves rather than a timestamp per id: an
// id survives between one and two windows, memory is bounded by two windows of
// distinct ids, and no per-id time has to be stored or serialized. Exchanges
// scope client order id uniqueness to the trading day, so a day is the honest
// window -- but the window is a config value whose default is 0, meaning
// unbounded, so this changes nothing until an operator asks for it.
//
// Every decision the dedup makes is here; the engine only reports the reject.
class ClOrdIdWindow
{
 public:
  // True if `clOrdId` was already used by `account` inside the dedup window
  // and the caller must therefore refuse the submission; false and newly
  // registered otherwise. clOrdId 0 means "not set" and is exempt.
  //
  // `windowNs` <= 0 is the unbounded window: ids accumulate in one generation
  // and nothing is ever dropped, which is what the venue always did.
  //
  // On the hot path of every submission -- non-virtual and defined here so it
  // inlines into the engine exactly as the member function it replaced did.
  bool duplicate(uint64_t account, uint64_t clOrdId, int64_t nowNs, int64_t windowNs)
  {
    if (clOrdId == 0)
    {
      return false;
    }
    Generations& seen = accounts_[account];
    rotate(seen, nowNs, windowNs);
    return seen.prev.count(clOrdId) != 0 || !seen.cur.insert(clOrdId).second;
  }

  // Recovery: one snapshot batch of ids into the named generation (0 = the
  // current half, 1 = the previous one).
  void restore(uint64_t account, uint32_t generation, const uint64_t* ids, uint32_t count)
  {
    Generations& seen = accounts_[account];
    std::unordered_set<uint64_t>& gen = generation == 0 ? seen.cur : seen.prev;
    for (uint32_t i = 0; i < count; ++i)
    {
      gen.insert(ids[i]);
    }
  }

  // Fold into the determinism digest. Both halves, in order and each sorted.
  // No separator between them: for every state the engine can actually reach,
  // the concatenation and the rotation moment below already tell two different
  // splits apart, and a marker that no reachable state needs is untested
  // weight.
  uint64_t hashInto(uint64_t h) const
  {
    for (uint64_t acct : sortedKeysOf(accounts_))
    {
      h = mix(h, 0xB007U);
      h = mix(h, acct);
      const Generations& seen = accounts_.at(acct);
      for (uint32_t g = 0; g < 2; ++g)
      {
        for (uint64_t id : sortedIds(seen, g))
        {
          h = mix(h, id);
        }
      }
      h = mix(h, static_cast<uint64_t>(seen.rotatedAtNs));
    }
    return h;
  }

  // Serialize as RestoreClOrdIds batches, accounts in key order and ids sorted
  // within each generation -- the same traversal the hash folds, so the file
  // is byte-for-byte deterministic.
  void writeSnapshot(Journal& out, int64_t ts) const
  {
    for (uint64_t acct : sortedKeysOf(accounts_))
    {
      const Generations& seen = accounts_.at(acct);
      for (uint32_t g = 0; g < 2; ++g)
      {
        RestoreClOrdIds batch{};
        batch.account = acct;
        batch.generation = g;
        for (uint64_t id : sortedIds(seen, g))
        {
          batch.ids[batch.count++] = id;
          if (batch.count == kClOrdIdBatch)
          {
            out.append(InboundCommand{batch}, ts);
            batch = RestoreClOrdIds{};
            batch.account = acct;
            batch.generation = g;
          }
        }
        if (batch.count > 0)
        {
          out.append(InboundCommand{batch}, ts);
        }
      }
    }
  }

 private:
  // Ids the account has already used, in two generations.
  struct Generations
  {
    std::unordered_set<uint64_t> cur;
    std::unordered_set<uint64_t> prev;
    int64_t rotatedAtNs{0};
  };

  // Past the window the older half goes and the newer takes its place. Done
  // on touch rather than on a timer: the engine has no timer, and an account
  // nobody is trading does not need its window rolled.
  static void rotate(Generations& w, int64_t nowNs, int64_t windowNs)
  {
    if (windowNs <= 0)
    {
      return;  // unbounded: what it always did
    }
    if (w.rotatedAtNs == 0)
    {
      w.rotatedAtNs = nowNs;
      return;
    }
    if (nowNs - w.rotatedAtNs < windowNs)
    {
      return;
    }
    w.prev = std::move(w.cur);
    w.cur.clear();
    w.rotatedAtNs = nowNs;
  }

  static std::vector<uint64_t> sortedIds(const Generations& seen, uint32_t generation)
  {
    const std::unordered_set<uint64_t>& gen = generation == 0 ? seen.cur : seen.prev;
    // order: sorted below, before the vector is handed out
    std::vector<uint64_t> ids(gen.begin(), gen.end());
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  std::unordered_map<uint64_t, Generations> accounts_;
};

}  // namespace flox::venue
