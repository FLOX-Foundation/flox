/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/hyperliquid/hyperliquid_exchange_connector.h"
#include "flox-connectors/net/ix_websocket_client.h"
#include "flox-connectors/util/safe_parse.h"

#include <flox/log/log.h>

#include <simdjson.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace flox
{

HyperliquidExchangeConnector::HyperliquidExchangeConnector(const HyperliquidConfig& config,
                                                           BookUpdateBus* bookBus,
                                                           TradeBus* tradeBus,
                                                           SymbolRegistry* symbolRegistry,
                                                           std::shared_ptr<ILogger> logger)
    : _config(config),
      _bookBus(bookBus),
      _tradeBus(tradeBus),
      _registry(symbolRegistry),
      _logger(std::move(logger))
{
  assert(_registry && "symbols registry not set");
  assert(_bookBus && "book bus not set");
  assert(_tradeBus && "trade bus not set");

  // Disable WS protocol ping (pingIntervalSec = 0) - Hyperliquid requires application-level ping
  _wsClient = std::make_unique<IxWebSocketClient>(config.wsEndpoint, "https://app.hyperliquid.xyz",
                                                  config.reconnectDelayMs, _logger.get(), 0);
}

void HyperliquidExchangeConnector::start()
{
  if (_running.exchange(true))
  {
    return;
  }

  if (!_wsClient || !_bookBus || !_tradeBus)
  {
    _logger->warn("[Hyperliquid] missing client or buses");
    return;
  }

  _wsClient->onOpen(
      [this]()
      {
        FLOX_LOG("[Hyperliquid] WS open, sending subscriptions");

        // Send subscriptions in batches with small delays to avoid overwhelming the server
        // Hyperliquid seems to have issues when >40 messages are sent rapidly
        constexpr size_t BATCH_SIZE = 10;  // 5 symbols worth of subscriptions per batch

        for (size_t i = 0; i < _config.symbols.size(); ++i)
        {
          const auto& coin = _config.symbols[i];
          std::array<char, 128> buf{};

          int n = std::snprintf(
              buf.data(), buf.size(),
              R"({"method":"subscribe","subscription":{"type":"l2Book","coin":"%.*s"}})",
              static_cast<int>(coin.size()), coin.c_str());
          if (n > 0 && n < static_cast<int>(buf.size()))
          {
            _wsClient->send({buf.data(), static_cast<size_t>(n)});
          }

          n = std::snprintf(
              buf.data(), buf.size(),
              R"({"method":"subscribe","subscription":{"type":"trades","coin":"%.*s"}})",
              static_cast<int>(coin.size()), coin.c_str());
          if (n > 0 && n < static_cast<int>(buf.size()))
          {
            _wsClient->send({buf.data(), static_cast<size_t>(n)});
          }

          // Add small delay after each batch of 5 symbols (10 subscriptions)
          if ((i + 1) % 5 == 0 && i + 1 < _config.symbols.size())
          {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
        }

        FLOX_LOG("[Hyperliquid] Subscribed to " << _config.symbols.size() << " symbols");
      });

  _wsClient->onClose(
      [this](uint16_t code, std::string_view reason)
      {
        FLOX_LOG("[Hyperliquid] WS close code=" << code << " reason=\"" << reason << '"');
      });

  _wsClient->onMessage(
      [this](std::string_view payload)
      {
        handleMessage(payload);
      });

  _wsClient->start();

  // Start ping thread to keep connection alive
  _pingThread = std::thread(&HyperliquidExchangeConnector::pingLoop, this);
}

void HyperliquidExchangeConnector::pingLoop()
{
  // Initial delay to let connection establish before starting ping loop
  for (int i = 0; i < 50 && _running.load(); ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  while (_running.load())
  {
    if (_wsClient)
    {
      _wsClient->send(R"({"method":"ping"})");
    }
    // Send heartbeat every 30 seconds (Hyperliquid timeout is 60 seconds)
    // Sleep in small intervals to allow quick shutdown
    for (int i = 0; i < 300 && _running.load(); ++i)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void HyperliquidExchangeConnector::stop()
{
  if (!_running.exchange(false))
  {
    return;
  }

  if (_pingThread.joinable())
  {
    _pingThread.join();
  }

  if (_wsClient)
  {
    _wsClient->stop();
  }
}

SymbolId HyperliquidExchangeConnector::resolveSymbolId(std::string_view symbol)
{
  if (auto existing = _registry->getSymbolId("hyperliquid", std::string(symbol)))
  {
    return *existing;
  }

  SymbolInfo info;
  info.exchange = "hyperliquid";
  info.symbol = std::string(symbol);
  info.type = InstrumentType::Future;
  return _registry->registerSymbol(info);
}

void HyperliquidExchangeConnector::handleMessage(std::string_view payload)
{
  static thread_local simdjson::dom::parser parser;

  try
  {
    auto doc = parser.parse(payload);

    auto channelEl = doc["channel"];
    if (channelEl.error())
    {
      return;
    }

    std::string_view channel = channelEl.get_string().value();
    auto dataEl = doc["data"];
    if (dataEl.error())
    {
      return;
    }

    if (channel == "l2Book")
    {
      auto evOpt = _bookPool.acquire();
      if (!evOpt)
      {
        return;
      }
      auto& ev = *evOpt;

      auto coinEl = dataEl["coin"];
      if (coinEl.error())
      {
        return;
      }

      SymbolId sid = resolveSymbolId(coinEl.get_string().value());

      ev->update.symbol = sid;
      // Hyperliquid sends full book snapshots on each update
      ev->update.type = BookUpdateType::SNAPSHOT;

      // Parse timestamp (milliseconds)
      auto timeEl = dataEl["time"];
      if (!timeEl.error())
      {
        int64_t tsMs = timeEl.get_int64().value();
        ev->update.exchangeTsNs = tsMs * 1'000'000;
      }

      auto levelsEl = dataEl["levels"];
      if (levelsEl.error())
      {
        return;
      }

      auto levels = levelsEl.get_array();
      size_t idx = 0;
      for (auto levelArr : levels)
      {
        for (auto lvl : levelArr.get_array())
        {
          auto pxEl = lvl["px"];
          auto szEl = lvl["sz"];
          if (pxEl.error() || szEl.error())
          {
            continue;
          }

          auto priceOpt = util::safeParseDouble(pxEl.get_string().value());
          auto qtyOpt = util::safeParseDouble(szEl.get_string().value());
          if (!priceOpt || !qtyOpt)
          {
            _logger->warn("[Hyperliquid] Invalid price/qty in book level");
            continue;
          }

          Price price = Price::fromDouble(*priceOpt);
          Quantity qty = Quantity::fromDouble(*qtyOpt);

          if (idx == 0)
          {
            ev->update.bids.emplace_back(price, qty);
          }
          else
          {
            ev->update.asks.emplace_back(price, qty);
          }
        }
        idx++;
      }

      if (!ev->update.bids.empty() || !ev->update.asks.empty())
      {
        _bookBus->publish(std::move(ev));
      }
    }
    else if (channel == "trades")
    {
      auto arr = dataEl.get_array();

      for (auto t : arr)
      {
        auto coinEl = t["coin"];
        auto pxEl = t["px"];
        auto szEl = t["sz"];
        auto sideEl = t["side"];

        if (coinEl.error() || pxEl.error() || szEl.error() || sideEl.error())
        {
          continue;
        }

        auto priceOpt = util::safeParseDouble(pxEl.get_string().value());
        auto qtyOpt = util::safeParseDouble(szEl.get_string().value());
        if (!priceOpt || !qtyOpt)
        {
          _logger->warn("[Hyperliquid] Invalid trade price/qty");
          continue;
        }

        SymbolId sid = resolveSymbolId(coinEl.get_string().value());

        TradeEvent ev;
        ev.trade.symbol = sid;
        ev.trade.price = Price::fromDouble(*priceOpt);
        ev.trade.quantity = Quantity::fromDouble(*qtyOpt);
        std::string_view side = sideEl.get_string().value();
        ev.trade.isBuy = (side == "B" || side == "buy");

        // Parse timestamp (milliseconds)
        auto timeEl = t["time"];
        if (!timeEl.error())
        {
          int64_t tsMs = timeEl.get_int64().value();
          ev.trade.exchangeTsNs = tsMs * 1'000'000;
        }

        if (const auto info = _registry->getSymbolInfo(sid))
        {
          ev.trade.instrument = info->type;
        }

        _tradeBus->publish(ev);
      }
    }
  }
  catch (const simdjson::simdjson_error& e)
  {
    _logger->warn(std::string("[Hyperliquid] simdjson error: ") + e.what());
  }
}

}  // namespace flox
