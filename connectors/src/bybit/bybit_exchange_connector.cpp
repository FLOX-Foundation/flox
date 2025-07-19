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

#include <flox/log/atomic_logger.h>
#include <flox/log/log.h>

#include <chrono>
#include <memory>
#include <string_view>

#include <simdjson.h>

namespace flox
{

BybitExchangeConnector::BybitExchangeConnector(
    const BybitConfig& config,
    BookUpdateBus* bookUpdateBus,
    TradeBus* tradeBus,
    std::move_only_function<SymbolId(std::string_view)> symbolMapper,
    std::shared_ptr<ILogger> logger)
    : _bookUpdateBus(bookUpdateBus),
      _tradeBus(tradeBus),
      _getSymbolId(std::move(symbolMapper)),
      _logger(std::move(logger))
{
  _symbols = config.symbols;
  _endpoint = config.endpoint;

  _wsClient = std::make_unique<IxWebSocketClient>(
      _endpoint,
      "https://www.bybit.com",
      config.reconnectDelayMs,
      _logger.get());
}

void BybitExchangeConnector::start()
{
  if (_running.exchange(true))
  {
    return;
  }

  FLOX_LOG("starting BybitExchangeConnector");

  _wsClient->onOpen([this]()
                    {  
    std::string sub;
    sub.reserve(64 + _symbols.size() * 32);
    sub += R"({"op":"subscribe","args":[)";

    bool first = true;
    for (const auto& sym : _symbols)
    {
      if (!first)
      {
        sub += ',';
      }
      first = false;

      sub += '"';
      sub += "orderbook.1.";
      sub += sym;
      sub += "\",\"";
      sub += "publicTrade.";
      sub += sym;
      sub += '"';
    }
    sub += "]}";

    _logger->info("[Bybit] WebSocket connected, sending subscription");
    _wsClient->send(sub); });

  _wsClient->onMessage([this](std::string_view payload)
                       {
    try {
      handleMessage(payload);
    }
    catch (const std::exception& e)
    {
      FLOX_LOG_ERROR("exception during message handling");
      _logger->error(std::string("[Bybit] Exception while handling message: ") + e.what());
    } });

  _wsClient->onClose([this](int code, std::string_view reason)
                     {
                       FLOX_LOG("socket closed");
                       _logger->info("[Bybit] WebSocket closed: code=" + std::to_string(code) +
                                     ", reason=" + std::string(reason)); });

  _wsClient->start();
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
}

void BybitExchangeConnector::handleMessage(std::string_view payload)
{
  static thread_local simdjson::ondemand::parser parser;

  try
  {
    std::string json(payload);
    auto doc = parser.iterate(json);

    auto topicField = doc.find_field_unordered("topic");
    if (topicField.error())
    {
      return;
    }

    std::string_view topic = topicField.get_string().value();

    auto dataField = doc.find_field_unordered("data");
    if (dataField.error())
    {
      return;
    }

    auto data = dataField.value();

    if (topic.starts_with("orderbook.1."))
    {
      auto evOpt = _bookPool.acquire();
      if (!evOpt)
      {
        return;
      }

      auto& ev = *evOpt;
      ev->update.symbol = _getSymbolId(data["s"]);
      ev->update.type = BookUpdateType::SNAPSHOT;

      auto bidsRes = data["b"];
      if (!bidsRes.error())
      {
        for (simdjson::ondemand::value lvlVal : bidsRes.get_array().value())
        {
          auto lvl = lvlVal.get_array().value();
          std::string_view psv = lvl.at(0).get_string().value();
          std::string_view qsv = lvl.at(1).get_string().value();
          ev->update.bids.emplace_back(
              Price::fromDouble(std::strtod(psv.data(), nullptr)),
              Quantity::fromDouble(std::strtod(qsv.data(), nullptr)));
        }
      }

      auto asksRes = data["a"];
      if (!asksRes.error())
      {
        for (simdjson::ondemand::value lvlVal : asksRes.get_array().value())
        {
          auto lvl = lvlVal.get_array().value();
          std::string_view psv = lvl.at(0).get_string().value();
          std::string_view qsv = lvl.at(1).get_string().value();
          ev->update.asks.emplace_back(
              Price::fromDouble(std::strtod(psv.data(), nullptr)),
              Quantity::fromDouble(std::strtod(qsv.data(), nullptr)));
        }
      }

      if (!ev->update.bids.empty() || !ev->update.asks.empty())
      {
        _bookUpdateBus->publish(std::move(ev));
      }
    }
    else if (topic.starts_with("publicTrade."))
    {
      simdjson::ondemand::array trades = data.get_array().value();
      for (simdjson::ondemand::value t : trades)
      {
        SymbolId sym = _getSymbolId(t["s"]);
        std::string_view psv = t["p"].get_string().value();
        std::string_view qsv = t["v"].get_string().value();
        TradeEvent ev;
        ev.trade.symbol = sym;
        ev.trade.price = Price::fromDouble(std::strtod(psv.data(), nullptr));
        ev.trade.quantity = Quantity::fromDouble(std::strtod(qsv.data(), nullptr));
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

}  // namespace flox
