/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/account.h"

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
  _positions.erase(
      std::remove_if(_positions.begin(), _positions.end(),
                     [&](const LeveragedPosition& p)
                     { return p.symbol == symbol; }),
      _positions.end());
}

void Account::setMark(SymbolId symbol, double price, int64_t tsNs)
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

double Account::markFor(SymbolId symbol) const
{
  for (const auto& m : _marks)
  {
    if (m.symbol == symbol)
    {
      return m.price;
    }
  }
  return 0.0;
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
    if (p.quantity == 0.0)
    {
      continue;
    }
    const int64_t ts = markTsFor(p.symbol);
    if (ts == INT64_MIN)
    {
      return true;  // never marked
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
  _rolling.push_back(RollingFill{tsNs, notional, symbol});
  _rollingTotal += notional;
  evictExpired(tsNs);
}

std::vector<std::pair<SymbolId, double>>
Account::rollingNotionalBySymbol30d() const
{
  std::vector<std::pair<SymbolId, double>> out;
  for (const auto& f : _rolling)
  {
    bool found = false;
    for (auto& [sym, total] : out)
    {
      if (sym == f.symbol)
      {
        total += f.notional;
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
    _rollingTotal -= _rolling.front().notional;
    _rolling.pop_front();
  }
  if (_rollingTotal < 0.0)
  {
    _rollingTotal = 0.0;
  }
}

double Account::totalNotional() const
{
  double n = 0.0;
  for (const auto& p : _positions)
  {
    const double mark = markFor(p.symbol);
    const double px = mark > 0.0 ? mark : p.entryPrice;
    n += std::abs(p.quantity) * px * p.contractMultiplier;
  }
  return n;
}

double Account::totalUnrealisedPnl() const
{
  double upnl = 0.0;
  for (const auto& p : _positions)
  {
    const double mark = markFor(p.symbol);
    if (mark <= 0.0)
    {
      continue;  // no mark → assume valued at entry; zero uPnL.
    }
    upnl += p.quantity * (mark - p.entryPrice) * p.contractMultiplier;
  }
  return upnl;
}

double Account::marginNotional() const
{
  double n = 0.0;
  for (const auto& p : _positions)
  {
    if (p.isLongOption)
    {
      continue;  // premium-paid long option posts no maintenance margin
    }
    const double mark = markFor(p.symbol);
    const double px = mark > 0.0 ? mark : p.entryPrice;
    n += std::abs(p.quantity) * px * p.contractMultiplier;
  }
  return n;
}

double Account::marginUnrealisedPnl() const
{
  double upnl = 0.0;
  for (const auto& p : _positions)
  {
    if (p.isLongOption)
    {
      continue;  // its loss is bounded by the paid premium, not a margin call
    }
    const double mark = markFor(p.symbol);
    if (mark <= 0.0)
    {
      continue;
    }
    upnl += p.quantity * (mark - p.entryPrice) * p.contractMultiplier;
  }
  return upnl;
}

double Account::crossHeadroom(double tierFraction) const
{
  const double notional = marginNotional();
  const double upnl = marginUnrealisedPnl();
  const double mmReq = notional * tierFraction;
  return _equity + upnl - mmReq;
}

}  // namespace flox
