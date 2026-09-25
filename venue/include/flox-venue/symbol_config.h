/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/messages.h"

// The instrument's configuration, in a header of its own.
//
// It used to sit at the top of matching_engine.h, which was fine while the
// engine was the only reader. The engine's components (engine/*.h) are plain
// classes the engine owns, and each of them holds the SAME config by
// reference -- an operator changing the maintenance margin has to be visible
// to clearing without a second copy to keep in step. A component including
// matching_engine.h to reach the struct would be a cycle, so the struct
// moved down here instead: matching_engine.h includes this, and so does
// every component.
//
// Nothing about the struct changed; it is the same fields in the same order.

namespace flox::venue
{

// TriggerRef (the conditional-order reference price selector) lives in
// messages.h next to the SetTriggerRef command that carries it.

struct SymbolConfig
{
  SymbolId id{};
  Price tickSize{};           // 0 = unchecked
  Quantity lotSize{};         // 0 = unchecked
  Quantity minQty{};          // 0 = unchecked
  Price minPrice{};           // 0 = unchecked (price band / collar lower bound)
  Price maxPrice{};           // 0 = unchecked (price band / collar upper bound)
  Quantity maxOrderQty{};     // 0 = unchecked (fat-finger max size)
  Volume maxOrderNotional{};  // 0 = unchecked (fat-finger max notional, limit orders)
  bool halted{false};
  TriggerRef triggerRef{TriggerRef::Last};
  DurationNs lastLookWindowNs{};  // 0 = last look disabled venue-wide
  // How long a client order id stays reserved against reuse. 0 = forever,
  // which is what the venue always did and what it still does unless an
  // operator says otherwise. Past the window an old id is accepted again --
  // exchanges scope client order id uniqueness to the trading day, so that is
  // the honest bound, and it has to be stated rather than discovered.
  int64_t clOrdIdWindowNs{0};
  bool lastLookAcceptOnTimeout{false};  // window elapses with no decision -> accept vs reject
  // Symmetric price tolerance. When set, the VENUE decides whether the price
  // moved too far during the hold, and it applies the same threshold in both
  // directions: outside it, the fill is rejected whoever it would have
  // favoured.
  //
  // This is what removes the free option. A maker allowed to answer a held
  // fill however it likes will, over enough samples, fill the ones that moved
  // its way and refuse the ones that did not -- and that asymmetry is
  // invisible to the taker, who sees only a reject rate. Enforcing magnitude
  // at the venue leaves nothing to be asymmetric about outside the band, and
  // lastLookStats makes what happens inside it visible.
  //
  // 0 = no venue-side check: the maker's answer stands whatever the price did.
  int64_t lastLookToleranceRaw{0};
  AssetId baseAsset{0};             // e.g. BTC in BTC-USD (settled to the seller/buyer)
  AssetId quoteAsset{1};            // e.g. USD in BTC-USD
  int32_t luldBps{0};               // limit-up/limit-down band around the reference (0 = off)
  DurationNs luldHaltNs{};          // trading pause LENGTH on a band breach
  bool linearPerp{false};           // derivatives: linear perpetual (margin, no asset delivery)
  int32_t initialMarginBps{0};      // IM as bps of notional (1000 = 10% = 10x leverage)
  int32_t maintenanceMarginBps{0};  // MM; position liquidated when equity < MM (0 = off)
  // Liquidation decided elsewhere (portfolio margin above the per-symbol
  // engines). The engine still posts isolated IM and settles, but never
  // liquidates on its own -- it closes only on ForceClosePosition.
  bool externalLiquidation{false};
  bool autoDeleverage{false};  // ADL: recover a bankruptcy deficit from winners before insurance
  Quantity maxPositionQty{};   // 0 = unchecked (max |position| per account, perp risk cap)
  uint32_t maxOpenOrders{0};   // 0 = unchecked (max live resting orders per account)

  // How a price level is allocated between the makers resting on it.
  //
  // The engine takes the policy as a constructor argument too, and that
  // argument still wins for a caller that names it. What this field adds is
  // the only route a DEPLOYMENT has: SequencedShard and SymbolRouter build
  // their engine from a SymbolConfig, so a policy that cannot travel on the
  // config cannot reach a journal, a checkpoint or a gateway at all.
  //
  // Not folded into configHash by this field: the hash already folds
  // matcher_.policy(), which is the value the engine actually matches under
  // however it was given. Hashing both would move every existing row for no
  // new information.
  MatchPolicy matchPolicy{MatchPolicy::PriceTimeFifo};

  // Per-symbol fixed-point scale, same semantics as core SymbolInfo. Default
  // 1e8 = the compile-time Price/Quantity scale. Money always settles at
  // kMoneyScale regardless. Must satisfy scalesValid().
  int64_t priceScale{Price::Scale};
  int64_t qtyScale{Quantity::Scale};

  // Startup FALLBACK funding interval (0 = the venue does not fund this
  // instrument). The engine settles funding only when told to (the sequenced
  // ApplyFunding command); this is the calendar it publishes when no schedule
  // has been set -- the next boundary of a fixed-interval grid anchored at the
  // sequencer-ts origin, a function of (now, interval). The AUTHORITATIVE
  // calendar is engine state set by SetFundingSchedule, which overrides this;
  // see MatchingEngine::nextFundingNs. Excluded from configHash for the same
  // reason the other mutable knobs are: it reinterprets no stored number, so it
  // cannot invalidate a snapshot.
  DurationNs fundingIntervalNs{};
};

}  // namespace flox::venue
