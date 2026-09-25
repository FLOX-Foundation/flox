/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The engine's timebase, and the config fields nobody reads.
//
// fromUnixMs()/fromUnixNs() turn a venue's wall-clock stamp into a
// FloxClock (steady_clock) TimePoint through unix_to_flox_offset_ns(), a
// process-global atomic that starts at zero. At zero the conversion is not
// approximately right, it is the exact bug the typed clocks were introduced
// to stop: a unix epoch relabelled as a steady-clock reading, decades adrift.
// Only init_timebase_mapping() anchors the offset, and nothing on the live
// C++ startup path calls it -- one connector does it lazily for itself, in a
// call_once of its own, and every other connector, aggregator and bar policy
// that converts an exchange timestamp gets the zero offset.
//
// The anchoring belongs to the engine, before anything it owns runs, so that
// no component has to remember it and none of them can disagree about when
// it happened.

#include "flox/connector/abstract_exchange_connector.h"
#include "flox/engine/abstract_subsystem.h"
#include "flox/engine/engine.h"
#include "flox/engine/engine_config.h"
#include "flox/util/base/time.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace flox;

namespace
{

int64_t nowUnixMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Restores the process-global offset, so one test cannot decide another's
// result through it.
class OffsetGuard
{
 public:
  OffsetGuard() : _saved(unix_to_flox_offset_ns().load(std::memory_order_relaxed)) {}
  ~OffsetGuard() { unix_to_flox_offset_ns().store(_saved, std::memory_order_relaxed); }

  static void reset() { unix_to_flox_offset_ns().store(0, std::memory_order_relaxed); }

 private:
  int64_t _saved;
};

// Records the offset it saw when the engine started it. A connector converts
// venue timestamps from its very first message, so the mapping has to be
// anchored before start() is called, not merely at some point during it.
class OffsetWatchingConnector : public IExchangeConnector
{
 public:
  std::string exchangeId() const override { return "offset-watcher"; }

  void start() override
  {
    started = true;
    offsetAtStart = unix_to_flox_offset_ns().load(std::memory_order_relaxed);
    mappedAtStart = fromUnixMs(nowUnixMs());
  }

  bool started{false};
  int64_t offsetAtStart{0};
  TimePoint mappedAtStart{};
};

class OffsetWatchingSubsystem : public ISubsystem
{
 public:
  void start() override
  {
    started = true;
    offsetAtStart = unix_to_flox_offset_ns().load(std::memory_order_relaxed);
  }

  bool started{false};
  int64_t offsetAtStart{0};
};

Engine makeEngine(const EngineConfig& cfg,
                  std::vector<std::unique_ptr<ISubsystem>> subsystems = {},
                  std::vector<std::shared_ptr<IExchangeConnector>> connectors = {})
{
  return Engine(cfg, std::move(subsystems), std::move(connectors));
}

}  // namespace

// The green control for this area: the conversion itself is sound once the
// offset is anchored by hand. Only the engine's silence is under test below.
TEST(EngineTimebase, AnchoredMappingCarriesAKnownInstantBothWays)
{
  OffsetGuard guard;
  init_timebase_mapping();

  const int64_t ms = nowUnixMs();
  const TimePoint fromMs = fromUnixMs(ms);
  const TimePoint fromNs = fromUnixNs(ms * kNsPerMs);
  EXPECT_EQ(fromMs, fromNs) << "the two entry points must agree on one instant";

  const auto driftMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(fromMs - now()).count();
  EXPECT_LT(std::llabs(driftMs), 1000);
}

TEST(EngineTimebase, StartAnchorsTheUnixToFloxMapping)
{
  OffsetGuard guard;
  OffsetGuard::reset();

  EngineConfig cfg;
  Engine engine = makeEngine(cfg);
  engine.start();

  EXPECT_NE(unix_to_flox_offset_ns().load(std::memory_order_relaxed), 0)
      << "Engine::start() left unix_to_flox_offset_ns() at zero: every "
         "fromUnixMs/fromUnixNs conversion in the process reinterprets a "
         "wall-clock epoch as a steady-clock reading";

  const TimePoint mapped = fromUnixMs(nowUnixMs());
  const auto driftMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(mapped - now()).count();
  EXPECT_LT(std::llabs(driftMs), 1000)
      << "a wall-clock instant of now(), mapped after Engine::start(), landed "
      << driftMs << " ms from FloxClock::now()";

  engine.stop();
}

// The mapping has to exist before the components that convert timestamps do,
// otherwise a connector's first messages carry the zero-offset answer and the
// rest carry the anchored one -- a tape whose timestamps change meaning
// mid-file is worse than one that is uniformly wrong.
TEST(EngineTimebase, MappingIsAnchoredBeforeSubsystemsAndConnectorsStart)
{
  OffsetGuard guard;
  OffsetGuard::reset();

  auto subsystem = std::make_unique<OffsetWatchingSubsystem>();
  auto* subsystemRaw = subsystem.get();
  auto connector = std::make_shared<OffsetWatchingConnector>();

  std::vector<std::unique_ptr<ISubsystem>> subsystems;
  subsystems.push_back(std::move(subsystem));
  std::vector<std::shared_ptr<IExchangeConnector>> connectors{connector};

  EngineConfig cfg;
  Engine engine = makeEngine(cfg, std::move(subsystems), std::move(connectors));
  engine.start();

  ASSERT_TRUE(subsystemRaw->started);
  ASSERT_TRUE(connector->started);

  EXPECT_NE(subsystemRaw->offsetAtStart, 0)
      << "a subsystem was started with the timebase still unanchored";
  EXPECT_NE(connector->offsetAtStart, 0)
      << "a connector was started with the timebase still unanchored, so its "
         "first converted venue timestamps are decades adrift and the later "
         "ones are not";

  const auto driftMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           connector->mappedAtStart - now())
                           .count();
  EXPECT_LT(std::llabs(driftMs), 1000);

  engine.stop();
}

// A connector must not have to anchor the timebase for itself. The one that
// does today wraps init_timebase_mapping() in its own call_once; with the
// engine anchoring first, that call has to be harmless -- which means
// init_timebase_mapping() is a no-op once the mapping is established, not a
// second reading that silently moves every already-converted timestamp
// relative to the ones that follow.
TEST(EngineTimebase, AConnectorNeedNotAnchorTheMappingItself)
{
  OffsetGuard guard;
  OffsetGuard::reset();

  EngineConfig cfg;
  Engine engine = makeEngine(cfg);
  engine.start();

  const int64_t anchored = unix_to_flox_offset_ns().load(std::memory_order_relaxed);
  ASSERT_NE(anchored, 0) << "the engine did not anchor the mapping at all";

  // What a connector's lazy ensureTimebaseMapped() does on first use.
  init_timebase_mapping();

  EXPECT_EQ(unix_to_flox_offset_ns().load(std::memory_order_relaxed), anchored)
      << "a second init_timebase_mapping() moved the offset after the engine "
         "had anchored it; a connector calling it on first use would shift "
         "the meaning of every timestamp converted before that moment";

  engine.stop();
}

// --------------------------------------------------------------- dead config
//
// EngineConfig::exchanges, ExchangeConfig and SymbolConfig{tickSize,
// expectedDeviation} are read by nothing in the tree: Engine touches only
// memoryProfile and drainTimeoutMs. docs/how-to/configuration.md and
// docs/reference/api/engine/engine_config.md both describe them as live
// startup input -- "During startup, the engine registers symbols" out of
// config.exchanges into a SymbolRegistry, with tickSize feeding order-book
// alignment.
//
// Two honest endings, and this test accepts either:
//
//   used    -- the engine reads config.exchanges at start and the symbols
//              are reachable afterwards through a registry it exposes;
//   removed -- EngineConfig::exchanges (with ExchangeConfig and
//              SymbolConfig) is gone from the header, and the two documents
//              above no longer promise it.
//
// needs, for the "used" ending:
//   const SymbolRegistry& Engine::symbolRegistry() const noexcept;
//   Engine::start() registers every (ExchangeConfig::name,
//   SymbolConfig::symbol) pair in it, with SymbolInfo::tickSize taken from
//   SymbolConfig::tickSize.
//
// The "removed" ending needs no interface: deleting the field makes the
// detector below false and this test passes, and the doc line goes with it.

namespace
{

template <class C>
concept ConfigCarriesExchanges = requires(C c) { c.exchanges; };

template <class E>
concept EngineExposesSymbolRegistry = requires(const E& e) { e.symbolRegistry(); };

// Templated so that the endings this test does not take are never compiled:
// the "used" branch names members that the "removed" ending deletes.
template <class Cfg, class Eng>
void checkExchangesAreReadOrGone()
{
  if constexpr (!ConfigCarriesExchanges<Cfg>)
  {
    SUCCEED() << "EngineConfig::exchanges is gone; the docs that described it "
                 "must go with it";
  }
  else if constexpr (!EngineExposesSymbolRegistry<Eng>)
  {
    FAIL() << "EngineConfig::exchanges / ExchangeConfig / SymbolConfig are "
              "filled in by callers and read by nothing: Engine uses only "
              "memoryProfile and drainTimeoutMs, and there is no way to "
              "observe a configured symbol after start(). Either the engine "
              "registers them (see the needs note above this test) or the "
              "fields and their documentation are removed.";
  }
  else
  {
    OffsetGuard guard;

    Cfg cfg;
    using ExchangeCfg = typename std::decay_t<decltype(cfg.exchanges)>::value_type;
    ExchangeCfg exchange;
    exchange.name = "testvenue";
    exchange.type = "testvenue_futures";

    using SymbolCfg = typename std::decay_t<decltype(exchange.symbols)>::value_type;
    SymbolCfg symbol;
    symbol.symbol = "BTCUSDT";
    symbol.tickSize = 0.5;
    symbol.expectedDeviation = 10.0;
    exchange.symbols.push_back(symbol);
    cfg.exchanges.push_back(exchange);

    Eng engine(cfg, {}, {});
    engine.start();

    const auto& registry = engine.symbolRegistry();
    const auto id = registry.getSymbolId("testvenue", "BTCUSDT");
    ASSERT_TRUE(id.has_value()) << "the configured symbol was not registered at start";

    const auto info = registry.getSymbolInfo(*id);
    ASSERT_TRUE(info.has_value());
    EXPECT_DOUBLE_EQ(info->tickSize.toDouble(), 0.5)
        << "SymbolConfig::tickSize did not reach the registered symbol";

    engine.stop();
  }
}

}  // namespace

TEST(EngineConfigFields, ExchangesAndSymbolsAreEitherReadOrGone)
{
  checkExchangesAreReadOrGone<EngineConfig, Engine>();
}
