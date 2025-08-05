/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/bybit/bybit_order_executor.h"
#include "flox-connectors/bybit/authenticated_rest_client.h"

#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/order_tracker.h>
#include <flox/log/log.h>

#include <cstdio>
#include <exception>
#include <memory>
#include <string>

#include <simdjson.h>

namespace flox
{

BybitOrderExecutor::BybitOrderExecutor(std::unique_ptr<AuthenticatedRestClient> client,
                                       SymbolRegistry* registry, OrderTracker* orderTracker)
    : _client(std::move(client)), _registry(registry), _orderTracker(orderTracker)
{
}

BybitOrderExecutor::~BybitOrderExecutor() = default;

void BybitOrderExecutor::submitOrder(const Order& order)
{
  auto info = _registry->getSymbolInfo(order.symbol);
  if (!info.has_value())
  {
    FLOX_LOG_ERROR("[Bybit] No symbol info registered for id " << order.symbol);
    std::terminate();
  }

  std::string body;
  body.reserve(160);
  body.append("{\"category\":\"").append(Bybit::toString(info->type)).append("\",");
  body.append("\"symbol\":\"").append(info->symbol).append("\",");
  body.append("\"side\":\"").append(order.side == Side::BUY ? "Buy" : "Sell").append("\",");
  body.append("\"orderType\":\"Limit\",");
  body.append("\"qty\":\"").append(order.quantity.toString()).append("\",");
  body.append("\"price\":\"").append(order.price.toString()).append("\"}");

  FLOX_LOG("[BybitOrderExecutor] Submitting order: id="
           << order.id << " symbol=" << info->symbol << " side="
           << (order.side == Side::BUY ? "Buy" : "Sell") << " qty=" << order.quantity.toDouble()
           << " price=" << order.price.toDouble() << " category=" << Bybit::toString(info->type));

  _client->post(
      "/v5/order/create", body,
      [this, order](std::string_view response)
      {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(response);
        auto doc = parser.iterate(padded);

        int64_t retCode = doc["retCode"].get_int64().value();
        if (retCode != 0)
        {
          std::string_view retMsg = doc["retMsg"].get_string().value();
          FLOX_LOG_ERROR("[BybitOrderExecutor] Order submission failed: retCode="
                         << retCode << " retMsg=" << retMsg);
          return;
        }

        std::string_view orderId = doc["result"]["orderId"].get_string().value();

        FLOX_LOG("[BybitOrderExecutor] Order submitted. id=" << order.id
                                                             << " → exchangeOrderId=" << orderId);

        _orderTracker->onSubmitted(order, orderId);
      },
      [this](std::string_view error)
      {
        FLOX_LOG_ERROR("[BybitOrderExecutor] Transport error: " << error);
      });
}

void BybitOrderExecutor::cancelOrder(OrderId orderId)
{
  const auto* state = _orderTracker->get(orderId);
  if (!state)
  {
    FLOX_LOG_ERROR("[BybitOrderExecutor] Cannot cancel, unknown orderId=" << orderId);
    return;
  }

  auto info = _registry->getSymbolInfo(state->localOrder.symbol);
  if (!info.has_value())
  {
    FLOX_LOG_ERROR("[BybitOrderExecutor] No symbol info for symbolId=" << state->localOrder.symbol);
    return;
  }

  const std::string& exchangeOrderId = state->exchangeOrderId;

  std::string body;
  body.reserve(128);
  body.append("{\"category\":\"").append(Bybit::toString(info->type)).append("\",");
  body.append("\"symbol\":\"").append(info->symbol).append("\",");
  body.append("\"orderId\":\"").append(exchangeOrderId).append("\"}");

  FLOX_LOG_INFO("[BybitOrderExecutor] Cancelling order: localId=" << orderId << " exchangeId="
                                                                  << exchangeOrderId);

  _client->post(
      "/v5/order/cancel", body,
      [this, orderId](std::string_view response)
      {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(response);
        auto doc = parser.iterate(padded);

        int64_t retCode = doc["retCode"].get_int64().value();
        if (retCode != 0)
        {
          std::string_view msg = doc["retMsg"].get_string().value();
          FLOX_LOG_ERROR("[BybitOrderExecutor] Cancel failed: orderId="
                         << orderId << " retCode=" << retCode << " msg=" << msg);
        }
        else
        {
          FLOX_LOG_INFO("[BybitOrderExecutor] Cancel successful: orderId=" << orderId);
          _orderTracker->onCanceled(orderId);
        }
      },
      [orderId](std::string_view err)
      {
        FLOX_LOG_ERROR("[BybitOrderExecutor] Cancel transport error: orderId=" << orderId
                                                                               << " err=" << err);
      });
}

void BybitOrderExecutor::replaceOrder(OrderId oldOrderId, const Order& newOrder)
{
  auto info = _registry->getSymbolInfo(newOrder.symbol);
  if (!info.has_value())
  {
    FLOX_LOG_ERROR("[BybitOrderExecutor] No symbol info for symbolId=" << newOrder.symbol);
    return;
  }

  const auto* state = _orderTracker->get(oldOrderId);
  if (!state)
  {
    FLOX_LOG_ERROR("[BybitOrderExecutor] Cannot replace, unknown orderId=" << oldOrderId);
    return;
  }

  const std::string& exchangeOrderId = state->exchangeOrderId;

  std::string qty = newOrder.quantity.toString();
  std::string price = newOrder.price.toString();

  std::string body;
  body.reserve(128 + qty.size() + price.size());
  body.append("{\"category\":\"").append(Bybit::toString(info->type)).append("\",");
  body.append("\"symbol\":\"").append(info->symbol).append("\",");
  body.append("\"orderId\":\"").append(exchangeOrderId).append("\",");
  body.append("\"qty\":\"").append(qty).append("\",");
  body.append("\"price\":\"").append(price).append("\"}");

  FLOX_LOG_INFO("[BybitOrderExecutor] Replacing order: localId="
                << oldOrderId << " exchangeId=" << exchangeOrderId << " newQty=" << qty
                << " newPrice=" << price);

  _client->post(
      "/v5/order/amend", body,
      [this, oldOrderId, newOrder](std::string_view response)
      {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(response);
        auto doc = parser.iterate(padded);

        int64_t retCode = doc["retCode"].get_int64().value();
        if (retCode != 0)
        {
          std::string_view msg = doc["retMsg"].get_string().value();
          FLOX_LOG_ERROR("[BybitOrderExecutor] Replace failed: orderId="
                         << oldOrderId << " retCode=" << retCode << " msg=" << msg);
        }
        else
        {
          FLOX_LOG_INFO("[BybitOrderExecutor] Replace successful: orderId=" << oldOrderId);
          _orderTracker->onReplaced(oldOrderId, newOrder, "");
        }
      },
      [oldOrderId](std::string_view err)
      {
        FLOX_LOG_ERROR("[BybitOrderExecutor] Replace transport error: orderId=" << oldOrderId
                                                                                << " err=" << err);
      });
}

}  // namespace flox