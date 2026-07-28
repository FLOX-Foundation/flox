/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * O(1) tick-indexed ladder matching book: the performance-path order-level
 * resting book the reference MatchingBook is measured against (same interface).
 *
 * Design:
 * - Two dense price ladders (bids, asks), one Level per tick over a bounded
 *   price band. Level index = (price - base) / tick, so price<->level is O(1).
 * - Intrusive FIFO order list per level (time priority) over a preallocated
 *   node pool with a free list -- zero allocations in steady state.
 * - An occupancy bitmap per side + a cached best cursor: best is O(1), and the
 *   next non-empty level on a cursor move is a single word scan.
 * - Order id -> node via an open-addressing table (no per-order allocation).
 *
 * The band is enforced upstream by the engine's price collar (SymbolConfig
 * min/maxPrice); in-band is a precondition of addResting.
 */
#pragma once

#include "flox/book/resting_order.h"

#include <bit>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace flox
{

class LadderBook
{
 public:
  struct Config
  {
    int64_t basePriceRaw{};  // price.raw() of level 0
    int64_t tickRaw{};       // raw ticks per level (> 0)
    int32_t numLevels{};     // ladder height per side
    int32_t maxOrders{};     // node-pool capacity
  };

  explicit LadderBook(Config c)
      : base_(c.basePriceRaw),
        tick_(c.tickRaw),
        numLevels_(c.numLevels),
        bidLevels_(static_cast<size_t>(c.numLevels)),
        askLevels_(static_cast<size_t>(c.numLevels)),
        nodes_(static_cast<size_t>(c.maxOrders)),
        bidBits_(wordsFor(c.numLevels), 0),
        askBits_(wordsFor(c.numLevels), 0)
  {
    for (int32_t i = 0; i < c.maxOrders; ++i)
    {
      nodes_[static_cast<size_t>(i)].nextFree = i + 1;
    }
    if (c.maxOrders > 0)
    {
      nodes_[static_cast<size_t>(c.maxOrders) - 1].nextFree = -1;
    }
    freeHead_ = c.maxOrders > 0 ? 0 : -1;

    idxCap_ = 1;
    while (idxCap_ < static_cast<size_t>(c.maxOrders) * 2 + 1)
    {
      idxCap_ <<= 1;
    }
    idx_.assign(idxCap_, Slot{});
  }

  bool contains(OrderId id) const noexcept { return findSlot(id) != -1; }
  bool empty() const noexcept { return count_ == 0; }
  bool full() const noexcept { return freeHead_ == -1; }

  void addResting(Side side, const RestingOrder& o) noexcept
  {
    const int32_t lvl = levelOf(o.price);
    if (lvl < 0 || lvl >= numLevels_)
    {
      return;  // out of band -- engine's collar must prevent this
    }
    const int32_t n = allocNode();
    if (n < 0)
    {
      return;  // capacity -- engine should gate via full() before matching
    }
    Node& node = nodes_[static_cast<size_t>(n)];
    node.order = o;
    node.level = lvl;
    node.side = side;
    node.prev = -1;
    node.next = -1;

    Level& level = levelRef(side, lvl);
    if (level.head < 0)
    {
      level.head = level.tail = n;
      setBit(bitsRef(side), lvl);
      onLevelOccupied(side, lvl);
    }
    else
    {
      nodes_[static_cast<size_t>(level.tail)].next = n;
      node.prev = level.tail;
      level.tail = n;
    }
    level.totalQty += o.leaves + o.hidden;  // hidden reserve is real liquidity
    insertSlot(o.id, n);                    // count_ is maintained by allocNode/freeNode
  }

  const RestingOrder* find(OrderId id) const noexcept
  {
    const int32_t n = findSlot(id);
    return n < 0 ? nullptr : &nodes_[static_cast<size_t>(n)].order;
  }

  // In-place leaves reduction (keeps FIFO position). Caller guarantees
  // newLeaves <= current and > 0.
  bool reduce(OrderId id, Quantity newLeaves) noexcept
  {
    const int32_t n = findSlot(id);
    if (n < 0)
    {
      return false;
    }
    Node& node = nodes_[static_cast<size_t>(n)];
    levelRef(node.side, node.level).totalQty -= (node.order.leaves - newLeaves);
    node.order.leaves = newLeaves;
    return true;
  }

  // Reduce an order (by id) by `by`, with iceberg refill+requeue when its peak
  // is exhausted, else remove it. Used by pro-rata matching.
  void consumeById(OrderId id, Quantity by) noexcept
  {
    const int32_t n = findSlot(id);
    if (n < 0)
    {
      return;
    }
    Node& node = nodes_[static_cast<size_t>(n)];
    node.order.leaves -= by;
    levelRef(node.side, node.level).totalQty -= by;
    if (!node.order.leaves.isZero())
    {
      return;
    }
    if (!node.order.hidden.isZero())
    {
      const Quantity newVis =
          (node.order.peak < node.order.hidden) ? node.order.peak : node.order.hidden;
      node.order.leaves = newVis;
      node.order.hidden = node.order.hidden - newVis;
      moveToTail(node.side, node.level, n);
      return;
    }
    unlink(n);
    eraseSlot(id);
    freeNode(n);
  }

  std::optional<RestingOrder> cancel(OrderId id) noexcept
  {
    const int32_t n = findSlot(id);
    if (n < 0)
    {
      return std::nullopt;
    }
    const RestingOrder copy = nodes_[static_cast<size_t>(n)].order;
    unlink(n);
    eraseSlot(id);
    freeNode(n);
    return copy;
  }

  RestingOrder* peekBest(Side restingSide) noexcept
  {
    const int32_t lvl = (restingSide == Side::BUY) ? bestBidLevel_ : bestAskLevel_;
    if (lvl < 0)
    {
      return nullptr;
    }
    const int32_t head = levelRef(restingSide, lvl).head;
    return head < 0 ? nullptr : &nodes_[static_cast<size_t>(head)].order;
  }

  // Aggregated (price, total qty) per level, best-first. Used by auction uncross.
  void levels(Side side, std::vector<std::pair<Price, Quantity>>& out) const
  {
    out.clear();
    if (side == Side::BUY)
    {
      for (int32_t l = bestBidLevel_; l >= 0; l = highestSetAtOrBelow(bidBits_, l - 1))
      {
        out.emplace_back(priceOf(l), bidLevels_[static_cast<size_t>(l)].totalQty);
      }
    }
    else
    {
      for (int32_t l = bestAskLevel_; l >= 0; l = lowestSetAtOrAbove(askBits_, l + 1))
      {
        out.emplace_back(priceOf(l), askLevels_[static_cast<size_t>(l)].totalQty);
      }
    }
  }

  // Copy all orders at the best price of `restingSide` in FIFO order.
  void bestLevel(Side restingSide, std::vector<RestingOrder>& out) const
  {
    out.clear();
    const int32_t lvl = (restingSide == Side::BUY) ? bestBidLevel_ : bestAskLevel_;
    if (lvl < 0)
    {
      return;
    }
    const Level& level =
        (restingSide == Side::BUY) ? bidLevels_[static_cast<size_t>(lvl)]
                                   : askLevels_[static_cast<size_t>(lvl)];
    for (int32_t idx = level.head; idx >= 0; idx = nodes_[static_cast<size_t>(idx)].next)
    {
      out.push_back(nodes_[static_cast<size_t>(idx)].order);
    }
  }

  void fillBest(Side restingSide, Quantity by) noexcept
  {
    const int32_t lvl = (restingSide == Side::BUY) ? bestBidLevel_ : bestAskLevel_;
    if (lvl < 0)
    {
      return;
    }
    Level& level = levelRef(restingSide, lvl);
    const int32_t head = level.head;
    Node& node = nodes_[static_cast<size_t>(head)];
    node.order.leaves -= by;
    level.totalQty -= by;
    if (node.order.leaves.isZero())
    {
      if (!node.order.hidden.isZero())
      {
        // iceberg: expose the next peak, re-queue at the tail (lose priority).
        const Quantity newVis =
            (node.order.peak < node.order.hidden) ? node.order.peak : node.order.hidden;
        node.order.leaves = newVis;
        node.order.hidden = node.order.hidden - newVis;
        requeueFrontToTail(restingSide, lvl, head);
      }
      else
      {
        const OrderId id = node.order.id;
        unlink(head);  // clears the bit + advances cursor when the level empties
        eraseSlot(id);
        freeNode(head);
      }
    }
  }

  std::optional<Price> bestBid() const noexcept
  {
    return bestBidLevel_ < 0 ? std::nullopt : std::optional<Price>{priceOf(bestBidLevel_)};
  }
  std::optional<Price> bestAsk() const noexcept
  {
    return bestAskLevel_ < 0 ? std::nullopt : std::optional<Price>{priceOf(bestAskLevel_)};
  }

  Quantity availableWithin(Side takerSide, Price limit, bool isMarket) const noexcept
  {
    Quantity total{};
    if (takerSide == Side::BUY)
    {
      const int32_t limitLvl = isMarket ? (numLevels_ - 1) : clampLevel(limit);
      for (int32_t l = bestAskLevel_; l >= 0 && l <= limitLvl;
           l = lowestSetAtOrAbove(askBits_, l + 1))
      {
        total += askLevels_[static_cast<size_t>(l)].totalQty;
      }
    }
    else
    {
      const int32_t limitLvl = isMarket ? 0 : clampLevel(limit);
      for (int32_t l = bestBidLevel_; l >= 0 && l >= limitLvl;
           l = highestSetAtOrBelow(bidBits_, l - 1))
      {
        total += bidLevels_[static_cast<size_t>(l)].totalQty;
      }
    }
    return total;
  }

  // availableWithin, skipping makers for which skip(acct) is true. Iterates the
  // per-level node lists (not the aggregate totalQty) so an STP-aware FOK
  // precheck can exclude same-scope liquidity. See MatchingBook::availableWithinExcl.
  template <class Skip>
  Quantity availableWithinExcl(Side takerSide, Price limit, bool isMarket, Skip skip) const noexcept
  {
    Quantity total{};
    if (takerSide == Side::BUY)
    {
      const int32_t limitLvl = isMarket ? (numLevels_ - 1) : clampLevel(limit);
      for (int32_t l = bestAskLevel_; l >= 0 && l <= limitLvl;
           l = lowestSetAtOrAbove(askBits_, l + 1))
      {
        for (int32_t idx = askLevels_[static_cast<size_t>(l)].head; idx >= 0;
             idx = nodes_[static_cast<size_t>(idx)].next)
        {
          const RestingOrder& o = nodes_[static_cast<size_t>(idx)].order;
          if (!skip(o.accountId))
          {
            total += o.leaves + o.hidden;
          }
        }
      }
    }
    else
    {
      const int32_t limitLvl = isMarket ? 0 : clampLevel(limit);
      for (int32_t l = bestBidLevel_; l >= 0 && l >= limitLvl;
           l = highestSetAtOrBelow(bidBits_, l - 1))
      {
        for (int32_t idx = bidLevels_[static_cast<size_t>(l)].head; idx >= 0;
             idx = nodes_[static_cast<size_t>(idx)].next)
        {
          const RestingOrder& o = nodes_[static_cast<size_t>(idx)].order;
          if (!skip(o.accountId))
          {
            total += o.leaves + o.hidden;
          }
        }
      }
    }
    return total;
  }

 private:
  struct Level
  {
    int32_t head{-1};
    int32_t tail{-1};
    Quantity totalQty{};
  };
  struct Node
  {
    RestingOrder order{};
    int32_t prev{-1};
    int32_t next{-1};
    int32_t level{-1};
    Side side{};
    int32_t nextFree{-1};
  };
  struct Slot
  {
    OrderId id{};
    int32_t node{-1};
    uint8_t state{0};  // 0 empty, 1 occupied, 2 tombstone
  };

  static size_t wordsFor(int32_t levels) noexcept
  {
    return static_cast<size_t>((levels + 63) / 64);
  }

  int32_t levelOf(Price p) const noexcept
  {
    return static_cast<int32_t>((p.raw() - base_) / tick_);
  }
  int32_t clampLevel(Price p) const noexcept
  {
    const int64_t l = (p.raw() - base_) / tick_;
    if (l < 0)
    {
      return -1;
    }
    if (l >= numLevels_)
    {
      return numLevels_ - 1;
    }
    return static_cast<int32_t>(l);
  }
  Price priceOf(int32_t lvl) const noexcept
  {
    return Price::fromRaw(base_ + static_cast<int64_t>(lvl) * tick_);
  }

  Level& levelRef(Side s, int32_t lvl) noexcept
  {
    return (s == Side::BUY) ? bidLevels_[static_cast<size_t>(lvl)]
                            : askLevels_[static_cast<size_t>(lvl)];
  }
  std::vector<uint64_t>& bitsRef(Side s) noexcept { return (s == Side::BUY) ? bidBits_ : askBits_; }

  static void setBit(std::vector<uint64_t>& v, int32_t i) noexcept
  {
    v[static_cast<size_t>(i) >> 6] |= (1ULL << (static_cast<unsigned>(i) & 63U));
  }
  static void clearBit(std::vector<uint64_t>& v, int32_t i) noexcept
  {
    v[static_cast<size_t>(i) >> 6] &= ~(1ULL << (static_cast<unsigned>(i) & 63U));
  }

  static int32_t highestSetAtOrBelow(const std::vector<uint64_t>& v, int32_t i) noexcept
  {
    if (i < 0)
    {
      return -1;
    }
    size_t w = static_cast<size_t>(i) >> 6;
    const unsigned bit = static_cast<unsigned>(i) & 63U;
    uint64_t mask = (bit == 63U) ? ~0ULL : ((1ULL << (bit + 1U)) - 1ULL);
    uint64_t word = v[w] & mask;
    while (true)
    {
      if (word != 0)
      {
        return static_cast<int32_t>((w << 6) + static_cast<size_t>(63 - std::countl_zero(word)));
      }
      if (w == 0)
      {
        return -1;
      }
      --w;
      word = v[w];
    }
  }

  int32_t lowestSetAtOrAbove(const std::vector<uint64_t>& v, int32_t i) const noexcept
  {
    if (i >= numLevels_)
    {
      return -1;
    }
    if (i < 0)
    {
      i = 0;
    }
    size_t w = static_cast<size_t>(i) >> 6;
    const unsigned bit = static_cast<unsigned>(i) & 63U;
    uint64_t mask = ~((1ULL << bit) - 1ULL);
    uint64_t word = v[w] & mask;
    while (true)
    {
      if (word != 0)
      {
        const int32_t idx =
            static_cast<int32_t>((w << 6) + static_cast<size_t>(std::countr_zero(word)));
        return idx < numLevels_ ? idx : -1;
      }
      ++w;
      if (w >= v.size())
      {
        return -1;
      }
      word = v[w];
    }
  }

  void onLevelOccupied(Side s, int32_t lvl) noexcept
  {
    if (s == Side::BUY)
    {
      if (lvl > bestBidLevel_)
      {
        bestBidLevel_ = lvl;
      }
    }
    else
    {
      if (bestAskLevel_ < 0 || lvl < bestAskLevel_)
      {
        bestAskLevel_ = lvl;
      }
    }
  }

  void onLevelEmptied(Side s, int32_t lvl) noexcept
  {
    clearBit(bitsRef(s), lvl);
    if (s == Side::BUY)
    {
      if (lvl == bestBidLevel_)
      {
        bestBidLevel_ = highestSetAtOrBelow(bidBits_, lvl - 1);
      }
    }
    else
    {
      if (lvl == bestAskLevel_)
      {
        bestAskLevel_ = lowestSetAtOrAbove(askBits_, lvl + 1);
      }
    }
  }

  // Move the level's head node to its tail (iceberg refill re-queue). Precondition:
  // n == level.head and the level has >= 2 nodes handled; single-node is a no-op.
  void requeueFrontToTail(Side side, int32_t lvl, int32_t n) noexcept
  {
    Level& level = levelRef(side, lvl);
    if (level.head == level.tail)
    {
      return;  // single node -- already at the tail
    }
    Node& node = nodes_[static_cast<size_t>(n)];
    level.head = node.next;
    nodes_[static_cast<size_t>(node.next)].prev = -1;
    node.prev = level.tail;
    node.next = -1;
    nodes_[static_cast<size_t>(level.tail)].next = n;
    level.tail = n;
  }

  // Move any node in a level to its tail (pro-rata iceberg refill re-queue).
  void moveToTail(Side side, int32_t lvl, int32_t n) noexcept
  {
    Level& level = levelRef(side, lvl);
    if (level.tail == n)
    {
      return;  // already at the tail
    }
    Node& node = nodes_[static_cast<size_t>(n)];
    if (node.prev >= 0)
    {
      nodes_[static_cast<size_t>(node.prev)].next = node.next;
    }
    else
    {
      level.head = node.next;
    }
    nodes_[static_cast<size_t>(node.next)].prev = node.prev;  // node.next valid (n != tail)
    node.prev = level.tail;
    node.next = -1;
    nodes_[static_cast<size_t>(level.tail)].next = n;
    level.tail = n;
  }

  void unlink(int32_t n) noexcept
  {
    Node& node = nodes_[static_cast<size_t>(n)];
    Level& level = levelRef(node.side, node.level);
    level.totalQty -= (node.order.leaves + node.order.hidden);
    if (node.prev >= 0)
    {
      nodes_[static_cast<size_t>(node.prev)].next = node.next;
    }
    else
    {
      level.head = node.next;
    }
    if (node.next >= 0)
    {
      nodes_[static_cast<size_t>(node.next)].prev = node.prev;
    }
    else
    {
      level.tail = node.prev;
    }
    if (level.head < 0)
    {
      level.totalQty = Quantity{};  // exact-zero guard against fixed-point drift
      onLevelEmptied(node.side, node.level);
    }
    node.prev = node.next = -1;
    node.level = -1;
  }

  int32_t allocNode() noexcept
  {
    if (freeHead_ < 0)
    {
      return -1;
    }
    const int32_t idx = freeHead_;
    freeHead_ = nodes_[static_cast<size_t>(idx)].nextFree;
    ++count_;
    return idx;
  }
  void freeNode(int32_t idx) noexcept
  {
    nodes_[static_cast<size_t>(idx)] = Node{};
    nodes_[static_cast<size_t>(idx)].nextFree = freeHead_;
    freeHead_ = idx;
    --count_;
  }

  // ---- open-addressing id index ----
  size_t hash(OrderId id) const noexcept
  {
    uint64_t x = id;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return static_cast<size_t>(x) & (idxCap_ - 1);
  }
  void insertSlot(OrderId id, int32_t node) noexcept
  {
    size_t h = hash(id);
    size_t firstTomb = idxCap_;
    for (size_t i = 0; i < idxCap_; ++i)
    {
      const size_t k = (h + i) & (idxCap_ - 1);
      if (idx_[k].state == 1)
      {
        continue;
      }
      if (idx_[k].state == 2)
      {
        if (firstTomb == idxCap_)
        {
          firstTomb = k;
        }
        continue;
      }
      const size_t dst = (firstTomb != idxCap_) ? firstTomb : k;
      idx_[dst] = Slot{id, node, 1};
      return;
    }
  }
  int32_t findSlot(OrderId id) const noexcept
  {
    const size_t h = hash(id);
    for (size_t i = 0; i < idxCap_; ++i)
    {
      const size_t k = (h + i) & (idxCap_ - 1);
      if (idx_[k].state == 0)
      {
        return -1;
      }
      if (idx_[k].state == 1 && idx_[k].id == id)
      {
        return idx_[k].node;
      }
    }
    return -1;
  }
  void eraseSlot(OrderId id) noexcept
  {
    const size_t h = hash(id);
    for (size_t i = 0; i < idxCap_; ++i)
    {
      const size_t k = (h + i) & (idxCap_ - 1);
      if (idx_[k].state == 0)
      {
        return;
      }
      if (idx_[k].state == 1 && idx_[k].id == id)
      {
        idx_[k].state = 2;
        return;
      }
    }
  }

  int64_t base_;
  int64_t tick_;
  int32_t numLevels_;
  std::vector<Level> bidLevels_;
  std::vector<Level> askLevels_;
  std::vector<Node> nodes_;
  std::vector<uint64_t> bidBits_;
  std::vector<uint64_t> askBits_;
  std::vector<Slot> idx_;
  size_t idxCap_{0};
  int32_t freeHead_{-1};
  int32_t bestBidLevel_{-1};
  int32_t bestAskLevel_{-1};
  int32_t count_{0};
};

}  // namespace flox
