/*
  * Flox Engine
  * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
  *
  * Copyright (c) 2025 FLOX Foundation
  * Licensed under the MIT License. See LICENSE file in the project root for full
  * license information.
  */

#include "flox-connectors/bybit/bybit_exchange_connector.h"
#include "flox-connectors/net/ix_websocket_client.h"
#include "flox/engine/symbol_registry.h"

#include <flox/log/atomic_logger.h>
#include <flox/log/log.h>

#include <openssl/hmac.h>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <simdjson.h>

namespace flox
{

static constexpr auto BYBIT_ORIGIN = "https://www.bybit.com";

std::optional<SymbolInfo> parseOptionSymbol(std::string_view fullSymbol,
                                            std::string_view exchange = "bybit")
{
  if (fullSymbol.ends_with("-USDT"))
  {
    fullSymbol.remove_suffix(6);
  }

  // Format: BTC-30AUG24-50000-C
  size_t dash1 = fullSymbol.find('-');
  size_t dash2 = fullSymbol.find('-', dash1 + 1);
  size_t dash3 = fullSymbol.find('-', dash2 + 1);

  if (dash1 == std::string_view::npos || dash2 == std::string_view::npos ||
      dash3 == std::string_view::npos)
  {
    return std::nullopt;
  }

  std::string underlying = std::string(fullSymbol.substr(0, dash1));
  std::string expiryStr =
      std::string(fullSymbol.substr(dash1 + 1, dash2 - dash1 - 1));  // e.g. 30AUG24
  std::string strikeStr =
      std::string(fullSymbol.substr(dash2 + 1, dash3 - dash2 - 1));  // e.g. 50000
  std::string typeStr = std::string(fullSymbol.substr(dash3 + 1));   // e.g. C or P

  // Parse date
  std::istringstream iss(expiryStr);
  std::tm tm = {};
  iss >> std::get_time(&tm, "%d%b%y");  // format: 30AUG24

  if (iss.fail())
  {
    return std::nullopt;
  }

  auto expiry_tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));

  // Strike
  double strike = std::strtod(strikeStr.c_str(), nullptr);
  if (strike <= 0.0)
  {
    return std::nullopt;
  }

  OptionType optType;
  if (typeStr == "C")
  {
    optType = OptionType::CALL;
  }
  else if (typeStr == "P")
  {
    optType = OptionType::PUT;
  }
  else
  {
    return std::nullopt;
  }

  SymbolInfo info;
  info.exchange = std::string(exchange);
  info.symbol = std::string(fullSymbol);
  info.type = InstrumentType::Option;
  info.strike = Price::fromDouble(strike);
  info.expiry = TimePoint(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      expiry_tp.time_since_epoch()));
  info.optionType = optType;

  return info;
}

static std::string makePrivateAuthPayload(std::string_view apiKey, std::string_view apiSecret,
                                          std::chrono::milliseconds ttl = std::chrono::seconds{15})
{
  using namespace std::chrono;

  const auto expires = duration_cast<milliseconds>(system_clock::now().time_since_epoch()) + ttl;
  const auto expiresMs = expires.count();
  const std::string toSign = "GET/realtime" + std::to_string(expiresMs);

  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hashLen = 0;
  HMAC(EVP_sha256(), apiSecret.data(), static_cast<int>(apiSecret.size()),
       reinterpret_cast<const unsigned char*>(toSign.data()), toSign.size(), hash, &hashLen);

  char hex[EVP_MAX_MD_SIZE * 2 + 1];
  for (unsigned i = 0; i < hashLen; ++i)
  {
    std::sprintf(hex + i * 2, "%02x", hash[i]);
  }
  hex[hashLen * 2] = '\0';

  std::string payload;
  payload.reserve(128);
  payload.append(R"({"op":"auth","args":[")")
      .append(apiKey)
      .append("\",")
      .append(std::to_string(expiresMs))
      .append(",\"")
      .append(hex)
      .append("\"]}");

  return payload;
}

BybitExchangeConnector::BybitExchangeConnector(const BybitConfig& config,
                                               BookUpdateBus* bookUpdateBus, TradeBus* tradeBus,
                                               OrderExecutionBus* orderBus,
                                               SymbolRegistry* registry,
                                               std::shared_ptr<ILogger> logger)
    : _config(config),
      _bookUpdateBus(bookUpdateBus),
      _tradeBus(tradeBus),
      _orderBus(orderBus),
      _registry(registry),
      _logger(std::move(logger))
{
  _wsClient = std::make_unique<IxWebSocketClient>(config.publicEndpoint, BYBIT_ORIGIN,
                                                  config.reconnectDelayMs, _logger.get());
}

void BybitExchangeConnector::start()
{
  if (!_config.isValid())
  {
    FLOX_LOG_ERROR("[Bybit] Invalid connector config");
    _logger->error("[Bybit] Invalid connector config");
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
        for (const auto& entry : _config.symbols)
        {
          const auto& sym = entry.name;
          const auto& type = entry.type;

          if (!first)
          {
            sub += ',';
          }

          first = false;

          sub += '"';
          sub += "orderbook." + std::to_string(static_cast<int>(entry.depth)) + "." + sym;
          sub += "\",\"";
          sub += "publicTrade." + sym;
          sub += '"';
        }

        sub += "]}";

        FLOX_LOG("[Bybit] WebSocket connected, sending subscription " << sub);
        _logger->info("[Bybit] WebSocket connected, sending subscription");
        _wsClient->send(sub);
      });

  _wsClient->onMessage(
      [this](std::string_view payload)
      {
        try
        {
          handleMessage(payload);
        }
        catch (const std::exception& e)
        {
          FLOX_LOG_ERROR("[Bybit] Exception while handling message: " << e.what());
          _logger->error(std::string("[Bybit] Exception while handling message: ") + e.what());
        }
      });

  _wsClient->onClose(
      [this](int code, std::string_view reason)
      {
        FLOX_LOG("[Bybit] WebSocket closed: code=" << std::to_string(code)
                                                   << ", reason=" << std::string(reason));
        _logger->info("[Bybit] WebSocket closed: code=" + std::to_string(code) +
                      ", reason=" + std::string(reason));
      });

  _wsClient->start();

  if (_config.enablePrivate)
  {
    _wsClientPrivate = std::make_unique<IxWebSocketClient>(_config.privateEndpoint, BYBIT_ORIGIN,
                                                           _config.reconnectDelayMs, _logger.get());

    _wsClientPrivate->onOpen(
        [this]()
        {
          auto auth = makePrivateAuthPayload(_config.apiKey, _config.apiSecret);
          _wsClientPrivate->send(auth);
        });

    _wsClientPrivate->onMessage(
        [this](std::string_view payload)
        {
          handlePrivateMessage(payload);
        });

    _wsClientPrivate->onClose(
        [this](int code, std::string_view reason)
        {
          FLOX_LOG("[Bybit] Private WS closed: code=" << std::to_string(code)
                                                      << ", reason=" << std::string(reason));
          _logger->info("[Bybit] Private WS closed: code=" + std::to_string(code) +
                        ", reason=" + std::string(reason));
        });

    _wsClientPrivate->start();
  }
}

void BybitExchangeConnector::stop()
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

void BybitExchangeConnector::handleMessage(std::string_view payload)
{
  static thread_local simdjson::ondemand::parser parser;

  try
  {
    std::string json(payload);
    auto doc = parser.iterate(json);

    auto topic = doc["topic"].get_string().value();
    auto data = doc["data"].value();

    if (topic.starts_with("orderbook."))
    {
      auto evOpt = _bookPool.acquire();
      if (!evOpt)
      {
        return;
      }

      auto& ev = *evOpt;
      SymbolId sym = resolveSymbolId(data["s"]);
      ev->update.symbol = sym;

      auto type = doc["type"];  // "snapshot" | "delta"
      BookUpdateType updateType = BookUpdateType::SNAPSHOT;
      if (!type.error())
      {
        auto tsv = type.get_string().value();
        if (tsv == "delta")
        {
          updateType = BookUpdateType::DELTA;
        }
      }

      ev->update.type = updateType;

      if (_registry)
      {
        if (const auto info = _registry->getSymbolInfo(sym))
        {
          ev->update.instrument = info->type;
          ev->update.strike = info->strike;
          ev->update.expiry = info->expiry;
          ev->update.optionType = info->optionType;
        }
      }

      for (auto side : {std::pair{"b", &ev->update.bids}, std::pair{"a", &ev->update.asks}})
      {
        auto arr = data[side.first];
        if (!arr.error())
        {
          for (auto lvl : arr.get_array().value())
          {
            auto lv = lvl.get_array().value();
            std::string_view psv = lv.at(0).get_string().value();
            lv.reset();
            std::string_view qsv = lv.at(1).get_string().value();
            side.second->emplace_back(Price::fromDouble(std::strtod(psv.data(), nullptr)),
                                      Quantity::fromDouble(std::strtod(qsv.data(), nullptr)));
          }
        }
      }

      if (!ev->update.bids.empty() || !ev->update.asks.empty())
      {
        _bookUpdateBus->publish(std::move(ev));
      }
    }
    else if (topic.starts_with("publicTrade."))
    {
      for (auto t : data.get_array().value())
      {
        SymbolId sym = resolveSymbolId(t["s"]);
        TradeEvent ev;
        ev.trade.symbol = sym;

        if (_registry)
        {
          if (const auto info = _registry->getSymbolInfo(sym))
          {
            ev.trade.instrument = info->type;
          }
        }

        ev.trade.price =
            Price::fromDouble(std::strtod(t["p"].get_string().value().data(), nullptr));
        ev.trade.quantity =
            Quantity::fromDouble(std::strtod(t["v"].get_string().value().data(), nullptr));
        ev.trade.isBuy = (t["S"].get_string().value() == "Buy");
        ev.trade.timestamp = std::chrono::steady_clock::now();
        _tradeBus->publish(ev);
      }
    }
  }
  catch (const simdjson::simdjson_error& e)
  {
    _logger->warn(std::string("[Bybit] simdjson error: ") + e.what());
  }
}

void BybitExchangeConnector::handlePrivateMessage(std::string_view payload)
{
  static thread_local simdjson::ondemand::parser parser;

  try
  {
    std::string json(payload);
    auto doc = parser.iterate(json);

    if (auto opField = doc["op"]; !opField.error())
    {
      auto op = opField.get_string().value();

      if (op == "auth")
      {
        bool success = doc["success"].get_bool().value();
        if (success)
        {
          std::string sub = "{\"op\":\"subscribe\",\"args\":[\"order\",\"execution\"]}";
          _wsClientPrivate->send(sub);
        }
      }

      FLOX_LOG("[Bybit] service op=" << op);
      _logger->info(std::string("[Bybit] service op=") + std::string(op));
      return;
    }

    auto topicField = doc["topic"];
    if (topicField.error())
    {
      FLOX_LOG_ERROR("[Bybit] frame without topic, skip");
      _logger->warn("[Bybit] frame without topic, skip");
      return;
    }

    auto topic = topicField.get_string().value();
    auto data = doc["data"].value();

    if (topic == "order")
    {
      for (auto d : data.get_array().value())
      {
        OrderEvent ev;

        std::string_view symbol = d["symbol"].get_string().value();

        ev.order.symbol = resolveSymbolId(symbol);
        ev.order.id = static_cast<OrderId>(
            std::strtoull(d["orderId"].get_string().value().data(), nullptr, 10));
        ev.order.side = (d["side"].get_string().value() == "Buy") ? Side::BUY : Side::SELL;

        ev.order.price =
            Price::fromDouble(std::strtod(d["price"].get_string().value().data(), nullptr));
        ev.order.quantity =
            Quantity::fromDouble(std::strtod(d["qty"].get_string().value().data(), nullptr));
        ev.order.filledQuantity =
            Quantity::fromDouble(std::strtod(d["cumExecQty"].get_string().value().data(), nullptr));

        std::string_view status = d["orderStatus"].get_string().value();
        if (status == "New")
        {
          ev.status = OrderEventStatus::SUBMITTED;
        }
        else if (status == "PartiallyFilled")
        {
          ev.status = OrderEventStatus::PARTIALLY_FILLED;
        }
        else if (status == "Filled")
        {
          ev.status = OrderEventStatus::FILLED;
        }
        else if (status == "Cancelled")
        {
          ev.status = OrderEventStatus::CANCELED;
        }
        else if (status == "Rejected")
        {
          ev.status = OrderEventStatus::REJECTED;
        }
        else if (status == "Expired")
        {
          ev.status = OrderEventStatus::EXPIRED;
        }
        else
        {
          ev.status = OrderEventStatus::SUBMITTED;
        }

        _orderBus->publish(std::move(ev));
      }
    }
    else if (topic == "execution")
    {
      for (auto d : data.get_array().value())
      {
        OrderEvent ev;

        ev.order.id = static_cast<OrderId>(
            std::strtoull(d["orderId"].get_string().value().data(), nullptr, 10));
        ev.order.symbol = resolveSymbolId(d["symbol"].get_string().value());
        ev.order.side = d["side"].get_string().value() == "Buy" ? Side::BUY : Side::SELL;

        ev.order.price =
            Price::fromDouble(std::strtod(d["execPrice"].get_string().value().data(), nullptr));
        ev.order.quantity =
            Quantity::fromDouble(std::strtod(d["execQty"].get_string().value().data(), nullptr));

        ev.order.filledQuantity = ev.order.quantity;

        ev.status = d["execType"].get_string().value() == "Trade"
                        ? OrderEventStatus::PARTIALLY_FILLED
                        : OrderEventStatus::SUBMITTED;

        _orderBus->publish(std::move(ev));
      }
    }
  }
  catch (const simdjson::simdjson_error& e)
  {
    FLOX_LOG_ERROR("[Bybit] simdjson private error: " << e.what());
    _logger->warn(std::string("[Bybit] simdjson private error: ") + e.what());
  }
}

SymbolId BybitExchangeConnector::resolveSymbolId(std::string_view symbol)
{
  auto existing = _registry->getSymbolId("bybit", std::string(symbol));
  if (existing)
  {
    return *existing;
  }

  SymbolInfo info;
  info.exchange = "bybit";
  info.symbol = std::string(symbol);

  // Try to parse as option
  if (auto parsed = parseOptionSymbol(symbol, "bybit"))
  {
    return _registry->registerSymbol(*parsed);
  }

  auto it = std::find_if(_config.symbols.begin(), _config.symbols.end(),
                         [&](const BybitConfig::SymbolEntry& entry)
                         {
                           return entry.name == symbol;
                         });

  if (it != _config.symbols.end())
  {
    info.type = it->type;
  }
  else if (auto parsed = parseOptionSymbol(symbol))
  {
    return _registry->registerSymbol(*parsed);
  }
  else
  {
    info.type = InstrumentType::Spot;
  }

  return _registry->registerSymbol(info);
}

bool BybitConfig::isValid() const
{
  if (publicEndpoint.empty())
  {
    FLOX_LOG_ERROR("BybitConfig validation failed: public endpoint is empty");
    return false;
  }

  for (const auto& s : symbols)
  {
    if (s.name.empty())
    {
      FLOX_LOG_ERROR("BybitConfig validation failed: symbol name is empty");
      return false;
    }

    if (s.depth == BookDepth::Invalid)
    {
      FLOX_LOG_ERROR("BybitConfig validation failed: symbol " << s.name
                                                              << " has invalid BookDepth");
      return false;
    }

    switch (s.type)
    {
      case InstrumentType::Spot:
        if (s.depth != BookDepth::Top1 && s.depth != BookDepth::Top50 &&
            s.depth != BookDepth::Top200)
        {
          FLOX_LOG_ERROR("BybitConfig validation failed: symbol "
                         << s.name << " (Spot) has unsupported BookDepth: "
                         << static_cast<int>(s.depth) << ". Allowed: 1, 50, 200");
          return false;
        }
        break;

      case InstrumentType::Future:
        if (s.depth != BookDepth::Top1 && s.depth != BookDepth::Top50 &&
            s.depth != BookDepth::Top200 && s.depth != BookDepth::Top500)
        {
          FLOX_LOG_ERROR("BybitConfig validation failed: symbol "
                         << s.name << " (Future) has unsupported BookDepth: "
                         << static_cast<int>(s.depth) << ". Allowed: 1, 50, 200, 500");
          return false;
        }
        break;

      case InstrumentType::Option:
        if (s.depth != BookDepth::Top25 && s.depth != BookDepth::Top100)
        {
          FLOX_LOG_ERROR("BybitConfig validation failed: symbol "
                         << s.name << " (Option) has unsupported BookDepth: "
                         << static_cast<int>(s.depth) << ". Allowed: 25, 100");
          return false;
        }
        break;

      default:
        FLOX_LOG_ERROR("BybitConfig validation failed: symbol " << s.name
                                                                << " has unknown InstrumentType");
        return false;
    }
  }

  if (enablePrivate)
  {
    if (privateEndpoint.empty())
    {
      FLOX_LOG_ERROR("BybitConfig validation failed: private endpoint is empty");
      return false;
    }

    if (apiKey.empty() || apiSecret.empty())
    {
      FLOX_LOG_ERROR("BybitConfig validation failed: private API credentials missing");
      return false;
    }
  }

  return true;
}
}  // namespace flox
