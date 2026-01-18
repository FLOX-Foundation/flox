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
#include "flox-connectors/util/safe_parse.h"
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
static constexpr auto BITGET_USER_AGENT =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Safari/537.36";

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
    std::snprintf(hex + i * 2, 3, "%02x", hash[i]);
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
      return "SPOT";
    case InstrumentType::Future:
      return "USDT-FUTURES";
    case InstrumentType::Inverse:
      return "COIN-FUTURES";
    case InstrumentType::Option:
      return "SUSDT-FUTURES";  // Bitget uses this for simulation
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
  _wsClient = std::make_unique<IxWebSocketClient>(
      cfg.publicEndpoint, BITGET_ORIGIN, cfg.reconnectDelayMs, _logger.get(), 0, BITGET_USER_AGENT);
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
        constexpr size_t BATCH_SIZE = 10;

        for (size_t batchStart = 0; batchStart < _config.symbols.size(); batchStart += BATCH_SIZE)
        {
          std::string sub;
          sub.reserve(64 + BATCH_SIZE * 200);
          sub += R"({"op":"subscribe","args":[)";

          bool first = true;
          size_t batchEnd = std::min(batchStart + BATCH_SIZE, _config.symbols.size());

          for (size_t i = batchStart; i < batchEnd; ++i)
          {
            const auto& s = _config.symbols[i];

            if (!first)
            {
              sub += ',';
            }
            first = false;

            sub += R"({"instType":")";
            sub += bitgetWsInstType(s.type);
            sub += R"(","channel":")";
            switch (s.depth)
            {
              case BitgetConfig::BookDepth::Depth1:
                sub += "books1";
                break;
              case BitgetConfig::BookDepth::Depth5:
                sub += "books5";
                break;
              case BitgetConfig::BookDepth::Depth15:
                sub += "books15";
                break;
              case BitgetConfig::BookDepth::DepthFull:
              default:
                sub += "books";
                break;
            }
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

          _logger->info(std::string("[Bitget] subscribe batch ") +
                        std::to_string(batchStart / BATCH_SIZE + 1) + ": " +
                        std::to_string(batchEnd - batchStart) + " symbols");

          _wsClient->send(sub);
        }
      });

  _wsClient->onMessage(
      [this](std::string_view payload)
      {
        handleMessage(payload);
      });

  _wsClient->start();
  _pingThread = std::thread(&BitgetExchangeConnector::pingLoop, this);

  if (_config.enablePrivate)
  {
    _wsClientPrivate = std::make_unique<IxWebSocketClient>(_config.privateEndpoint, BITGET_ORIGIN,
                                                           _config.reconnectDelayMs, _logger.get(),
                                                           0, BITGET_USER_AGENT);

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

  if (_pingThread.joinable())
  {
    _pingThread.join();
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

void BitgetExchangeConnector::pingLoop()
{
  for (int i = 0; i < 50 && _running.load(); ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  while (_running.load())
  {
    if (_wsClient)
    {
      _wsClient->send("ping");
    }
    if (_wsClientPrivate)
    {
      _wsClientPrivate->send("ping");
    }

    for (int i = 0; i < 250 && _running.load(); ++i)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void BitgetExchangeConnector::handleMessage(std::string_view payload)
{
  if (payload == "pong")
  {
    return;
  }

  static thread_local simdjson::dom::parser parser;

  try
  {
    auto doc = parser.parse(payload);

    // Check if this is a data message
    auto actionEl = doc["action"];
    auto dataEl = doc["data"];
    if (actionEl.error() && dataEl.error())
    {
      return;
    }

    std::string_view action{};
    if (!actionEl.error())
    {
      action = actionEl.get_string().value();
    }

    auto arg = doc["arg"];
    if (arg.error())
    {
      return;
    }

    std::string_view channel = arg["channel"].get_string().value();
    std::string_view inst = arg["instId"].get_string().value();

    if (dataEl.error())
    {
      return;
    }
    auto data = dataEl.get_array();

    if (channel.substr(0, 5) == "books")
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
        auto bidsEl = d["bids"];
        if (!bidsEl.error())
        {
          for (auto lvl : bidsEl.get_array())
          {
            auto row = lvl.get_array();
            auto it = row.begin();
            std::string_view p = (*it).get_string().value();
            ++it;
            std::string_view q = (*it).get_string().value();

            auto priceOpt = util::safeParseDouble(p);
            auto qtyOpt = util::safeParseDouble(q);
            if (!priceOpt || !qtyOpt)
            {
              _logger->warn("[Bitget] Invalid bid price/qty in book update");
              continue;
            }
            ev->update.bids.emplace_back(Price::fromDouble(*priceOpt),
                                         Quantity::fromDouble(*qtyOpt));
          }
        }
        auto asksEl = d["asks"];
        if (!asksEl.error())
        {
          for (auto lvl : asksEl.get_array())
          {
            auto row = lvl.get_array();
            auto it = row.begin();
            std::string_view p = (*it).get_string().value();
            ++it;
            std::string_view q = (*it).get_string().value();

            auto priceOpt = util::safeParseDouble(p);
            auto qtyOpt = util::safeParseDouble(q);
            if (!priceOpt || !qtyOpt)
            {
              _logger->warn("[Bitget] Invalid ask price/qty in book update");
              continue;
            }
            ev->update.asks.emplace_back(Price::fromDouble(*priceOpt),
                                         Quantity::fromDouble(*qtyOpt));
          }
        }

        // Parse timestamp if available
        auto tsEl = d["ts"];
        if (!tsEl.error())
        {
          std::string_view tsStr = tsEl.get_string().value();
          auto tsOpt = util::parseInt64(tsStr);
          if (tsOpt)
          {
            ev->update.exchangeTsNs = *tsOpt * 1'000'000;
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
        std::string_view priceSv = val["price"].get_string().value();
        std::string_view qtySv = val["size"].get_string().value();
        std::string_view sideSv = val["side"].get_string().value();

        auto priceOpt = util::safeParseDouble(priceSv);
        auto qtyOpt = util::safeParseDouble(qtySv);
        if (!priceOpt || !qtyOpt)
        {
          _logger->warn("[Bitget] Invalid trade price/qty");
          continue;
        }

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

        ev.trade.price = Price::fromDouble(*priceOpt);
        ev.trade.quantity = Quantity::fromDouble(*qtyOpt);
        ev.trade.isBuy = (sideSv == "buy" || sideSv == "Buy");

        // Parse timestamp
        auto tsEl = val["ts"];
        if (!tsEl.error())
        {
          std::string_view tsStr = tsEl.get_string().value();
          auto tsOpt = util::parseInt64(tsStr);
          if (tsOpt)
          {
            ev.trade.exchangeTsNs = *tsOpt * 1'000'000;
          }
        }

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
  if (payload == "pong")
  {
    return;
  }

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

        auto orderIdOpt = util::parseUint64(d["orderId"].get_string().value());
        if (!orderIdOpt)
        {
          _logger->warn("[Bitget] Invalid orderId in order event");
          continue;
        }
        ev.order.id = static_cast<OrderId>(*orderIdOpt);
        ev.order.side = d["side"].get_string().value() == "buy" ? Side::BUY : Side::SELL;

        auto priceOpt = util::safeParseDouble(d["price"].get_string().value());
        auto qtyOpt = util::safeParseDouble(d["size"].get_string().value());
        if (!priceOpt || !qtyOpt)
        {
          _logger->warn("[Bitget] Invalid price/qty in order event");
          continue;
        }
        ev.order.price = Price::fromDouble(*priceOpt);
        ev.order.quantity = Quantity::fromDouble(*qtyOpt);
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
