/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/liquidation_engine.h"

#include "flox/backtest/account.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/execution/order.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace flox
{

namespace
{
// Allocate synthetic order IDs for liquidation orders out of the
// top half of the OrderId space so they don't collide with
// strategy-issued IDs.
std::atomic<uint64_t> g_liquidationOrderIdCounter{1ULL << 62};
OrderId nextLiquidationOrderId()
{
  return g_liquidationOrderIdCounter.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

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

void LiquidationEngine::setMarkImpactModelByName(const std::string& name, double weight)
{
  const std::string lower = toLower(name);
  if (lower == "none")
  {
    _markImpactModel = MarkImpactModel::None;
  }
  else if (lower == "book_anchored" || lower == "bookanchored" || lower == "anchored")
  {
    _markImpactModel = MarkImpactModel::BookAnchored;
  }
  else if (lower == "book_only" || lower == "bookonly")
  {
    _markImpactModel = MarkImpactModel::BookOnly;
  }
  else
  {
    return;
  }
  _markImpactWeight = weight;
}

void LiquidationEngine::attachAccount(Account* account)
{
  if (account == nullptr)
  {
    return;
  }
  for (Account* a : _accounts)
  {
    if (a == account)
    {
      return;  // already attached
    }
  }
  _accounts.push_back(account);
}

void LiquidationEngine::detachAccount(uint64_t accountId)
{
  _accounts.erase(
      std::remove_if(_accounts.begin(), _accounts.end(),
                     [&](const Account* a)
                     { return a != nullptr && a->accountId() == accountId; }),
      _accounts.end());
}

void LiquidationEngine::setAdlRankingByName(const std::string& name)
{
  std::string lower;
  lower.reserve(name.size());
  for (char c : name)
  {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "pnl_ratio" || lower == "pnlratio")
  {
    _adlRanking = AdlRanking::PnlRatio;
  }
  else if (lower == "binance")
  {
    _adlRanking = AdlRanking::Binance;
  }
  else if (lower == "bybit")
  {
    _adlRanking = AdlRanking::Bybit;
  }
  else if (lower == "position_size" || lower == "positionsize")
  {
    _adlRanking = AdlRanking::PositionSize;
  }
}

double LiquidationEngine::mmFractionFor(double notional) const
{
  if (_tiers.empty())
  {
    return 0.0;
  }
  double mm = _tiers.front().mmFraction;
  for (const auto& t : _tiers)
  {
    if (notional >= t.minNotional)
    {
      mm = t.mmFraction;
    }
    else
    {
      break;
    }
  }
  return mm;
}

void LiquidationEngine::resetStats() noexcept
{
  _statLiquidations = 0;
  _statInsurancePayments = 0;
  _statAdlCloseouts = 0;
  _deficitsPaidByFund.clear();
  _deficitsPaidByAdl.clear();
  _cascadeSizesPerTick.clear();
  _fundBalanceHistory.clear();
  _firstAdlTickIdx = UINT64_MAX;
  _onMarkTickCounter = 0;
}

LiquidationOutcome LiquidationEngine::onMarks(
    const std::vector<std::pair<SymbolId, double>>& marks, int64_t tsNs)
{
  // Phase 1: atomically update every attached account's mark for
  // every (symbol, price) pair. This is the headline T053 fix —
  // cross-margin walks below see ALL fresh marks instead of being
  // run with one fresh symbol and the rest stale.
  for (Account* acct : _accounts)
  {
    if (acct == nullptr)
    {
      continue;
    }
    for (const auto& [sym, px] : marks)
    {
      acct->setMark(sym, px, tsNs);
    }
  }

  // Phase 2: drive the liquidation walk once per symbol. Aggregate
  // outcomes across walks; cascade behaviour for each symbol is
  // preserved as it would be under per-symbol onMark.
  LiquidationOutcome aggregated;
  for (const auto& [sym, px] : marks)
  {
    if (px <= 0.0)
    {
      continue;
    }
    const LiquidationOutcome part = onMark(sym, px);
    aggregated.liquidated.insert(aggregated.liquidated.end(),
                                 part.liquidated.begin(), part.liquidated.end());
    aggregated.adlClosedOut.insert(aggregated.adlClosedOut.end(),
                                   part.adlClosedOut.begin(),
                                   part.adlClosedOut.end());
    aggregated.insuranceFundDelta += part.insuranceFundDelta;
    aggregated.liquidationsCount += part.liquidationsCount;
    aggregated.insurancePaymentsCount += part.insurancePaymentsCount;
    aggregated.adlCloseoutsCount += part.adlCloseoutsCount;
  }
  return aggregated;
}

LiquidationOutcome LiquidationEngine::onMark(SymbolId symbol, double markPrice)
{
  LiquidationOutcome aggregated;
  if (markPrice <= 0.0)
  {
    return aggregated;
  }
  const uint64_t tickIdx = _onMarkTickCounter++;

  double mark = markPrice;
  const uint32_t maxRounds =
      (_markImpactModel == MarkImpactModel::None || _executor == nullptr)
          ? 1u
          : (_maxCascadeDepth == 0u ? 1u : (_maxCascadeDepth + 1u));

  for (uint32_t round = 0; round < maxRounds; ++round)
  {
    OnMarkPass pass = onMarkOnce(symbol, mark, tickIdx);
    // Aggregate per-pass results into the call-level outcome.
    aggregated.liquidated.insert(aggregated.liquidated.end(),
                                 pass.outcome.liquidated.begin(),
                                 pass.outcome.liquidated.end());
    aggregated.adlClosedOut.insert(aggregated.adlClosedOut.end(),
                                   pass.outcome.adlClosedOut.begin(),
                                   pass.outcome.adlClosedOut.end());
    aggregated.insuranceFundDelta += pass.outcome.insuranceFundDelta;
    aggregated.liquidationsCount += pass.outcome.liquidationsCount;
    aggregated.insurancePaymentsCount += pass.outcome.insurancePaymentsCount;
    aggregated.adlCloseoutsCount += pass.outcome.adlCloseoutsCount;
    // Stop conditions: no fills this pass, no impact model, or no
    // executor (book mid unavailable).
    if (pass.outcome.liquidationsCount == 0)
    {
      break;
    }
    if (_markImpactModel == MarkImpactModel::None || _executor == nullptr)
    {
      break;
    }
    // Recompute the mark from the post-cascade book.
    const Price midPx = _executor->bookMidPrice(symbol);
    if (midPx.raw() <= 0)
    {
      break;  // book mid unavailable; fall back to current mark.
    }
    const double bookMid = midPx.toDouble();
    double nextMark = mark;
    if (_markImpactModel == MarkImpactModel::BookAnchored)
    {
      const double w = std::clamp(_markImpactWeight, 0.0, 1.0);
      nextMark = (1.0 - w) * markPrice + w * bookMid;
    }
    else if (_markImpactModel == MarkImpactModel::BookOnly)
    {
      nextMark = bookMid;
    }
    if (std::abs(nextMark - mark) < 1e-12)
    {
      break;  // mark didn't move; no point retrying.
    }
    mark = nextMark;
  }
  return aggregated;
}

LiquidationEngine::OnMarkPass LiquidationEngine::onMarkOnce(SymbolId symbol,
                                                            double markPrice,
                                                            uint64_t tickIdx)
{
  OnMarkPass pass;
  LiquidationOutcome& out = pass.outcome;
  if (markPrice <= 0.0)
  {
    return pass;
  }

  double accountDeficit = 0.0;
  // Account walk: cross-margin accounts evaluate the account-level
  // MM check and close worst-PnL legs first. Isolated accounts let
  // each position liquidate independently via the per-position
  // logic below (positions are visible via account.positionsMut()).
  for (Account* acct : _accounts)
  {
    if (acct == nullptr)
    {
      continue;
    }
    acct->setMark(symbol, markPrice);
    if (acct->marginMode() == MarginMode::Cross)
    {
      const auto walked = walkCrossAccount(*acct, symbol, markPrice);
      for (uint64_t id : walked.outcome.liquidated)
      {
        out.liquidated.push_back(id);
      }
      out.liquidationsCount += walked.outcome.liquidationsCount;
      accountDeficit += walked.deficit;
    }
    else
    {
      // Isolated: per-position MM check using the equity slice
      // posted at openPosition.
      const auto walked = walkIsolatedAccount(*acct, symbol, markPrice);
      for (uint64_t id : walked.outcome.liquidated)
      {
        out.liquidated.push_back(id);
      }
      out.liquidationsCount += walked.outcome.liquidationsCount;
      accountDeficit += walked.deficit;
    }
  }

  // Step 1: collect every orphan position on this symbol that's underwater.
  std::vector<size_t> underwater;
  for (size_t i = 0; i < _positions.size(); ++i)
  {
    const auto& p = _positions[i];
    if (p.symbol != symbol || p.quantity == 0.0)
    {
      continue;
    }
    const double notional = std::abs(p.quantity) * markPrice;
    const double mm = mmFractionFor(notional);
    const double mmReq = notional * mm;
    // Unrealised PnL at mark.
    const double upnl = p.quantity * (markPrice - p.entryPrice);
    if (p.equity + upnl < mmReq)
    {
      underwater.push_back(i);
    }
  }
  if (underwater.empty() && accountDeficit == 0.0 && out.liquidationsCount == 0)
  {
    return pass;
  }
  if (underwater.empty())
  {
    // No orphan work this tick, but accounts may have generated a
    // deficit and/or cascade-size stats need recording.
    if (out.liquidationsCount > 0)
    {
      _cascadeSizesPerTick.push_back(static_cast<uint32_t>(out.liquidationsCount));
    }
    runInsuranceAndAdlPhase(symbol, markPrice, accountDeficit, tickIdx, out);
    return pass;
  }

  // Step 2: liquidate each underwater position. With an executor
  // attached, route through it as a market order; without one,
  // fall back to the flat-bps slippage close.
  double totalDeficit = 0.0;
  std::vector<size_t> liquidated;
  liquidated.reserve(underwater.size());
  for (size_t idx : underwater)
  {
    auto& p = _positions[idx];
    double closePrice = 0.0;
    double filledQty = 0.0;
    if (_executor != nullptr)
    {
      const auto exClose = closeThroughExecutor(p, markPrice);
      closePrice = exClose.closePrice;
      filledQty = exClose.filledQty;
      if (filledQty <= 0.0)
      {
        // Executor couldn't fill any of this position this tick
        // (e.g. empty book). Leave the position in place; the next
        // onMark call will retry. Do not count as a liquidation.
        continue;
      }
    }
    else
    {
      const double signQ = (p.quantity > 0.0) ? 1.0 : -1.0;
      const double slip = _slippageBps / 10000.0;
      closePrice = markPrice * (1.0 - signQ * slip);
      filledQty = std::abs(p.quantity);
    }
    const double signedFilled = (p.quantity > 0.0) ? filledQty : -filledQty;
    const double realized = signedFilled * (closePrice - p.entryPrice);
    // Equity attributed to the filled portion (proportional).
    const double filledEquity = p.equity * (filledQty / std::abs(p.quantity));
    const double residualEquity = filledEquity + realized;
    out.liquidated.push_back(p.accountId);
    ++_statLiquidations;
    ++out.liquidationsCount;
    if (residualEquity < 0.0)
    {
      totalDeficit += -residualEquity;
    }
    // If the executor partial-filled, leave the unfilled remainder
    // in place for the next tick. Otherwise mark the whole position
    // for removal.
    if (filledQty >= std::abs(p.quantity) - 1e-12)
    {
      liquidated.push_back(idx);
    }
    else
    {
      const double remaining = std::abs(p.quantity) - filledQty;
      p.quantity = (p.quantity > 0.0) ? remaining : -remaining;
      p.equity -= filledEquity;
    }
  }
  // Remove fully-liquidated positions from the book. Iterate descending
  // so erase indexes stay valid.
  std::sort(liquidated.begin(), liquidated.end(), std::greater<size_t>());
  for (size_t i : liquidated)
  {
    _positions.erase(_positions.begin() + static_cast<long>(i));
  }
  _cascadeSizesPerTick.push_back(static_cast<uint32_t>(out.liquidationsCount));

  // Unified deficit covers orphan + account walks. T055 lifted ADL
  // routing to scan the combined candidate pool.
  totalDeficit += accountDeficit;
  if (totalDeficit <= 0.0)
  {
    return pass;
  }
  runInsuranceAndAdlPhase(symbol, markPrice, totalDeficit, tickIdx, out);
  return pass;
}

void LiquidationEngine::runInsuranceAndAdlPhase(SymbolId symbol,
                                                double markPrice,
                                                double totalDeficit,
                                                uint64_t tickIdx,
                                                LiquidationOutcome& out)
{
  if (totalDeficit <= 0.0)
  {
    return;
  }
  // Step 1: insurance fund covers what it can.
  const double insurancePayment = std::min(totalDeficit, _insuranceFund);
  if (insurancePayment > 0.0)
  {
    _insuranceFund -= insurancePayment;
    totalDeficit -= insurancePayment;
    out.insuranceFundDelta -= insurancePayment;
    ++_statInsurancePayments;
    ++out.insurancePaymentsCount;
    _deficitsPaidByFund.push_back(insurancePayment);
    _fundBalanceHistory.push_back(_insuranceFund);
  }
  if (totalDeficit <= 0.0 || !_adlEnabled)
  {
    return;
  }

  // Step 2: ADL across orphan + every attached account. Selection
  // criterion is "current PnL > 0 on this symbol"; ranking per the
  // configured AdlRanking. When ADL closes an account-owned
  // position, the realised PnL credits the owning account's equity
  // (cross mode) or the position's isolated_equity (isolated mode).
  struct AdlCandidate
  {
    Account* owner;  // nullptr → orphan position
    size_t idx;
    double score;
    double upnl;
  };
  std::vector<AdlCandidate> candidates;

  auto scorePosition = [this, markPrice](const LeveragedPosition& p,
                                         double upnl) -> double
  {
    switch (_adlRanking)
    {
      case AdlRanking::PnlRatio:
        return (p.equity > 0.0) ? (upnl / p.equity) : upnl;
      case AdlRanking::Binance:
      case AdlRanking::Bybit:
      {
        const double notional = std::abs(p.quantity) * markPrice;
        const double leverage =
            (p.equity > 0.0) ? (notional / p.equity) : 0.0;
        return upnl * leverage;
      }
      case AdlRanking::PositionSize:
        return std::abs(p.quantity);
    }
    return upnl;
  };

  // Orphan candidates.
  for (size_t i = 0; i < _positions.size(); ++i)
  {
    const auto& p = _positions[i];
    if (p.symbol != symbol || p.quantity == 0.0)
    {
      continue;
    }
    const double upnl = p.quantity * (markPrice - p.entryPrice);
    if (upnl > 0.0)
    {
      candidates.push_back({nullptr, i, scorePosition(p, upnl), upnl});
    }
  }
  // Account candidates.
  for (Account* acct : _accounts)
  {
    if (acct == nullptr)
    {
      continue;
    }
    auto& book = acct->positionsMut();
    for (size_t i = 0; i < book.size(); ++i)
    {
      const auto& p = book[i];
      if (p.symbol != symbol || p.quantity == 0.0)
      {
        continue;
      }
      const double upnl = p.quantity * (markPrice - p.entryPrice);
      if (upnl > 0.0)
      {
        candidates.push_back({acct, i, scorePosition(p, upnl), upnl});
      }
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const AdlCandidate& a, const AdlCandidate& b)
            { return a.score > b.score; });

  // Track indexes per source so we can erase descending after the
  // close loop.
  std::vector<size_t> orphanClose;
  std::unordered_map<Account*, std::vector<size_t>> acctClose;
  for (const auto& c : candidates)
  {
    if (totalDeficit <= 0.0)
    {
      break;
    }
    if (c.owner == nullptr)
    {
      const auto& p = _positions[c.idx];
      out.adlClosedOut.push_back(p.accountId);
      orphanClose.push_back(c.idx);
    }
    else
    {
      auto& p = c.owner->positionsMut()[c.idx];
      // Credit the account's equity (cross mode) or the leg's
      // isolated_equity (isolated mode) with the realised PnL. The
      // mark price is the close benchmark for ADL — no executor
      // routing on ADL closes by spec.
      if (c.owner->marginMode() == MarginMode::Cross)
      {
        c.owner->addEquity(c.upnl);
      }
      else
      {
        p.equity += c.upnl;
      }
      out.adlClosedOut.push_back(p.accountId);
      acctClose[c.owner].push_back(c.idx);
    }
    totalDeficit -= c.upnl;
    ++_statAdlCloseouts;
    ++out.adlCloseoutsCount;
    _deficitsPaidByAdl.push_back(c.upnl);
    if (_firstAdlTickIdx == UINT64_MAX)
    {
      _firstAdlTickIdx = tickIdx;
    }
  }
  // Erase orphan positions descending.
  std::sort(orphanClose.begin(), orphanClose.end(), std::greater<size_t>());
  for (size_t i : orphanClose)
  {
    _positions.erase(_positions.begin() + static_cast<long>(i));
  }
  // Erase account positions descending per account.
  for (auto& [acct, idxs] : acctClose)
  {
    std::sort(idxs.begin(), idxs.end(), std::greater<size_t>());
    auto& book = acct->positionsMut();
    for (size_t i : idxs)
    {
      book.erase(book.begin() + static_cast<long>(i));
    }
  }
}

LiquidationEngine::AccountWalkOutcome LiquidationEngine::walkCrossAccount(
    Account& account, SymbolId symbol, double markPrice)
{
  AccountWalkOutcome result;
  if (account.positionCount() == 0)
  {
    return result;
  }

  // Drive the cross-margin liquidation loop. While the account is
  // underwater (cross-headroom < 0 at the total-notional tier), close
  // the position with the most negative uPnL first; recheck. Stop
  // when the account is solvent or no positions remain.
  while (account.positionCount() > 0)
  {
    const double notional = account.totalNotional();
    const double mm = mmFractionFor(notional);
    const double headroom = account.crossHeadroom(mm);
    if (headroom >= 0.0)
    {
      break;
    }

    // Pick the worst-PnL position (most negative uPnL). If tied,
    // pick the one with the largest absolute notional.
    auto& book = account.positionsMut();
    int worstIdx = -1;
    double worstUpnl = 0.0;
    double worstNotional = 0.0;
    for (size_t i = 0; i < book.size(); ++i)
    {
      const auto& p = book[i];
      const double mark = account.markFor(p.symbol);
      const double px = mark > 0.0 ? mark : p.entryPrice;
      const double upnl = p.quantity * (px - p.entryPrice);
      const double n = std::abs(p.quantity) * px;
      const bool better = (worstIdx < 0) ||
                          (upnl < worstUpnl) ||
                          (upnl == worstUpnl && n > worstNotional);
      if (better)
      {
        worstIdx = static_cast<int>(i);
        worstUpnl = upnl;
        worstNotional = n;
      }
    }
    if (worstIdx < 0)
    {
      break;
    }

    LeveragedPosition victim = book[static_cast<size_t>(worstIdx)];
    const double victimMark = account.markFor(victim.symbol) > 0.0
                                  ? account.markFor(victim.symbol)
                                  : markPrice;
    double closePrice = 0.0;
    double filledQty = 0.0;
    if (_executor != nullptr)
    {
      const auto exClose = closeThroughExecutor(victim, victimMark);
      closePrice = exClose.closePrice;
      filledQty = exClose.filledQty;
      if (filledQty <= 0.0)
      {
        // Executor couldn't fill (empty book). Stop trying this
        // tick; caller retries on the next mark.
        break;
      }
    }
    else
    {
      const double signQ = (victim.quantity > 0.0) ? 1.0 : -1.0;
      const double slip = _slippageBps / 10000.0;
      closePrice = victimMark * (1.0 - signQ * slip);
      filledQty = std::abs(victim.quantity);
    }

    const double signedFilled = (victim.quantity > 0.0) ? filledQty : -filledQty;
    const double realized = signedFilled * (closePrice - victim.entryPrice);
    account.addEquity(realized);
    result.outcome.liquidated.push_back(account.accountId());
    ++_statLiquidations;
    ++result.outcome.liquidationsCount;

    auto& mut = account.positionsMut();
    const size_t idx = static_cast<size_t>(worstIdx);
    if (filledQty >= std::abs(victim.quantity) - 1e-12)
    {
      mut.erase(mut.begin() + static_cast<long>(idx));
    }
    else
    {
      const double remaining = std::abs(victim.quantity) - filledQty;
      mut[idx].quantity = (victim.quantity > 0.0) ? remaining : -remaining;
    }
  }

  // Any negative equity remaining is the account's contribution to
  // the deficit pool that the caller routes through insurance + ADL.
  if (account.equity() < 0.0)
  {
    result.deficit = -account.equity();
    account.setEquity(0.0);
  }
  return result;
}

LiquidationEngine::AccountWalkOutcome LiquidationEngine::walkIsolatedAccount(
    Account& account, SymbolId symbol, double markPrice)
{
  AccountWalkOutcome result;
  if (account.positionCount() == 0)
  {
    return result;
  }

  // Mirror the per-position logic from `onMarkOnce` but iterate the
  // account's own position book. Each position carries its own
  // isolated equity slice; underwater positions liquidate
  // independently.
  auto& book = account.positionsMut();
  std::vector<size_t> liquidated;
  for (size_t i = 0; i < book.size(); ++i)
  {
    auto& p = book[i];
    if (p.symbol != symbol || p.quantity == 0.0)
    {
      continue;
    }
    const double notional = std::abs(p.quantity) * markPrice;
    const double mm = mmFractionFor(notional);
    const double mmReq = notional * mm;
    const double upnl = p.quantity * (markPrice - p.entryPrice);
    if (p.equity + upnl >= mmReq)
    {
      continue;  // healthy
    }

    // Underwater: close through executor (when attached) or via
    // flat-bps slippage on the bankruptcy price.
    double closePrice = 0.0;
    double filledQty = 0.0;
    if (_executor != nullptr)
    {
      const auto exClose = closeThroughExecutor(p, markPrice);
      closePrice = exClose.closePrice;
      filledQty = exClose.filledQty;
      if (filledQty <= 0.0)
      {
        // Executor couldn't fill; retry next tick. Do not count as
        // a liquidation.
        continue;
      }
    }
    else
    {
      const double signQ = (p.quantity > 0.0) ? 1.0 : -1.0;
      const double slip = _slippageBps / 10000.0;
      closePrice = markPrice * (1.0 - signQ * slip);
      filledQty = std::abs(p.quantity);
    }
    const double signedFilled = (p.quantity > 0.0) ? filledQty : -filledQty;
    const double realized = signedFilled * (closePrice - p.entryPrice);
    const double filledEquity = p.equity * (filledQty / std::abs(p.quantity));
    const double residualEquity = filledEquity + realized;
    result.outcome.liquidated.push_back(account.accountId());
    ++_statLiquidations;
    ++result.outcome.liquidationsCount;
    if (residualEquity < 0.0)
    {
      result.deficit += -residualEquity;
    }
    if (filledQty >= std::abs(p.quantity) - 1e-12)
    {
      liquidated.push_back(i);
    }
    else
    {
      const double remaining = std::abs(p.quantity) - filledQty;
      p.quantity = (p.quantity > 0.0) ? remaining : -remaining;
      p.equity -= filledEquity;
    }
  }
  // Remove fully-liquidated positions descending so erase indexes
  // stay valid.
  std::sort(liquidated.begin(), liquidated.end(), std::greater<size_t>());
  for (size_t i : liquidated)
  {
    book.erase(book.begin() + static_cast<long>(i));
  }
  return result;
}

LiquidationEngine::ExecutorClose LiquidationEngine::closeThroughExecutor(
    const LeveragedPosition& p, double markPrice)
{
  ExecutorClose out;
  if (_executor == nullptr || p.quantity == 0.0)
  {
    return out;
  }
  // Snapshot fill count before submitting; new fills above this
  // index are attributable to the liquidation order.
  const size_t fillsBefore = _executor->fills().size();
  Order order;
  order.id = nextLiquidationOrderId();
  order.symbol = p.symbol;
  // Side that closes the position.
  order.side = (p.quantity > 0.0) ? Side::SELL : Side::BUY;
  order.type = OrderType::MARKET;
  order.quantity = Quantity::fromDouble(std::abs(p.quantity));
  order.price = Price::fromDouble(markPrice);
  _executor->submitOrder(order);
  const auto& fills = _executor->fills();
  if (fills.size() <= fillsBefore)
  {
    return out;  // executor didn't fill anything (empty book)
  }
  // Aggregate fills attributable to this liquidation order.
  double totalQty = 0.0;
  double notional = 0.0;
  for (size_t i = fillsBefore; i < fills.size(); ++i)
  {
    const auto& f = fills[i];
    if (f.orderId != order.id)
    {
      continue;
    }
    const double q = f.quantity.toDouble();
    const double px = f.price.toDouble();
    totalQty += q;
    notional += q * px;
  }
  if (totalQty <= 0.0)
  {
    return out;
  }
  out.filledQty = totalQty;
  out.closePrice = notional / totalQty;
  return out;
}

LiquidationEngine LiquidationEngine::binance_um_futures()
{
  LiquidationEngine e;
  // Binance UM USDT-margined futures, BTCUSDT bracket ladder (sampled).
  e.addTier(0.0, 0.004);
  e.addTier(50'000.0, 0.005);
  e.addTier(250'000.0, 0.01);
  e.addTier(1'000'000.0, 0.025);
  e.addTier(10'000'000.0, 0.05);
  e.addTier(20'000'000.0, 0.10);
  e.setInsuranceFundCapital(900'000'000.0);  // approximate Binance fund
  e.setLiquidationSlippageBps(15.0);
  e.setAdlEnabled(true);
  e.setAdlRanking(AdlRanking::Binance);
  return e;
}

LiquidationEngine LiquidationEngine::bybit_linear()
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.addTier(100'000.0, 0.01);
  e.addTier(500'000.0, 0.025);
  e.addTier(2'500'000.0, 0.05);
  e.setInsuranceFundCapital(100'000'000.0);
  e.setLiquidationSlippageBps(20.0);
  e.setAdlEnabled(true);
  e.setAdlRanking(AdlRanking::Bybit);
  return e;
}

LiquidationEngine LiquidationEngine::okx_swap()
{
  LiquidationEngine e;
  e.addTier(0.0, 0.005);
  e.addTier(50'000.0, 0.0075);
  e.addTier(500'000.0, 0.02);
  e.setInsuranceFundCapital(150'000'000.0);
  e.setLiquidationSlippageBps(20.0);
  e.setAdlEnabled(true);
  e.setAdlRanking(AdlRanking::PnlRatio);
  return e;
}

}  // namespace flox
