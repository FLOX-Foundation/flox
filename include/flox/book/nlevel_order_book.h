/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/book/abstract_order_book.h"
#include "flox/book/events/book_update_event.h"
#include "flox/common.h"
#include "flox/util/base/math.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <type_traits>

namespace flox
{

template <size_t MaxLevels = 8192>
class NLevelOrderBook : public IOrderBook
{
 public:
  static constexpr size_t MAX_LEVELS = MaxLevels;

  explicit NLevelOrderBook(Price tickSize) noexcept
      : _tickSize(tickSize)
  {
    _tickSizeDiv = math::make_fastdiv64((uint64_t)_tickSize.raw(), 1);
    clear();
  }

  [[nodiscard]] inline std::optional<size_t> bestAskIndex() const noexcept
  {
    if (_bestAskIdx < MAX_LEVELS)
    {
      return _bestAskIdx;
    }

    if (_minAsk >= MAX_LEVELS)
    {
      return std::nullopt;
    }

    for (size_t i = _minAsk; i <= _maxAsk && i < MAX_LEVELS; ++i)
    {
      if (!_asks[i].isZero())
      {
        return i;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] inline std::optional<size_t> bestBidIndex() const noexcept
  {
    if (_bestBidIdx < MAX_LEVELS)
    {
      return _bestBidIdx;
    }

    if (_minBid >= MAX_LEVELS)
    {
      return std::nullopt;
    }

    for (size_t i = _maxBid + 1; i-- > _minBid;)
    {
      if (!_bids[i].isZero())
      {
        return i;
      }

      if (i == 0)
      {
        break;
      }
    }
    return std::nullopt;
  }

  void dump(std::ostream& os, size_t levels,
            int pricePrec = 4, int qtyPrec = 3, bool ansi = false) const
  {
    if (levels == 0)
    {
      return;
    }

    if (levels > 512)
    {
      levels = 512;
    }

    auto aBest = bestAsk();
    auto bBest = bestBid();

    const double ts = _tickSize.toDouble();
    os.setf(std::ios::fixed);
    os << "tick=" << std::setprecision(pricePrec) << ts
       << "  baseIndex=" << _baseIndex;

    if (aBest && bBest)
    {
      const double ba = aBest->toDouble(), bb = bBest->toDouble();
      os << "  spread=" << std::setprecision(pricePrec) << (ba - bb)
         << "  mid=" << std::setprecision(pricePrec) << ((ba + bb) * 0.5);
    }
    os << "\n";

    struct Row
    {
      double px{0}, qty{0};
      bool have{false};
    };

    std::array<Row, 512> asks{}, bids{};
    size_t na = 0, nb = 0;

    if (auto aIdxOpt = bestAskIndex())
    {
      for (size_t i = *aIdxOpt; i <= _maxAsk && i < MAX_LEVELS && na < levels; ++i)
      {
        if (_asks[i].isZero())
        {
          continue;
        }

        asks[na].have = true;
        asks[na].px = indexToPrice(i).toDouble();
        asks[na].qty = _asks[i].toDouble();
        ++na;
      }
    }

    if (auto bIdxOpt = bestBidIndex())
    {
      for (size_t i = *bIdxOpt + 1; i-- > _minBid && nb < levels;)
      {
        if (_bids[i].isZero())
        {
          if (i == 0)
          {
            break;
          }
          continue;
        }

        bids[nb].have = true;
        bids[nb].px = indexToPrice(i).toDouble();
        bids[nb].qty = _bids[i].toDouble();
        ++nb;

        if (i == 0)
        {
          break;
        }
      }
    }

    auto num_len = [](double v, int prec) -> int
    {
      char buf[64];
      auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, prec);
      return int(r.ptr - buf);
    };

    auto print_num = [](std::ostream& o, double v, int prec, int width)
    {
      char buf[64];
      auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, prec);
      int len = int(r.ptr - buf);
      for (int i = 0; i < width - len; ++i)
      {
        o.put(' ');
      }
      o.write(buf, len);
    };

    auto print_dash = [](std::ostream& o, int width)
    {
      for (int i = 0; i < width - 1; ++i)
      {
        o.put(' ');
      }
      o.put('-');
    };

    int wQty = 7;
    int wPx = 6;

    for (size_t i = 0; i < na; ++i)
    {
      wQty = std::max(wQty, num_len(asks[i].qty, qtyPrec));
      wPx = std::max(wPx, num_len(asks[i].px, pricePrec));
    }

    for (size_t i = 0; i < nb; ++i)
    {
      wQty = std::max(wQty, num_len(bids[i].qty, qtyPrec));
      wPx = std::max(wPx, num_len(bids[i].px, pricePrec));
    }

    const char* RED = ansi ? "\033[31m" : "";
    const char* GRN = ansi ? "\033[32m" : "";
    const char* DIM = ansi ? "\033[2m" : "";
    const char* RST = ansi ? "\033[0m" : "";

    os << "  " << std::setw(wQty) << "ASK_QTY"
       << "  " << std::setw(wPx) << "ASK_PX"
       << "  " << DIM << "│" << RST << "  "
       << std::setw(wPx) << "BID_PX"
       << "  " << std::setw(wQty) << "BID_QTY"
       << "\n";

    const size_t rows = std::max(na, nb);

    for (size_t r = 0; r < rows; ++r)
    {
      os << "  ";
      if (r < na && asks[r].have)
      {
        os << RED;
        print_num(os, asks[r].qty, qtyPrec, wQty);
        os << RST;
      }
      else
      {
        print_dash(os, wQty);
      }

      os << "  ";
      if (r < na && asks[r].have)
      {
        os << RED;
        print_num(os, asks[r].px, pricePrec, wPx);
        os << RST;
      }
      else
      {
        print_dash(os, wPx);
      }

      os << "  " << DIM << "│" << RST << "  ";

      if (r < nb && bids[r].have)
      {
        os << GRN;
        print_num(os, bids[r].px, pricePrec, wPx);
        os << RST;
      }
      else
      {
        print_dash(os, wPx);
      }

      os << "  ";
      if (r < nb && bids[r].have)
      {
        os << GRN;
        print_num(os, bids[r].qty, qtyPrec, wQty);
        os << RST;
      }
      else
      {
        print_dash(os, wQty);
      }

      os << "\n";
    }
  }

  void applyBookUpdate(const BookUpdateEvent& ev) override
  {
    const auto& up = ev.update;

    if (up.type == BookUpdateType::SNAPSHOT)
    {
      int64_t minIdx = std::numeric_limits<int64_t>::max();
      int64_t maxIdx = std::numeric_limits<int64_t>::min();

      auto acc = [&](const auto& vec)
      {
        for (const auto& [p, _] : vec)
        {
          const int64_t t = ticks(p);
          if (t < minIdx)
          {
            minIdx = t;
          }
          if (t > maxIdx)
          {
            maxIdx = t;
          }
        }
      };

      acc(up.bids);
      acc(up.asks);

      if (minIdx == std::numeric_limits<int64_t>::max())
      {
        clear();
      }
      else
      {
        reanchor(minIdx, maxIdx);
      }

      _bids.fill({});
      _asks.fill({});
      _minBid = _minAsk = MAX_LEVELS;
      _maxBid = _maxAsk = 0;
      _bestBidIdx = _bestAskIdx = MAX_LEVELS;
      _bestBidTick = _bestAskTick = -1;
    }

    for (const auto& [p, q] : up.bids)
    {
      const size_t i = localIndex(p);
      if (i >= MAX_LEVELS)
      {
        continue;
      }

      const bool had = !_bids[i].isZero();
      if (_bids[i].raw() == q.raw())
      {
        continue;
      }

      _bids[i] = q;

      if (!q.isZero())
      {
        if (i < _minBid)
        {
          _minBid = i;
        }
        if (i > _maxBid)
        {
          _maxBid = i;
        }
        if (_bestBidIdx >= MAX_LEVELS || i > _bestBidIdx)
        {
          _bestBidIdx = i;
          _bestBidTick = _baseIndex + static_cast<int64_t>(i);
        }
      }
      else if (had)
      {
        if (i == _bestBidIdx)
        {
          _bestBidIdx = prevNonZeroBid(i);
          _bestBidTick = (_bestBidIdx < MAX_LEVELS)
                             ? (_baseIndex + static_cast<int64_t>(_bestBidIdx))
                             : -1;
        }
        if (i == _minBid)
        {
          _minBid = nextNonZeroBid(_minBid);
        }
        if (i == _maxBid)
        {
          _maxBid = prevNonZeroBid(_maxBid);
        }
      }
    }

    for (const auto& [p, q] : up.asks)
    {
      const size_t i = localIndex(p);
      if (i >= MAX_LEVELS)
      {
        continue;
      }

      const bool had = !_asks[i].isZero();
      if (_asks[i].raw() == q.raw())
      {
        continue;
      }

      _asks[i] = q;

      if (!q.isZero())
      {
        if (i < _minAsk)
        {
          _minAsk = i;
        }
        if (i > _maxAsk)
        {
          _maxAsk = i;
        }
        if (_bestAskIdx >= MAX_LEVELS || i < _bestAskIdx)
        {
          _bestAskIdx = i;
          _bestAskTick = _baseIndex + static_cast<int64_t>(i);
        }
      }
      else if (had)
      {
        if (i == _bestAskIdx)
        {
          _bestAskIdx = nextNonZeroAsk(i);
          _bestAskTick = (_bestAskIdx < MAX_LEVELS)
                             ? (_baseIndex + static_cast<int64_t>(_bestAskIdx))
                             : -1;
        }
        if (i == _minAsk)
        {
          _minAsk = nextNonZeroAsk(_minAsk);
        }
        if (i == _maxAsk)
        {
          _maxAsk = prevNonZeroAsk(_maxAsk);
        }
      }
    }
  }

  [[nodiscard]] inline std::optional<Price> bestBid() const override
  {
    const int64_t t = _bestBidTick;
    if (t < 0)
    {
      return std::nullopt;
    }
    return std::optional<Price>{Price::fromRaw(_tickSize.raw() * t)};
  }

  [[nodiscard]] inline std::optional<Price> bestAsk() const override
  {
    const int64_t t = _bestAskTick;
    if (t < 0)
    {
      return std::nullopt;
    }
    return std::optional<Price>{Price::fromRaw(_tickSize.raw() * t)};
  }

  [[nodiscard]] inline Quantity bidAtPrice(Price p) const override
  {
    const size_t i = localIndex(p);
    return i < MAX_LEVELS ? _bids[i] : Quantity{};
  }

  [[nodiscard]] inline Quantity askAtPrice(Price p) const override
  {
    const size_t i = localIndex(p);
    return i < MAX_LEVELS ? _asks[i] : Quantity{};
  }

  [[nodiscard]] inline std::pair<double, double> consumeAsks(double needQtyBase) const noexcept
  {
    if (_bestAskIdx >= MAX_LEVELS)
    {
      return {0.0, 0.0};
    }

    double rem = needQtyBase;
    double notional = 0.0;

    const double ts = _tickSize.toDouble();
    size_t i = _bestAskIdx;
    const size_t hi = _maxAsk;

    double px = ts * static_cast<double>(_baseIndex + static_cast<int64_t>(i));

    for (; i <= hi && rem > math::EPS_QTY; ++i, px += ts)
    {
      const double q = _asks[i].toDouble();
      if (q <= 0.0)
      {
        continue;
      }

      const double take = q < rem ? q : rem;
      notional += take * px;
      rem -= take;
    }

    return {needQtyBase - rem, notional};
  }

  [[nodiscard]] inline std::pair<double, double> consumeBids(double needQtyBase) const noexcept
  {
    if (_bestBidIdx >= MAX_LEVELS)
    {
      return {0.0, 0.0};
    }

    double rem = needQtyBase;
    double notional = 0.0;

    const double ts = _tickSize.toDouble();
    size_t i = _bestBidIdx;
    const size_t lo = _minBid;

    double px = ts * static_cast<double>(_baseIndex + static_cast<int64_t>(i));

    for (;;)
    {
      if (rem <= math::EPS_QTY)
      {
        break;
      }

      const double q = _bids[i].toDouble();
      if (q > 0.0)
      {
        const double take = q < rem ? q : rem;
        notional += take * px;
        rem -= take;
      }

      if (i == lo)
      {
        break;
      }
      --i;
      px -= ts;
    }

    return {needQtyBase - rem, notional};
  }

  [[nodiscard]] inline Price tickSize() const noexcept { return _tickSize; }

  void clear() noexcept
  {
    _bids.fill({});
    _asks.fill({});
    _minBid = _minAsk = MAX_LEVELS;
    _maxBid = _maxAsk = 0;
    _baseIndex = 0;
    _bestBidIdx = _bestAskIdx = MAX_LEVELS;
    _bestBidTick = _bestAskTick = -1;
  }

 private:
  [[nodiscard]] inline int64_t ticks(Price p) const noexcept
  {
    const int64_t pr = p.raw();
    return math::sdiv_round_nearest(pr, _tickSizeDiv);
  }

  [[nodiscard]] inline Price indexToPrice(size_t i) const noexcept
  {
    const int64_t ts = _tickSize.raw();
    const int64_t tick = _baseIndex + static_cast<int64_t>(i);
    return Price::fromRaw(ts * tick);
  }

  [[nodiscard]] inline size_t localIndex(Price p) const noexcept
  {
    const int64_t t = ticks(p) - _baseIndex;
    return (static_cast<uint64_t>(t) < static_cast<uint64_t>(MAX_LEVELS))
               ? static_cast<size_t>(t)
               : MAX_LEVELS;
  }

  void reanchor(int64_t minIdx, int64_t maxIdx) noexcept
  {
    constexpr int64_t HYST = 8;
    const int64_t span = maxIdx - minIdx + 1;
    const int64_t curLo = _baseIndex;
    const int64_t curHi = _baseIndex + static_cast<int64_t>(MAX_LEVELS) - 1;

    if (curLo + HYST <= minIdx && maxIdx <= curHi - HYST)
    {
      return;
    }

    if (span >= static_cast<int64_t>(MAX_LEVELS))
    {
      _baseIndex = minIdx;
    }
    else
    {
      const int64_t mid = (minIdx + maxIdx) / 2;
      _baseIndex = mid - static_cast<int64_t>(MAX_LEVELS / 2);
    }
  }

  [[nodiscard]] inline size_t nextNonZeroAsk(size_t from) const noexcept
  {
    for (size_t i = from; i < MAX_LEVELS; ++i)
    {
      if (!_asks[i].isZero())
      {
        return i;
      }
    }
    return MAX_LEVELS;
  }

  [[nodiscard]] inline size_t prevNonZeroAsk(size_t from) const noexcept
  {
    for (size_t i = from + 1; i-- > 0;)
    {
      if (!_asks[i].isZero())
      {
        return i;
      }

      if (i == 0)
      {
        break;
      }
    }
    return MAX_LEVELS;
  }

  [[nodiscard]] inline size_t nextNonZeroBid(size_t from) const noexcept
  {
    for (size_t i = from; i < MAX_LEVELS; ++i)
    {
      if (!_bids[i].isZero())
      {
        return i;
      }
    }
    return MAX_LEVELS;
  }

  [[nodiscard]] inline size_t prevNonZeroBid(size_t from) const noexcept
  {
    for (size_t i = from + 1; i-- > 0;)
    {
      if (!_bids[i].isZero())
      {
        return i;
      }

      if (i == 0)
      {
        break;
      }
    }
    return MAX_LEVELS;
  }

 private:
  Price _tickSize;
  math::FastDiv64 _tickSizeDiv;

  int64_t _baseIndex{0};

  alignas(64) std::array<Quantity, MAX_LEVELS> _bids{};
  alignas(64) std::array<Quantity, MAX_LEVELS> _asks{};

  size_t _minBid{MAX_LEVELS}, _maxBid{0}, _minAsk{MAX_LEVELS}, _maxAsk{0};

  size_t _bestBidIdx{MAX_LEVELS}, _bestAskIdx{MAX_LEVELS};
  int64_t _bestBidTick{-1}, _bestAskTick{-1};
};

}  // namespace flox
