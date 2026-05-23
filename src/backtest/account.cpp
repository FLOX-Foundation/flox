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

void Account::openPosition(SymbolId symbol, double quantity, double entryPrice)
{
  LeveragedPosition p;
  p.accountId = _accountId;
  p.symbol = symbol;
  p.quantity = quantity;
  p.entryPrice = entryPrice;
  // For cross accounts, per-position equity is ignored; populated
  // here so isolated-mode call paths keep working without a second
  // openPosition signature.
  p.equity = 0.0;
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

void Account::setMark(SymbolId symbol, double price)
{
  for (auto& [sym, px] : _marks)
  {
    if (sym == symbol)
    {
      px = price;
      return;
    }
  }
  _marks.emplace_back(symbol, price);
}

double Account::markFor(SymbolId symbol) const
{
  for (const auto& [sym, px] : _marks)
  {
    if (sym == symbol)
    {
      return px;
    }
  }
  return 0.0;
}

void Account::recordFill(int64_t tsNs, double notional)
{
  _rolling.emplace_back(tsNs, notional);
  _rollingTotal += notional;
  evictExpired(tsNs);
}

void Account::evictExpired(int64_t nowNs)
{
  const int64_t cutoff = nowNs - kThirtyDaysNs;
  while (!_rolling.empty() && _rolling.front().first <= cutoff)
  {
    _rollingTotal -= _rolling.front().second;
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
    n += std::abs(p.quantity) * px;
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
    upnl += p.quantity * (mark - p.entryPrice);
  }
  return upnl;
}

double Account::crossHeadroom(double tierFraction) const
{
  const double notional = totalNotional();
  const double upnl = totalUnrealisedPnl();
  const double mmReq = notional * tierFraction;
  return _equity + upnl - mmReq;
}

}  // namespace flox
