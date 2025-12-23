/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/bitget/bitget_order_executor.h"
#include "flox-connectors/bitget/authenticated_rest_client.h"

#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/order_tracker.h>
#include <flox/log/log.h>

#include <simdjson.h>

namespace flox
{

template <typename Policies>
BitgetOrderExecutorT<Policies>::~BitgetOrderExecutorT() = default;

static constexpr std::string_view kPathPlace = "/api/v2/mix/order/place-order";
static constexpr std::string_view kPathCancel = "/api/v2/mix/order/cancel-order";
static constexpr std::string_view kPathModify = "/api/v2/mix/order/modify-order";

template <typename Policies>
void BitgetOrderExecutorT<Policies>::submitOrder(const Order& order)
{
  if (!_policies.rateLimit.tryAcquire(order.id))
  {
    return;
  }

  auto info = _registry->getSymbolInfo(order.symbol);
  if (!info)
  {
    FLOX_LOG_ERROR("[BitgetOE] submitOrder: unknown symbolId=" << order.symbol);
    return;
  }

  std::string body;
  body.reserve(192);
  body.append("{\"symbol\":\"")
      .append(info->symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginMode\":\"")
      .append(_params.marginMode)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",")
      .append("\"size\":\"")
      .append(order.quantity.toString())
      .append("\",")
      .append("\"price\":\"")
      .append(order.price.toString())
      .append("\",")
      .append("\"side\":\"")
      .append(order.side == Side::BUY ? "buy" : "sell")
      .append("\",")
      .append("\"tradeSide\":\"open\",")
      .append("\"orderType\":\"limit\",")
      .append("\"force\":\"")
      .append(_params.forcePolicy)
      .append("\",")
      .append("\"clientOid\":\"")
      .append(std::to_string(order.id))
      .append("\"}");

  _policies.timeout.trackSubmit(order.id);

  _client->post(
      std::string(kPathPlace), body,
      [this, order](std::string_view resp)
      {
        _policies.timeout.clearPending(order.id);

        simdjson::ondemand::parser p;
        simdjson::padded_string ps(resp);
        auto doc = p.iterate(ps);
        if (std::string_view(doc["code"]) != "00000")
        {
          FLOX_LOG_ERROR("[BitgetOE] submitOrder failed: " << doc["msg"].get_string().value());
          return;
        }

        std::string exchId = std::string(doc["data"]["orderId"].get_string().value());
        _orderTracker->onSubmitted(order, exchId);
      },
      [this, order](std::string_view err)
      {
        _policies.timeout.clearPending(order.id);
        FLOX_LOG_ERROR("[BitgetOE] submitOrder transport: " << err);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::cancelOrder(OrderId id)
{
  if (!_policies.rateLimit.tryAcquire(id))
  {
    return;
  }

  auto st = _orderTracker->get(id);
  if (!st)
  {
    FLOX_LOG_ERROR("[BitgetOE] cancelOrder: unknown id=" << id);
    return;
  }

  auto info = _registry->getSymbolInfo(st->localOrder.symbol);
  if (!info)
  {
    FLOX_LOG_ERROR("[BitgetOE] cancelOrder: no symbol info for id=" << st->localOrder.symbol);
    return;
  }

  std::string body;
  body.reserve(128);
  body.append("{\"symbol\":\"")
      .append(info->symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",");

  if (!st->exchangeOrderId.empty())
  {
    body.append("\"orderId\":\"").append(st->exchangeOrderId).append("\"}");
  }
  else
  {
    body.append("\"clientOid\":\"").append(std::to_string(id)).append("\"}");
  }

  _policies.timeout.trackCancel(id);

  _client->post(
      std::string(kPathCancel), body,
      [this, id](std::string_view resp)
      {
        _policies.timeout.clearPending(id);

        simdjson::ondemand::parser p;
        simdjson::padded_string ps(resp);
        auto doc = p.iterate(ps);
        if (std::string_view(doc["code"]) == "00000")
        {
          _orderTracker->onCanceled(id);
        }
        else
        {
          FLOX_LOG_ERROR("[BitgetOE] cancelOrder failed: " << doc["msg"].get_string().value());
        }
      },
      [this, id](std::string_view err)
      {
        _policies.timeout.clearPending(id);
        FLOX_LOG_ERROR("[BitgetOE] cancelOrder transport: " << err);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::replaceOrder(OrderId oldId, const Order& newOrd)
{
  if (!_policies.rateLimit.tryAcquire(oldId))
  {
    return;
  }

  auto st = _orderTracker->get(oldId);
  if (!st)
  {
    FLOX_LOG_ERROR("[BitgetOE] replaceOrder: unknown id=" << oldId);
    return;
  }

  auto info = _registry->getSymbolInfo(st->localOrder.symbol);
  if (!info)
  {
    FLOX_LOG_ERROR("[BitgetOE] replaceOrder: no symbol info for id=" << st->localOrder.symbol);
    return;
  }

  std::string body;
  body.reserve(256);
  body.append("{\"orderId\":\"")
      .append(st->exchangeOrderId)
      .append("\",")
      .append("\"symbol\":\"")
      .append(info->symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",")
      .append("\"newPrice\":\"")
      .append(newOrd.price.toString())
      .append("\",")
      .append("\"newSize\":\"")
      .append(newOrd.quantity.toString())
      .append("\",")
      .append("\"newClientOid\":\"")
      .append(std::to_string(newOrd.id))
      .append("\"}");

  _policies.timeout.trackReplace(oldId);

  _client->post(
      std::string(kPathModify), body,
      [this, oldId, newOrd](std::string_view resp)
      {
        _policies.timeout.clearPending(oldId);

        simdjson::ondemand::parser p;
        simdjson::padded_string ps(resp);
        auto doc = p.iterate(ps);
        if (std::string_view(doc["code"]) != "00000")
        {
          FLOX_LOG_ERROR("[BitgetOE] replaceOrder failed: " << doc["msg"].get_string().value());
          return;
        }

        auto ordIdField = doc["data"]["orderId"];
        std::string exch = ordIdField.type() == simdjson::ondemand::json_type::string
                               ? std::string(ordIdField.get_string().value())
                               : std::string();
        _orderTracker->onReplaced(oldId, newOrd, exch);
      },
      [this, oldId](std::string_view err)
      {
        _policies.timeout.clearPending(oldId);
        FLOX_LOG_ERROR("[BitgetOE] replaceOrder transport: " << err);
      });
}

// Explicit instantiations
template class BitgetOrderExecutorT<NoPolicies>;
template class BitgetOrderExecutorT<WithRateLimit>;
template class BitgetOrderExecutorT<WithTimeout>;
template class BitgetOrderExecutorT<FullPolicies>;

}  // namespace flox
