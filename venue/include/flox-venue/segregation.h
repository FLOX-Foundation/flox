/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/ledger.h"

#include <cstdint>
#include <unordered_set>
#include <utility>

namespace flox::venue
{

class SegregationReport
{
 public:
  SegregationReport(const Ledger& led, std::unordered_set<uint64_t> houseAccounts)
      : led_(led), house_(std::move(houseAccounts))
  {
  }

  // Segregated client money owed for `asset`: the sum of each client's POSITIVE
  // balance. A client's debit (negative) balance is the firm's receivable from
  // that client, NOT a reduction of what the firm owes other clients -- netting
  // it in (as a plain signed sum would) understates the segregation requirement
  // and lets a real custody shortfall report as fully backed. A negative client
  // wallet is reachable (funding can drive `available` negative on an account
  // whose position margin keeps it unliquidated).
  Amount clientTotal(AssetId asset) const
  {
    Amount t = 0;
    led_.forEachBalance([&](uint64_t acct, AssetId a, Amount total)
                        { if (a == asset && !house_.count(acct) && total > 0){ t += total;
} });
    return t;
  }

  // House (operational + insurance) money for `asset`.
  Amount houseTotal(AssetId asset) const
  {
    Amount t = 0;
    led_.forEachBalance([&](uint64_t acct, AssetId a, Amount total)
                        { if (a == asset && house_.count(acct)){ t += total;
} });
    return t;
  }

  // Client money must be fully covered by the segregated custody balance.
  bool fullyBacked(AssetId asset, Amount custodyBalance) const
  {
    return custodyBalance >= clientTotal(asset);
  }

  // Regulatory shortfall: how much custody falls short of client money owed
  // (0 when fully backed). A positive value is a reportable breach.
  Amount shortfall(AssetId asset, Amount custodyBalance) const
  {
    const Amount owed = clientTotal(asset);
    return custodyBalance < owed ? owed - custodyBalance : 0;
  }

 private:
  const Ledger& led_;
  std::unordered_set<uint64_t> house_;
};

}  // namespace flox::venue
