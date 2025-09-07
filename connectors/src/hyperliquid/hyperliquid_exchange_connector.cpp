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

#include <flox/log/log.h>

#include <simdjson.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>

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

  _wsClient = std::make_unique<IxWebSocketClient>(config.wsEndpoint, "https://app.hyperliquid.xyz",
                                                  config.reconnectDelayMs, _logger.get());
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

        for (const auto& coin : _config.symbols)
        {
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
        }
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
}

void HyperliquidExchangeConnector::stop()
{
  if (!_running.exchange(false))
  {
    return;
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
  static thread_local simdjson::ondemand::parser parser;
  static thread_local std::array<char, 16384> buf;

  if (payload.size() + simdjson::SIMDJSON_PADDING > buf.size())
  {
    return;
  }

  std::memcpy(buf.data(), payload.data(), payload.size());

  try
  {
    auto doc = parser.iterate(buf.data(), payload.size(), buf.size());
    auto chField = doc["channel"];
    if (chField.error())
    {
      return;
    }

    std::string_view channel = chField.get_string().value();
    auto dataField = doc["data"];
    if (dataField.error())
    {
      return;
    }
    auto data = dataField.value();

    if (channel == "l2Book")
    {
      auto evOpt = _bookPool.acquire();
      if (!evOpt)
      {
        return;
      }
      auto& ev = *evOpt;

      auto coinRes = data["coin"].get_string();
      if (coinRes.error())
      {
        return;
      }

      SymbolId sid = resolveSymbolId(coinRes.value_unsafe());

      ev->update.symbol = sid;
      ev->update.type = BookUpdateType::SNAPSHOT;

      auto levelsRes = data["levels"].get_array();
      if (levelsRes.error())
      {
        return;
      }

      auto levels = levelsRes.value();
      auto bidsNode = levels.at(0);
      if (!bidsNode.error())
      {
        auto bidsArr = bidsNode.get_array();
        if (!bidsArr.error())
        {
          for (auto lvl : bidsArr.value())
          {
            auto px = lvl["px"].get_string();
            auto sz = lvl["sz"].get_string();
            if (!px.error() && !sz.error())
            {
              ev->update.bids.emplace_back(
                  Price::fromDouble(std::strtod(px.value_unsafe().data(), nullptr)),
                  Quantity::fromDouble(std::strtod(sz.value_unsafe().data(), nullptr)));
            }
          }
        }
      }

      auto asksNode = levels.at(1);
      if (!asksNode.error())
      {
        auto asksArr = asksNode.get_array();
        if (!asksArr.error())
        {
          for (auto lvl : asksArr.value())
          {
            auto px = lvl["px"].get_string();
            auto sz = lvl["sz"].get_string();
            if (!px.error() && !sz.error())
            {
              ev->update.asks.emplace_back(
                  Price::fromDouble(std::strtod(px.value_unsafe().data(), nullptr)),
                  Quantity::fromDouble(std::strtod(sz.value_unsafe().data(), nullptr)));
            }
          }
        }
      }

      if (!ev->update.bids.empty() || !ev->update.asks.empty())
      {
        _bookBus->publish(std::move(ev));
      }
    }
    else if (channel == "trades")
    {
      auto arrRes = data.get_array();
      if (arrRes.error())
      {
        return;
      }

      for (auto t : arrRes.value())
      {
        auto coinRes = t["coin"].get_string();
        auto pxRes = t["px"].get_string();
        auto szRes = t["sz"].get_string();
        auto sideRes = t["side"].get_string();

        if (coinRes.error() || pxRes.error() || szRes.error() || sideRes.error())
        {
          continue;
        }

        SymbolId sid = resolveSymbolId(coinRes.value_unsafe());

        TradeEvent ev;
        ev.trade.symbol = sid;
        ev.trade.price = Price::fromDouble(std::strtod(pxRes.value_unsafe().data(), nullptr));
        ev.trade.quantity = Quantity::fromDouble(std::strtod(szRes.value_unsafe().data(), nullptr));
        ev.trade.isBuy = sideRes.value_unsafe() == "buy";
        ev.trade.exchangeTsNs = nowNsMonotonic();

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
