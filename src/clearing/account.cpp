/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/clearing/account.h"

#include <cctype>
#include <climits>

namespace flox
{

namespace
{
std::string toLower(const std::string& s)
{
  std::string lower;
  lower.reserve(s.size());
  for (char c : s)
  {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return lower;
}
}  // namespace

void Account::setMarginModeByName(const std::string& name)
{
  const std::string lower = toLower(name);
  if (lower == "cross")
  {
    _mode = MarginMode::Cross;
  }
  else if (lower == "isolated")
  {
    _mode = MarginMode::Isolated;
  }
}

void Account::openPosition(SymbolId symbol, double quantity, double entryPrice,
                           double isolatedEquity, double contractMultiplier, bool isLongOption)
{
  openPosition(symbol, Quantity::fromDouble(quantity), Price::fromDouble(entryPrice),
               Volume::fromDouble(isolatedEquity), Quantity::fromDouble(contractMultiplier),
               isLongOption);
}

void Account::openPosition(SymbolId symbol, Quantity quantity, Price entryPrice,
                           Volume isolatedEquity, Quantity contractMultiplier, bool isLongOption)
{
  LeveragedPosition p;
  p.accountId = _accountId;
  p.symbol = symbol;
  p.quantity = quantity;
  p.entryPrice = entryPrice;
  // Cross mode ignores per-position equity (LiquidationEngine reads
  // Account::equity instead). Isolated mode uses this field as the
  // posted margin backing this leg.
  p.equity = isolatedEquity;
  p.contractMultiplier = contractMultiplier;
  p.isLongOption = isLongOption;
  _positions.push_back(p);
}

void Account::closePosition(SymbolId symbol)
{
  // Realise first, erase second: the legs are what the PnL is computed from.
  const Price mark = markFor(symbol);
  if (mark.raw() > 0)
  {
    int64_t realised = 0;
    for (const auto& p : _positions)
    {
      if (p.symbol != symbol)
      {
        continue;
      }
      realised = checkedAddI64(realised, legUnrealisedPnlRaw(p, mark));
    }
    _equity = Volume::fromRaw(checkedAddI64(_equity.raw(), realised));
  }

  _positions.erase(
      std::remove_if(_positions.begin(), _positions.end(),
                     [&](const LeveragedPosition& p)
                     { return p.symbol == symbol; }),
      _positions.end());
}

int64_t Account::legUnrealisedPnlRaw(const LeveragedPosition& p, Price mark)
{
  const int64_t diff = checkedSubI64(mark.raw(), p.entryPrice.raw());
  const int64_t pnl = mulDivI64(p.quantity.raw(), diff, Volume::Scale);
  return mulDivI64(pnl, p.contractMultiplier.raw(), Quantity::Scale);
}

int64_t Account::legNotionalRaw(const LeveragedPosition& p, Price mark)
{
  const int64_t absQty = p.quantity.raw() < 0 ? -p.quantity.raw() : p.quantity.raw();
  // The multiplier scales money, so it is applied to the notional rather than
  // to the quantity: a fractional multiplier would otherwise round the
  // position size before it ever reached a price.
  const int64_t notional = mulDivI64(absQty, mark.raw(), Volume::Scale);
  return mulDivI64(notional, p.contractMultiplier.raw(), Quantity::Scale);
}

void Account::setMark(SymbolId symbol, double price, int64_t tsNs)
{
  setMark(symbol, Price::fromDouble(price), tsNs);
}

void Account::setMark(SymbolId symbol, Price price, int64_t tsNs)
{
  for (auto& m : _marks)
  {
    if (m.symbol == symbol)
    {
      m.price = price;
      m.tsNs = tsNs;
      return;
    }
  }
  _marks.push_back(Mark{symbol, price, tsNs});
}

Price Account::markFor(SymbolId symbol) const
{
  for (const auto& m : _marks)
  {
    if (m.symbol == symbol)
    {
      return m.price;
    }
  }
  return Price{};
}

int64_t Account::markTsFor(SymbolId symbol) const
{
  for (const auto& m : _marks)
  {
    if (m.symbol == symbol)
    {
      return m.tsNs;
    }
  }
  return INT64_MIN;
}

bool Account::hasStaleMarks(int64_t nowNs, int64_t budgetNs) const
{
  for (const auto& p : _positions)
  {
    if (p.quantity.isZero())
    {
      continue;
    }
    const int64_t ts = markTsFor(p.symbol);
    if (ts == INT64_MIN)
    {
      return true;  // never marked
    }
    // ts == 0 means the caller went through the bare `setMark(symbol,
    // price)` overload without a timestamp (the default keeps ts at 0, see
    // the header contract above). Computing `nowNs - 0` against a real
    // wall-clock nowNs is always tens of years, so the guard would report
    // every such mark as permanently stale -- the opposite of "treated as
    // fresh". Skip the staleness check for this sentinel instead.
    if (ts == 0)
    {
      continue;
    }
    if (nowNs - ts > budgetNs)
    {
      return true;
    }
  }
  return false;
}

void Account::recordFill(int64_t tsNs, double notional, SymbolId symbol)
{
  recordFill(tsNs, Volume::fromDouble(notional), symbol);
}

void Account::recordFill(int64_t tsNs, Volume notional, SymbolId symbol)
{
  _rolling.push_back(RollingFill{tsNs, notional, symbol});
  _rollingTotal = Volume::fromRaw(checkedAddI64(_rollingTotal.raw(), notional.raw()));
  evictExpired(tsNs);
}

std::vector<std::pair<SymbolId, Volume>>
Account::rollingNotionalBySymbol30d() const
{
  std::vector<std::pair<SymbolId, Volume>> out;
  for (const auto& f : _rolling)
  {
    bool found = false;
    for (auto& [sym, total] : out)
    {
      if (sym == f.symbol)
      {
        total = Volume::fromRaw(checkedAddI64(total.raw(), f.notional.raw()));
        found = true;
        break;
      }
    }
    if (!found)
    {
      out.emplace_back(f.symbol, f.notional);
    }
  }
  return out;
}

void Account::evictExpired(int64_t nowNs)
{
  const int64_t cutoff = nowNs - kThirtyDaysNs;
  while (!_rolling.empty() && _rolling.front().tsNs <= cutoff)
  {
    _rollingTotal =
        Volume::fromRaw(checkedSubI64(_rollingTotal.raw(), _rolling.front().notional.raw()));
    _rolling.pop_front();
  }
  // No clamp: what goes in comes back out to the raw, so a total below zero
  // would mean a fill was recorded negative, not that the sum drifted, and
  // rewriting it as zero would throw away every fill still in the window.
}

Volume Account::totalNotional() const
{
  int64_t n = 0;
  for (const auto& p : _positions)
  {
    const Price mark = markFor(p.symbol);
    const Price px = mark.raw() > 0 ? mark : p.entryPrice;
    n = checkedAddI64(n, legNotionalRaw(p, px));
  }
  return Volume::fromRaw(n);
}

Volume Account::totalUnrealisedPnl() const
{
  int64_t upnl = 0;
  for (const auto& p : _positions)
  {
    const Price mark = markFor(p.symbol);
    if (mark.raw() <= 0)
    {
      continue;  // no mark -> assume valued at entry; zero uPnL.
    }
    upnl = checkedAddI64(upnl, legUnrealisedPnlRaw(p, mark));
  }
  return Volume::fromRaw(upnl);
}

Volume Account::marginNotional() const
{
  int64_t n = 0;
  for (const auto& p : _positions)
  {
    if (p.isLongOption)
    {
      continue;  // premium-paid long option posts no maintenance margin
    }
    const Price mark = markFor(p.symbol);
    const Price px = mark.raw() > 0 ? mark : p.entryPrice;
    n = checkedAddI64(n, legNotionalRaw(p, px));
  }
  return Volume::fromRaw(n);
}

Volume Account::marginUnrealisedPnl() const
{
  int64_t upnl = 0;
  for (const auto& p : _positions)
  {
    if (p.isLongOption)
    {
      continue;  // its loss is bounded by the paid premium, not a margin call
    }
    const Price mark = markFor(p.symbol);
    if (mark.raw() <= 0)
    {
      continue;
    }
    upnl = checkedAddI64(upnl, legUnrealisedPnlRaw(p, mark));
  }
  return Volume::fromRaw(upnl);
}

Volume Account::crossHeadroom(double tierFraction) const
{
  const int64_t notional = marginNotional().raw();
  const int64_t upnl = marginUnrealisedPnl().raw();
  const int64_t mmReq = mulDivI64(notional, Volume::fromDouble(tierFraction).raw(), Volume::Scale);
  return Volume::fromRaw(checkedSubI64(checkedAddI64(_equity.raw(), upnl), mmReq));
}

}  // namespace flox
