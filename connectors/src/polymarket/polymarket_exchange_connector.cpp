/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/polymarket/polymarket_exchange_connector.h"
#include "flox-connectors/net/ix_websocket_client.h"
#include "flox-connectors/util/safe_parse.h"

#include <flox/log/log.h>
#include <flox/util/base/hash.h>

#include <simdjson.h>

#include <algorithm>
#include <sstream>

namespace flox
{

static constexpr auto POLYMARKET_ORIGIN = "https://polymarket.com";

PolymarketExchangeConnector::PolymarketExchangeConnector(const PolymarketConfig& config,
                                                         BookUpdateBus* bookUpdateBus,
                                                         TradeBus* tradeBus,
                                                         SymbolRegistry* registry,
                                                         std::shared_ptr<ILogger> logger)
    : _config(config),
      _bookUpdateBus(bookUpdateBus),
      _tradeBus(tradeBus),
      _registry(registry),
      _logger(std::move(logger))
{
}

void PolymarketExchangeConnector::start()
{
  if (!_config.isValid())
  {
    if (_logger)
    {
      _logger->error("[Polymarket] Invalid connector config");
    }
    return;
  }

  if (_running.exchange(true))
  {
    return;
  }

  _wsMarket = std::make_unique<IxWebSocketClient>(_config.wsEndpoint, POLYMARKET_ORIGIN,
                                                  _config.reconnectDelayMs, _logger.get(),
                                                  _config.pingIntervalSec);

  _wsMarket->onOpen(
      [this]()
      {
        if (_logger)
        {
          _logger->info("[Polymarket] WebSocket connected");
        }

        if (!_config.tokenIds.empty())
        {
          sendSubscribe(_config.tokenIds, "subscribe");
        }
      });

  _wsMarket->onMessage(
      [this](std::string_view payload)
      {
        try
        {
          handleMessage(payload);
        }
        catch (const std::exception& e)
        {
          if (_logger)
          {
            _logger->error(std::string("[Polymarket] Exception: ") + e.what());
          }
        }
      });

  _wsMarket->onClose(
      [this](int code, std::string_view reason)
      {
        if (_logger)
        {
          _logger->info("[Polymarket] WebSocket closed: code=" + std::to_string(code) +
                        ", reason=" + std::string(reason));
        }
      });

  _wsMarket->start();

  if (_logger)
  {
    _logger->info("[Polymarket] Connector started");
  }
}

void PolymarketExchangeConnector::stop()
{
  if (!_running.exchange(false))
  {
    return;
  }

  if (_wsMarket)
  {
    _wsMarket->stop();
    _wsMarket.reset();
  }

  if (_logger)
  {
    _logger->info("[Polymarket] Connector stopped");
  }
}

SymbolId PolymarketExchangeConnector::resolveSymbolId(std::string_view tokenId)
{
  std::string key(tokenId);

  auto it = _tokenToSymbol.find(key);
  if (it != _tokenToSymbol.end())
  {
    return it->second;
  }

  if (_registry)
  {
    auto existing = _registry->getSymbolId("polymarket", key);
    if (existing)
    {
      _tokenToSymbol[key] = *existing;
      return *existing;
    }

    SymbolInfo info;
    info.exchange = "polymarket";
    info.symbol = key;
    info.type = InstrumentType::Spot;  // TODO: Add PredictionMarket type

    SymbolId id = _registry->registerSymbol(info);
    _tokenToSymbol[key] = id;
    return id;
  }

  SymbolId id = static_cast<SymbolId>(hash::fnv1a_64(tokenId.data(), tokenId.size()) & 0xFFFFFFFF);
  _tokenToSymbol[key] = id;
  return id;
}

// Helper to parse value that can be string or number
static std::optional<double> parseStringOrDouble(simdjson::ondemand::value val)
{
  // Try as string first
  auto strResult = val.get_string();
  if (!strResult.error())
  {
    return util::safeParseDouble(strResult.value());
  }

  // Try as double
  auto dblResult = val.get_double();
  if (!dblResult.error())
  {
    return dblResult.value();
  }

  // Try as int64
  auto intResult = val.get_int64();
  if (!intResult.error())
  {
    return static_cast<double>(intResult.value());
  }

  return std::nullopt;
}

void PolymarketExchangeConnector::handleMessage(std::string_view payload)
{
  static thread_local simdjson::ondemand::parser parser;
  const uint64_t recvNs = nowNsMonotonic();

  try
  {
    simdjson::padded_string padded(payload);
    auto doc = parser.iterate(padded);

    // Check first character to determine message type
    if (!payload.empty() && payload[0] == '[')
    {
      // Initial snapshot - array of book snapshots
      for (auto item : doc.get_array())
      {
        processBookSnapshot(item.get_object().value(), recvNs);
      }
      return;
    }

    auto obj = doc.get_object().value();

    // Check for price_changes (incremental update)
    if (auto pc = obj["price_changes"]; !pc.error())
    {
      // Incremental updates - for now just ignore them
      // Full book updates will come via "book" event_type
      return;
    }

    // Check for event_type
    auto eventTypeField = obj["event_type"];
    if (eventTypeField.error())
    {
      return;
    }

    std::string_view eventType = eventTypeField.get_string().value();

    if (eventType == "book")
    {
      processBookSnapshot(std::move(obj), recvNs);
    }
    else if (eventType == "last_trade_price" || eventType == "trade")
    {
      auto assetIdField = obj["asset_id"];
      if (assetIdField.error())
      {
        return;
      }

      std::string_view tokenId = assetIdField.get_string().value();
      SymbolId sym = resolveSymbolId(tokenId);

      TradeEvent ev{};
      ev.recvNs = recvNs;
      ev.trade.symbol = sym;

      auto priceField = obj["price"];
      auto sizeField = obj["size"];
      auto sideField = obj["side"];

      if (!priceField.error() && !sizeField.error())
      {
        auto priceOpt = parseStringOrDouble(priceField.value());
        auto sizeOpt = parseStringOrDouble(sizeField.value());

        if (priceOpt && sizeOpt)
        {
          ev.trade.price = Price::fromDouble(*priceOpt);
          ev.trade.quantity = Quantity::fromDouble(*sizeOpt);

          if (!sideField.error())
          {
            ev.trade.isBuy = (sideField.get_string().value() == "BUY");
          }

          ev.trade.exchangeTsNs = nowNsMonotonic();
          ev.publishTsNs = nowNsMonotonic();
          _tradeBus->publish(ev);
        }
      }
    }
  }
  catch (const simdjson::simdjson_error& e)
  {
    if (_logger)
    {
      _logger->warn(std::string("[Polymarket] simdjson error: ") + e.what());
    }
  }
}

void PolymarketExchangeConnector::sendSubscribe(const std::vector<std::string>& tokenIds,
                                                const std::string& operation)
{
  if (!_wsMarket || tokenIds.empty())
  {
    return;
  }

  std::ostringstream json;
  json << R"({"assets_ids":[)";
  for (size_t i = 0; i < tokenIds.size(); ++i)
  {
    if (i > 0)
    {
      json << ",";
    }
    json << "\"" << tokenIds[i] << "\"";
  }
  json << R"(],"type":"market","operation":")" << operation << "\"}";

  _wsMarket->send(json.str());

  if (_logger)
  {
    _logger->info("[Polymarket] Subscribed to " + std::to_string(tokenIds.size()) + " tokens");
  }
}

void PolymarketExchangeConnector::processBookSnapshot(simdjson::ondemand::object obj,
                                                      uint64_t recvNs)
{
  auto assetIdField = obj["asset_id"];
  if (assetIdField.error())
  {
    return;
  }

  std::string_view tokenId = assetIdField.get_string().value();
  SymbolId sym = resolveSymbolId(tokenId);

  auto evOpt = _bookPool.acquire();
  if (!evOpt)
  {
    if (_logger)
    {
      _logger->warn("[Polymarket] Book pool exhausted");
    }
    return;
  }

  auto& ev = *evOpt;
  ev->recvNs = recvNs;
  ev->update.symbol = sym;
  ev->update.bids.clear();
  ev->update.asks.clear();

  // Parse bids
  if (auto bids = obj["bids"]; !bids.error())
  {
    for (auto level : bids.get_array())
    {
      auto lobj = level.get_object();
      double price = 0, size = 0;

      if (auto p = lobj["price"]; !p.error())
      {
        if (auto pOpt = util::safeParseDouble(p.get_string().value()))
        {
          price = *pOpt;
        }
      }
      if (auto s = lobj["size"]; !s.error())
      {
        if (auto sOpt = util::safeParseDouble(s.get_string().value()))
        {
          size = *sOpt;
        }
      }

      if (price > 0 && size > 0)
      {
        ev->update.bids.push_back({Price::fromDouble(price), Quantity::fromDouble(size)});
      }
    }
  }

  // Parse asks
  if (auto asks = obj["asks"]; !asks.error())
  {
    for (auto level : asks.get_array())
    {
      auto lobj = level.get_object();
      double price = 0, size = 0;

      if (auto p = lobj["price"]; !p.error())
      {
        if (auto pOpt = util::safeParseDouble(p.get_string().value()))
        {
          price = *pOpt;
        }
      }
      if (auto s = lobj["size"]; !s.error())
      {
        if (auto sOpt = util::safeParseDouble(s.get_string().value()))
        {
          size = *sOpt;
        }
      }

      if (price > 0 && size > 0)
      {
        ev->update.asks.push_back({Price::fromDouble(price), Quantity::fromDouble(size)});
      }
    }
  }

  ev->update.exchangeTsNs = nowNsMonotonic();
  ev->publishTsNs = nowNsMonotonic();

  _bookUpdateBus->publish(std::move(ev));
}

}  // namespace flox
