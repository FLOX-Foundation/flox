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
#include <flox/execution/events/order_event.h>
#include <flox/execution/order_tracker.h>
#include <flox/log/log.h>

#include <simdjson.h>

namespace flox
{

template <typename Policies>
BitgetOrderExecutorT<Policies>::~BitgetOrderExecutorT() = default;

static constexpr std::string_view kPathPlace = "/api/v2/mix/order/place-order";
static constexpr std::string_view kPathPlan = "/api/v2/mix/order/place-plan-order";
static constexpr std::string_view kPathCancel = "/api/v2/mix/order/cancel-order";
static constexpr std::string_view kPathCancelPlan = "/api/v2/mix/order/cancel-plan-order";
static constexpr std::string_view kPathModify = "/api/v2/mix/order/modify-order";
static constexpr std::string_view kPathSetLeverage = "/api/v2/mix/account/set-leverage";

template <typename Policies>
void BitgetOrderExecutorT<Policies>::setLeverage(const std::string& symbol, int leverage)
{
  std::string body;
  body.reserve(128);
  body.append("{\"symbol\":\"")
      .append(symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",")
      .append("\"leverage\":\"")
      .append(std::to_string(leverage))
      .append("\"}");

  _client->post(
      std::string(kPathSetLeverage), body,
      [symbol, leverage](std::string_view resp)
      {
        simdjson::ondemand::parser p;
        simdjson::padded_string ps(resp);
        auto doc = p.iterate(ps);
        if (std::string_view(doc["code"]) != "00000")
        {
          FLOX_LOG_ERROR("[BitgetOE] setLeverage failed for " << symbol << ": "
                                                              << doc["msg"].get_string().value());
        }
        else
        {
          FLOX_LOG_INFO("[BitgetOE] Leverage set to " << leverage << "x for " << symbol);
        }
      },
      [symbol](std::string_view err)
      {
        FLOX_LOG_ERROR("[BitgetOE] setLeverage transport: " << err);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::submitOrderWithLeverage(const Order& order, int leverage,
                                                             double slPrice, double tpPrice)
{
  auto info = _registry->getSymbolInfo(order.symbol);
  if (!info)
  {
    FLOX_LOG_ERROR("[BitgetOE] submitOrderWithLeverage: unknown symbolId=" << order.symbol);
    publishRejection(order, "unknown symbol");
    return;
  }

  std::string levBody;
  levBody.reserve(128);
  levBody.append("{\"symbol\":\"")
      .append(info->symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",")
      .append("\"leverage\":\"")
      .append(std::to_string(leverage))
      .append("\"}");

  _client->post(
      std::string(kPathSetLeverage), levBody,
      [this, order, leverage, slPrice, tpPrice](std::string_view resp)
      {
        simdjson::ondemand::parser p;
        simdjson::padded_string ps(resp);
        auto doc = p.iterate(ps);
        if (std::string_view(doc["code"]) != "00000")
        {
          auto msg = std::string(doc["msg"].get_string().value());
          FLOX_LOG_ERROR("[BitgetOE] setLeverage failed: " << msg);
          publishRejection(order, "setLeverage failed: " + msg);
          return;
        }
        FLOX_LOG_INFO("[BitgetOE] Leverage " << leverage << "x, submitting order");

        if (slPrice <= 0 && tpPrice <= 0)
        {
          submitOrder(order);
          return;
        }

        auto info2 = _registry->getSymbolInfo(order.symbol);
        if (!info2)
        {
          publishRejection(order, "unknown symbol");
          return;
        }

        bool isMarket = (order.type == OrderType::MARKET);
        std::string_view tradeSide = order.flags.reduceOnly ? "close" : "open";

        std::string body;
        body.reserve(384);
        body.append("{\"symbol\":\"")
            .append(info2->symbol)
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
            .append("\",");

        if (!isMarket)
        {
          body.append("\"price\":\"").append(order.price.toString()).append("\",");
        }

        body.append("\"side\":\"")
            .append(order.side == Side::BUY ? "buy" : "sell")
            .append("\",")
            .append("\"tradeSide\":\"")
            .append(tradeSide)
            .append("\",")
            .append("\"orderType\":\"")
            .append(isMarket ? "market" : "limit")
            .append("\",");

        if (!isMarket)
        {
          body.append("\"force\":\"").append(_params.forcePolicy).append("\",");
        }

        if (slPrice > 0)
        {
          body.append("\"presetStopLossPrice\":\"")
              .append(Price::fromDouble(slPrice).toString())
              .append("\",");
        }
        if (tpPrice > 0)
        {
          body.append("\"presetStopSurplusPrice\":\"")
              .append(Price::fromDouble(tpPrice).toString())
              .append("\",");
        }

        body.append("\"clientOid\":\"").append(std::to_string(order.id)).append("\"}");

        _policies.timeout.trackSubmit(order.id);

        _client->post(
            std::string(kPathPlace), body,
            [this, order](std::string_view resp2)
            {
              _policies.timeout.clearPending(order.id);
              simdjson::ondemand::parser p2;
              simdjson::padded_string ps2(resp2);
              auto doc2 = p2.iterate(ps2);
              if (std::string_view(doc2["code"]) != "00000")
              {
                auto msg = std::string(doc2["msg"].get_string().value());
                FLOX_LOG_ERROR("[BitgetOE] submitOrder failed: " << msg);
                publishRejection(order, msg);
                return;
              }
              std::string exchId = std::string(doc2["data"]["orderId"].get_string().value());
              _orderTracker->onSubmitted(order, exchId);
            },
            [this, order](std::string_view err2)
            {
              _policies.timeout.clearPending(order.id);
              std::string msg(err2);
              FLOX_LOG_ERROR("[BitgetOE] submitOrder transport: " << msg);
              publishRejection(order, msg);
            });
      },
      [this, order](std::string_view err)
      {
        std::string msg(err);
        FLOX_LOG_ERROR("[BitgetOE] setLeverage transport: " << msg);
        publishRejection(order, "setLeverage transport: " + msg);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::publishRejection(const Order& order, const std::string& reason)
{
  _orderTracker->onRejected(order.id, reason);
  if (_orderBus)
  {
    OrderEvent ev;
    ev.status = OrderEventStatus::REJECTED;
    ev.order = order;
    ev.rejectReason = reason;
    _orderBus->publish(std::move(ev));
  }
}

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
    publishRejection(order, "unknown symbol");
    return;
  }

  bool isPlan =
      (order.type == OrderType::STOP_MARKET || order.type == OrderType::TAKE_PROFIT_MARKET ||
       order.type == OrderType::STOP_LIMIT || order.type == OrderType::TAKE_PROFIT_LIMIT);

  if (isPlan)
  {
    submitPlanOrder(order, *info);
    return;
  }

  bool isMarket = (order.type == OrderType::MARKET);
  std::string_view tradeSide = order.flags.reduceOnly ? "close" : "open";

  std::string body;
  body.reserve(256);
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
      .append("\",");

  if (!isMarket)
  {
    body.append("\"price\":\"").append(order.price.toString()).append("\",");
  }

  body.append("\"side\":\"")
      .append(order.side == Side::BUY ? "buy" : "sell")
      .append("\",")
      .append("\"tradeSide\":\"")
      .append(tradeSide)
      .append("\",")
      .append("\"orderType\":\"")
      .append(isMarket ? "market" : "limit")
      .append("\",");

  if (!isMarket)
  {
    body.append("\"force\":\"").append(_params.forcePolicy).append("\",");
  }

  body.append("\"clientOid\":\"").append(std::to_string(order.id)).append("\"}");

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
          auto msg = std::string(doc["msg"].get_string().value());
          FLOX_LOG_ERROR("[BitgetOE] submitOrder failed: " << msg);
          publishRejection(order, msg);
          return;
        }

        std::string exchId = std::string(doc["data"]["orderId"].get_string().value());
        _orderTracker->onSubmitted(order, exchId);
      },
      [this, order](std::string_view err)
      {
        _policies.timeout.clearPending(order.id);
        std::string msg(err);
        FLOX_LOG_ERROR("[BitgetOE] submitOrder transport: " << msg);
        publishRejection(order, msg);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::submitPlanOrder(const Order& order, const SymbolInfo& info)
{
  std::string_view tradeSide = order.flags.reduceOnly ? "close" : "open";

  std::string body;
  body.reserve(320);
  body.append("{\"planType\":\"normal_plan\",")
      .append("\"symbol\":\"")
      .append(info.symbol)
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
      .append("\"triggerPrice\":\"")
      .append(order.triggerPrice.toString())
      .append("\",")
      .append("\"triggerType\":\"mark_price\",")
      .append("\"side\":\"")
      .append(order.side == Side::BUY ? "buy" : "sell")
      .append("\",")
      .append("\"tradeSide\":\"")
      .append(tradeSide)
      .append("\",")
      .append("\"orderType\":\"market\",")
      .append("\"clientOid\":\"")
      .append(std::to_string(order.id))
      .append("\"}");

  _policies.timeout.trackSubmit(order.id);

  _client->post(
      std::string(kPathPlan), body,
      [this, order](std::string_view resp)
      {
        _policies.timeout.clearPending(order.id);

        simdjson::ondemand::parser p;
        simdjson::padded_string ps(resp);
        auto doc = p.iterate(ps);
        if (std::string_view(doc["code"]) != "00000")
        {
          auto msg = std::string(doc["msg"].get_string().value());
          FLOX_LOG_ERROR("[BitgetOE] submitPlanOrder failed: " << msg);
          publishRejection(order, msg);
          return;
        }

        std::string exchId = std::string(doc["data"]["orderId"].get_string().value());
        _orderTracker->onSubmitted(order, exchId);
      },
      [this, order](std::string_view err)
      {
        _policies.timeout.clearPending(order.id);
        std::string msg(err);
        FLOX_LOG_ERROR("[BitgetOE] submitPlanOrder transport: " << msg);
        publishRejection(order, msg);
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

  bool isPlan = (st->localOrder.type == OrderType::STOP_MARKET ||
                 st->localOrder.type == OrderType::TAKE_PROFIT_MARKET ||
                 st->localOrder.type == OrderType::STOP_LIMIT ||
                 st->localOrder.type == OrderType::TAKE_PROFIT_LIMIT);

  std::string_view endpoint = isPlan ? kPathCancelPlan : kPathCancel;

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
      std::string(endpoint), body,
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
