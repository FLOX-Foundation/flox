/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include "flox/common.h"

namespace flox
{
#ifdef FLOX_UNIT_TEST
class L3OrderBookProbe;
#endif

enum class OrderStatus : uint8_t
{
  Ok,
  NoCapacity,
  NotFound,
  Extant
};

template <std::size_t MaxOrders = 8192>
class L3OrderBook
/*
 * Purpose:
 * - Record orders resting on the book: at what price, on which side, and at what quantity
 * Invariants:
 * - Compile time fixed capacity
 * - Zero runtime allocations
 * - No exceptions
 * - Time Price FIFO ordering at bid/ask price levels
*/
{
  using Index = std::uint32_t;

 public:
  L3OrderBook()
  {
    for (Index i = 0; i < MaxOrders; ++i)
    {
      orders_[i].nextFree = i + 1;
    }
    orders_[MaxOrders - 1].nextFree = kInvalid;
    freeHead_ = 0;
  }

  OrderStatus addOrder(OrderId id, Price price, Quantity quantity, Side side) noexcept
  {
    if (findSlot(id) != kInvalid)
    {
      return OrderStatus::Extant;
    }

    const auto slotIdx = allocateSlot();
    if (slotIdx == kInvalid)
    {
      return OrderStatus::NoCapacity;
    }

    if (!insertMapping(slotIdx, id))
    {
      deallocateSlot(slotIdx);
      return OrderStatus::NoCapacity;
    }

    auto& slot = orders_[slotIdx];
    slot.id = id;
    slot.price = price;
    slot.quantity = quantity;
    slot.side = side;

    const auto levelIdx = findOrCreateLevel(side, price);

    if (levelIdx == kInvalid)
    {
      deallocateSlot(slotIdx);
      eraseMapping(id);
      return OrderStatus::NoCapacity;
    }

    linkToLevel(slotIdx, side, levelIdx);
    return OrderStatus::Ok;
  }

  OrderStatus removeOrder(OrderId id) noexcept
  {
    const auto slotIdx = findSlot(id);
    if (slotIdx == kInvalid)
    {
      return OrderStatus::NotFound;
    }
    if (!eraseMapping(id))
    {
      return OrderStatus::NotFound;
    }

    unlinkFromLevel(slotIdx);
    deallocateSlot(slotIdx);

    return OrderStatus::Ok;
  }

  OrderStatus modifyOrder(OrderId id, Quantity newQty) noexcept
  {
    const auto slotIdx = findSlot(id);
    if (slotIdx == kInvalid)
    {
      return OrderStatus::NotFound;
    }
    auto& slot = orders_[slotIdx];
    auto& level = levelAt(slot.side, slot.level);

    const auto delta = newQty - slot.quantity;
    slot.quantity = newQty;
    level.totalQuantity += delta;

    return OrderStatus::Ok;
  }

  // NOTE: O(n) scan over price levels
  // Can be optimized later with cached extremes if required
  std::optional<Price> bestBid() const noexcept
  {
    Price maxPrice{};
    bool found = false;
    for (Index i = 0; i < MaxOrders; ++i)
    {
      if (bids_[i].used)
      {
        if (!found || maxPrice < bids_[i].price)
        {
          maxPrice = bids_[i].price;
          found = true;
        }
      }
    }
    return found ? std::optional<Price>{maxPrice} : std::nullopt;
  }
  std::optional<Price> bestAsk() const noexcept
  {
    Price minPrice{};
    bool found = false;
    for (Index i = 0; i < MaxOrders; ++i)
    {
      if (asks_[i].used)
      {
        if (!found || asks_[i].price < minPrice)
        {
          minPrice = asks_[i].price;
          found = true;
        }
      }
    }
    return found ? std::optional<Price>{minPrice} : std::nullopt;
  }

  Quantity bidAtPrice(Price price) const noexcept
  {
    for (Index i = 0; i < MaxOrders; ++i)
    {
      if (bids_[i].used && bids_[i].price == price)
      {
        return bids_[i].totalQuantity;
      }
    }
    return Quantity{};
  }

  Quantity askAtPrice(Price price) const noexcept
  {
    for (Index i = 0; i < MaxOrders; ++i)
    {
      if (asks_[i].used && asks_[i].price == price)
      {
        return asks_[i].totalQuantity;
      }
    }
    return Quantity{};
  }

 private:
#ifdef FLOX_UNIT_TEST
  friend class L3OrderBookProbe;
#endif

  static constexpr std::uint32_t kInvalid = UINT32_MAX;
  Index freeHead_;

  struct OrderSlot
  {
    OrderId id{};
    Price price{};
    Quantity quantity{};
    Side side{};

    Index prev{kInvalid};
    Index next{kInvalid};
    Index level{kInvalid};

    Index nextFree{kInvalid};
  };
  std::array<OrderSlot, MaxOrders> orders_;

  Index allocateSlot() noexcept
  {
    if (freeHead_ == kInvalid)
    {
      return kInvalid;
    }

    Index idx = freeHead_;
    freeHead_ = orders_[idx].nextFree;

    return idx;
  }

  void deallocateSlot(Index idx) noexcept
  {
    if (idx >= MaxOrders)
    {
      return;
    }
    orders_[idx] = OrderSlot{};
    orders_[idx].nextFree = freeHead_;
    freeHead_ = idx;
  }

  static constexpr std::size_t IndexCapacity_ = MaxOrders * 2;

  enum class SlotState
  {
    Empty,
    Occupied,
    Tombstone
  };
  struct OrderIndex
  {
    Index slot{};
    OrderId id{};
    SlotState state{SlotState::Empty};
  };

  std::size_t hash(OrderId id) const noexcept
  {
    return id % IndexCapacity_;  // Assuming Order ID's are integral and well distributed
  }

  std::array<OrderIndex, IndexCapacity_> orderIndices_;

  bool insertMapping(Index slotIdx, OrderId id) noexcept
  {
    std::size_t h = hash(id);
    Index firstTombstone = kInvalid;
    for (Index i = 0; i < IndexCapacity_; ++i)
    {
      Index idx = (h + i) % IndexCapacity_;
      const auto st = orderIndices_[idx].state;
      if (st == SlotState::Occupied)
      {
        continue;
      }
      if (st == SlotState::Empty)
      {
        idx = (firstTombstone != kInvalid) ? firstTombstone : idx;
        orderIndices_[idx] = {slotIdx, id, SlotState::Occupied};
        return true;
      }
      if (st == SlotState::Tombstone && firstTombstone == kInvalid)
      {
        firstTombstone = idx;
      }
    }
    return false;
  }

  bool eraseMapping(OrderId id) noexcept
  {
    std::size_t h = hash(id);
    for (Index i = 0; i < IndexCapacity_; ++i)
    {
      const Index idx = (h + i) % IndexCapacity_;
      const auto st = orderIndices_[idx].state;

      if (st == SlotState::Empty)
      {
        return false;
      }
      if (st == SlotState::Occupied && orderIndices_[idx].id == id)
      {
        orderIndices_[idx].state = SlotState::Tombstone;
        return true;
      }
    }
    return false;
  }

  Index findSlot(OrderId id) const noexcept
  {
    std::size_t h = hash(id);
    for (Index i = 0; i < IndexCapacity_; ++i)
    {
      const Index idx = (h + i) % IndexCapacity_;
      const auto st = orderIndices_[idx].state;

      if (st == SlotState::Empty)
      {
        return kInvalid;
      }
      if (orderIndices_[idx].id == id && orderIndices_[idx].state == SlotState::Occupied)
      {
        return orderIndices_[idx].slot;
      }
    }
    return kInvalid;
  }

  struct PriceLevel
  {
    Price price{};
    Quantity totalQuantity{};
    Index head{kInvalid};
    Index tail{kInvalid};
    bool used{false};
  };

  std::array<PriceLevel, MaxOrders> bids_;
  std::array<PriceLevel, MaxOrders> asks_;

  Index findOrCreateLevel(Side side, Price price) noexcept
  {
    auto& levels = (side == Side::BUY) ? bids_ : asks_;
    for (Index i = 0; i < MaxOrders; ++i)
    {
      if (levels[i].used && levels[i].price == price)
      {
        return i;
      }
    }
    for (Index i = 0; i < MaxOrders; ++i)
    {
      if (!levels[i].used)
      {
        levels[i] = {price, Quantity{}, kInvalid, kInvalid, true};
        return i;
      }
    }
    return kInvalid;  // caller to translate this to OrderStatus::NoCapacity
  }

  PriceLevel& levelAt(Side side, Index levelIdx) noexcept
  {
    return (side == Side::BUY) ? bids_[levelIdx] : asks_[levelIdx];
  }

  void linkToLevel(Index slotIdx, Side side, Index levelIdx) noexcept
  {
    auto& level = levelAt(side, levelIdx);
    auto& slot = orders_[slotIdx];

    slot.level = levelIdx;
    slot.prev = kInvalid;
    slot.next = kInvalid;

    if (level.head == kInvalid)
    {
      // empty level
      level.head = level.tail = slotIdx;
    }
    else
    {
      // append to tail
      auto oldTail = level.tail;
      orders_[oldTail].next = slotIdx;
      slot.prev = oldTail;
      level.tail = slotIdx;
    }

    level.totalQuantity += slot.quantity;
  }

  void unlinkFromLevel(Index slotIdx) noexcept
  {
    auto& slot = orders_[slotIdx];
    auto& level = levelAt(slot.side, slot.level);

    const auto currPrev = slot.prev;
    const auto currNext = slot.next;

    if (currPrev != kInvalid)
    {
      orders_[currPrev].next = currNext;
    }
    else
    {
      level.head = currNext;
    }
    if (currNext != kInvalid)
    {
      orders_[currNext].prev = currPrev;
    }
    else
    {
      level.tail = currPrev;
    }
    level.totalQuantity -= slot.quantity;

    if (level.head == kInvalid)
    {
      level.used = false;
    }

    slot.prev = kInvalid;
    slot.next = kInvalid;
    slot.level = kInvalid;
  }
};
}  // namespace flox
