/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/bitget/bitget_exchange_connector.h"
#include "flox-connectors/net/ix_websocket_client.h"
#include "flox/engine/symbol_registry.h"

#include <flox/log/log.h>

#include <openssl/hmac.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <simdjson.h>

namespace flox
{
static constexpr auto BITGET_ORIGIN = "https://www.bitget.com";

namespace
{

static std::string makeLoginPayload(std::string_view apiKey, std::string_view apiSecret,
                                    std::string_view passphrase)
{
  using namespace std::chrono;
  auto ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  std::string toSign = std::to_string(ts) + "GET/user/verify";

  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  HMAC(EVP_sha256(), apiSecret.data(), static_cast<int>(apiSecret.size()),
       reinterpret_cast<const unsigned char*>(toSign.data()), toSign.size(), hash, &len);

  char hex[EVP_MAX_MD_SIZE * 2 + 1];
  for (unsigned i = 0; i < len; ++i)
  {
    std::sprintf(hex + i * 2, "%02x", hash[i]);
  }
  hex[len * 2] = '\0';

  std::string payload;
  payload.reserve(200);
  payload.append(R"({"op":"login","args":[{)");
  payload.append("\"apiKey\":\"").append(apiKey).append("\",");
  payload.append("\"passphrase\":\"").append(passphrase).append("\",");
  payload.append("\"timestamp\":\"").append(std::to_string(ts)).append("\",");
  payload.append("\"sign\":\"").append(hex).append("\"}]}");

  return payload;
}

static std::string_view bitgetWsInstType(InstrumentType type)
{
  switch (type)
  {
    case InstrumentType::Spot:
      return "sp";
    case InstrumentType::Future:
      return "mc";
    case InstrumentType::Inverse:
      return "dmc";
    case InstrumentType::Option:
      return "cmc";
  }
  return "unknown";
}

}  // namespace

BitgetExchangeConnector::BitgetExchangeConnector(const BitgetConfig& cfg, BookUpdateBus* bookBus,
                                                 TradeBus* tradeBus, OrderExecutionBus* orderBus,
                                                 SymbolRegistry* registry,
                                                 std::shared_ptr<ILogger> logger)
    : _config(cfg),
      _bookUpdateBus(bookBus),
      _tradeBus(tradeBus),
      _orderBus(orderBus),
      _registry(registry),
      _logger(std::move(logger))
{
  _wsClient = std::make_unique<IxWebSocketClient>(cfg.publicEndpoint, BITGET_ORIGIN,
                                                  cfg.reconnectDelayMs, _logger.get());
}

void BitgetExchangeConnector::start()
{
  if (!_config.isValid())
  {
    _logger->error("[Bitget] Invalid config");
    return;
  }

  if (_running.exchange(true))
  {
    return;
  }

  _wsClient->onOpen(
      [this]()
      {
        std::string sub;
        sub.reserve(64 + _config.symbols.size() * 32);
        sub += R"({"op":"subscribe","args":[)";

        bool first = true;
        for (const auto& s : _config.symbols)
        {
          if (!first)
          {
            sub += ',';
          }
          first = false;

          sub += R"({"instType":")";
          sub += bitgetWsInstType(s.type);
          sub += R"(","channel":")";
          sub += (s.depth == BitgetConfig::BookDepth::Depth1 ? "books1" : "books50");
          sub += R"(","instId":")";
          sub += s.name;
          sub += R"("})";

          sub += ",";

          sub += R"({"instType":")";
          sub += bitgetWsInstType(s.type);
          sub += R"(","channel":"trade","instId":")";
          sub += s.name;
          sub += R"("})";
        }
        sub += "]}";

        _logger->info(std::string("[Bitget] subscribe: ") + sub);

        _wsClient->send(sub);
      });

  _wsClient->onMessage(
      [this](std::string_view payload)
      {
        handleMessage(payload);
      });

  _wsClient->start();

  if (_config.enablePrivate)
  {
    _wsClientPrivate = std::make_unique<IxWebSocketClient>(_config.privateEndpoint, BITGET_ORIGIN,
                                                           _config.reconnectDelayMs, _logger.get());

    _wsClientPrivate->onOpen(
        [this]()
        {
          auto auth = makeLoginPayload(_config.apiKey, _config.apiSecret, _config.passphrase);
          _wsClientPrivate->send(auth);
        });

    _wsClientPrivate->onMessage(
        [this](std::string_view payload)
        {
          handlePrivateMessage(payload);
        });

    _wsClientPrivate->start();
  }
}

void BitgetExchangeConnector::stop()
{
  if (!_running.exchange(false))
  {
    return;
  }
  if (_wsClient)
  {
    _wsClient->stop();
    _wsClient.reset();
  }
  if (_wsClientPrivate)
  {
    _wsClientPrivate->stop();
    _wsClientPrivate.reset();
  }
}

void BitgetExchangeConnector::handleMessage(std::string_view payload)
{
  static thread_local simdjson::ondemand::parser parser;

  try
  {
    std::string json_str(payload);
    auto doc = parser.iterate(json_str);

    if (doc["data"].error() && doc["action"].error())
    {
      return;
    }

    std::string_view action{};
    {
      auto actFld = doc["action"];
      if (!actFld.error())
      {
        auto actStr = actFld.get_string();
        if (!actStr.error())
        {
          action = actStr.value_unsafe();  // "snapshot" | "update"
        }
      }
    }

    auto arg = doc["arg"].get_object();
    auto channel = arg["channel"].get_string().value();
    auto inst = arg["instId"].get_string().value();
    simdjson::ondemand::array data = doc["data"].get_array();

    if (channel.starts_with("books"))
    {
      auto evOpt = _bookPool.acquire();
      if (!evOpt)
      {
        return;
      }
      auto& ev = *evOpt;
      SymbolId sid = resolveSymbolId(inst);
      ev->update.symbol = sid;

      BookUpdateType updateType = BookUpdateType::SNAPSHOT;
      if (action == "update")
      {
        updateType = BookUpdateType::DELTA;
      }

      ev->update.type = updateType;

      if (_registry)
      {
        if (const auto info = _registry->getSymbolInfo(sid))
        {
          ev->update.instrument = info->type;
        }
      }
      for (auto d : data)
      {
        auto bids = d["bids"].get_array();
        if (!bids.error())
        {
          for (auto lvl : bids.value())
          {
            auto row = lvl.get_array().value();
            std::string_view p = row.at(0).get_string().value();
            row.reset();
            std::string_view q = row.at(1).get_string().value();
            ev->update.bids.emplace_back(Price::fromDouble(std::strtod(p.data(), nullptr)),
                                         Quantity::fromDouble(std::strtod(q.data(), nullptr)));
          }
        }
        auto asks = d["asks"].get_array();
        if (!asks.error())
        {
          for (auto lvl : asks.value())
          {
            auto row = lvl.get_array().value();
            std::string_view p = row.at(0).get_string().value();
            row.reset();
            std::string_view q = row.at(1).get_string().value();
            ev->update.asks.emplace_back(Price::fromDouble(std::strtod(p.data(), nullptr)),
                                         Quantity::fromDouble(std::strtod(q.data(), nullptr)));
          }
        }
      }

      if (!ev->update.bids.empty() || !ev->update.asks.empty())
      {
        _bookUpdateBus->publish(std::move(ev));
      }
    }
    else if (channel == "trade")
    {
      for (auto val : data)
      {
        simdjson::ondemand::array row = val.get_array();

        auto it = row.begin();  // ➜ ts
        ++it;
        std::string_view priceSv = (*it).get_string().value();
        ++it;
        std::string_view qtySv = (*it).get_string().value();
        ++it;
        std::string_view sideSv = (*it).get_string().value();

        TradeEvent ev;
        SymbolId sid = resolveSymbolId(inst);
        ev.trade.symbol = sid;
        if (_registry)
        {
          if (const auto info = _registry->getSymbolInfo(sid))
          {
            ev.trade.instrument = info->type;
          }
        }

        ev.trade.price = Price::fromDouble(std::strtod(priceSv.data(), nullptr));
        ev.trade.quantity = Quantity::fromDouble(std::strtod(qtySv.data(), nullptr));
        ev.trade.isBuy = (sideSv == "buy" || sideSv == "BUY");

        _tradeBus->publish(ev);
      }
    }
  }
  catch (const simdjson::simdjson_error& e)
  {
    FLOX_LOG_ERROR(std::string("[Bitget] JSON parse error: ") + e.what() +
                   ", payload=" + std::string(payload));
    _logger->warn(std::string("[Bitget] json error: ") + e.what());
  }
}

void BitgetExchangeConnector::handlePrivateMessage(std::string_view payload)
{
  static thread_local simdjson::ondemand::parser parser;
  try
  {
    std::string json(payload);
    auto doc = parser.iterate(json);
    auto channelField = doc["arg"]["channel"];
    if (channelField.error())
    {
      return;
    }
    auto channel = channelField.get_string().value();
    auto data = doc["data"].get_array().value();
    if (channel == "orders")
    {
      for (auto d : data)
      {
        OrderEvent ev;
        ev.order.symbol = resolveSymbolId(d["instId"].get_string().value());
        ev.order.id = static_cast<OrderId>(
            std::strtoull(d["orderId"].get_string().value().data(), nullptr, 10));
        ev.order.side = d["side"].get_string().value() == "buy" ? Side::BUY : Side::SELL;
        ev.order.price =
            Price::fromDouble(std::strtod(d["price"].get_string().value().data(), nullptr));
        ev.order.quantity =
            Quantity::fromDouble(std::strtod(d["size"].get_string().value().data(), nullptr));
        std::string_view status = d["status"].get_string().value();
        if (status == "filled")
        {
          ev.status = OrderEventStatus::FILLED;
        }
        else if (status == "canceled")
        {
          ev.status = OrderEventStatus::CANCELED;
        }
        else
        {
          ev.status = OrderEventStatus::SUBMITTED;
        }
        _orderBus->publish(std::move(ev));
      }
    }
  }
  catch (const simdjson::simdjson_error& e)
  {
    FLOX_LOG_ERROR(std::string("[Bitget] priv json error: ") + e.what());
    _logger->warn(std::string("[Bitget] priv json error: ") + e.what());
  }
}

SymbolId BitgetExchangeConnector::resolveSymbolId(std::string_view sym)
{
  if (auto existing = _registry->getSymbolId("bitget", std::string(sym)))
  {
    return *existing;
  }

  SymbolInfo info;
  info.exchange = "bitget";
  info.symbol = std::string(sym);
  info.type = InstrumentType::Spot;

  for (const auto& s : _config.symbols)
  {
    if (s.name == sym)
    {
      info.type = s.type;
      break;
    }
  }
  return _registry->registerSymbol(info);
}

bool BitgetConfig::isValid() const
{
  if (publicEndpoint.empty())
  {
    return false;
  }
  if (enablePrivate &&
      (privateEndpoint.empty() || apiKey.empty() || apiSecret.empty() || passphrase.empty()))
  {
    return false;
  }
  for (const auto& s : symbols)
  {
    if (s.name.empty() || s.depth == BookDepth::Invalid)
    {
      return false;
    }
  }
  return true;
}

}  // namespace flox
