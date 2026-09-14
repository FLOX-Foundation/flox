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
#include "flox-connectors/execution/order_tif.h"

#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/events/order_event.h>
#include <flox/execution/order_tracker.h>
#include <flox/log/log.h>

#include <simdjson.h>

namespace flox
{

template <typename Policies>
BybitOrderExecutorT<Policies>::~BybitOrderExecutorT() = default;

namespace
{

// Bybit V5's tif token spelling; "" means the tif has no wire representation
// on this venue (GTD) and the caller must reject before building a body.
std::string_view bybitTifToken(NormalizedTif tif)
{
  switch (tif)
  {
    case NormalizedTif::GTC:
      return "GTC";
    case NormalizedTif::IOC:
      return "IOC";
    case NormalizedTif::FOK:
      return "FOK";
    case NormalizedTif::POST_ONLY:
      return "PostOnly";
    case NormalizedTif::UNSUPPORTED:
      return "";
  }
  return "";
}

// Standard stop/take-profit trigger-direction convention (rise=1, fall=2):
// a stop is placed on the far side of the current price from a favourable
// move, a take-profit on the near side. Independent of Bybit-account state,
// unlike the tradeSide/posMode question elsewhere in this file -- this is just "which way
// does the trigger price sit relative to the position being protected".
int triggerDirectionFor(OrderType type, Side side)
{
  const bool isStop = (type == OrderType::STOP_MARKET || type == OrderType::STOP_LIMIT);
  const bool sellSide = (side == Side::SELL);
  if (isStop)
  {
    return sellSide ? 2 : 1;  // sell-stop fires on the way down, buy-stop on the way up
  }
  return sellSide ? 1 : 2;  // sell take-profit fires on the way up, buy on the way down
}

bool isConditionalOrder(OrderType type)
{
  return type == OrderType::STOP_MARKET || type == OrderType::STOP_LIMIT ||
         type == OrderType::TAKE_PROFIT_MARKET || type == OrderType::TAKE_PROFIT_LIMIT;
}

}  // namespace

template <typename Policies>
void BybitOrderExecutorT<Policies>::publishRejection(const Order& order, const std::string& reason)
{
  FLOX_LOG_ERROR("[BybitOrderExecutor] " << reason << " (orderId=" << order.id << ")");
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
void BybitOrderExecutorT<Policies>::publishRateLimited(const Order& order)
{
  if (_orderBus)
  {
    OrderEvent ev;
    ev.status = OrderEventStatus::REJECTED_RATE_LIMIT;
    ev.order = order;
    ev.rejectReason = "client-side rate limit";
    _orderBus->publish(std::move(ev));
  }
}

template <typename Policies>
void BybitOrderExecutorT<Policies>::submitOrder(const Order& order)
{
  if (!_policies.rateLimit.tryAcquire(order.id,
                                      [this, &order]
                                      {
                                        publishRateLimited(order);
                                      }))
  {
    return;
  }

  auto info = _registry->getSymbolInfo(order.symbol);
  if (!info.has_value())
  {
    publishRejection(order, "No symbol info registered");
    return;
  }

  if (order.type == OrderType::TRAILING_STOP)
  {
    // Bybit's trailing stop is a position-attached parameter on
    // /v5/position/trading-stop, not an order/create body -- there is no
    // wire shape here that would make this a real trailing stop rather
    // than a resting limit the strategy mistakes for one. Reject loudly
    // instead of sending something that looks accepted but protects
    // nothing.
    publishRejection(order, "Bybit trailing stop is not supported via order/create");
    return;
  }
  if (order.type == OrderType::ICEBERG)
  {
    publishRejection(order, "Bybit iceberg (visible-quantity) orders are not supported");
    return;
  }

  const NormalizedTif tif = normalizeTif(order.timeInForce, order.flags.postOnly);
  if (tif == NormalizedTif::UNSUPPORTED)
  {
    publishRejection(order, "Bybit has no native good-till-date order");
    return;
  }

  const bool isConditional = isConditionalOrder(order.type);
  const bool priceIsLimit =
      (order.type == OrderType::LIMIT || order.type == OrderType::STOP_LIMIT ||
       order.type == OrderType::TAKE_PROFIT_LIMIT);

  std::string body;
  body.reserve(256);
  body.append("{\"category\":\"").append(Bybit::toString(info->type)).append("\",");
  body.append("\"symbol\":\"").append(info->symbol).append("\",");
  body.append("\"side\":\"").append(order.side == Side::BUY ? "Buy" : "Sell").append("\",");
  body.append("\"orderType\":\"").append(priceIsLimit ? "Limit" : "Market").append("\",");
  body.append("\"qty\":\"").append(order.quantity.toString()).append("\",");
  if (priceIsLimit)
  {
    body.append("\"price\":\"").append(order.price.toString()).append("\",");
  }
  body.append("\"timeInForce\":\"").append(bybitTifToken(tif)).append("\",");
  body.append("\"reduceOnly\":").append(order.flags.reduceOnly ? "true" : "false");
  if (isConditional)
  {
    body.append(",\"triggerPrice\":\"").append(order.triggerPrice.toString()).append("\"");
    body.append(",\"triggerDirection\":")
        .append(std::to_string(triggerDirectionFor(order.type, order.side)));
  }
  body.append("}");

  FLOX_LOG("[BybitOrderExecutor] Submitting order: id="
           << order.id << " symbol=" << info->symbol << " side="
           << (order.side == Side::BUY ? "Buy" : "Sell") << " qty=" << order.quantity.toDouble()
           << " price=" << order.price.toDouble() << " category=" << Bybit::toString(info->type));

  _policies.timeout.trackSubmit(order.id);

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
          // clearPending() runs AFTER the retCode check now: on the old
          // ordering, a rejected submit disarmed the timeout watchdog in
          // the same line that discovered the rejection, so the one
          // safety net meant to catch a hung exchange never had a chance
          // to fire on a hung-then-erroring one either.
          publishRejection(order, std::string("Order submission failed: retCode=") +
                                      std::to_string(retCode) + " retMsg=" + std::string(retMsg));
          _policies.timeout.clearPending(order.id);
          return;
        }

        std::string_view orderId = doc["result"]["orderId"].get_string().value();

        FLOX_LOG("[BybitOrderExecutor] Order submitted. id=" << order.id
                                                             << " → exchangeOrderId=" << orderId);

        _orderTracker->onSubmitted(order, orderId);
        _policies.timeout.clearPending(order.id);
      },
      [this, order](std::string_view error)
      {
        publishRejection(order, std::string("Transport error: ") + std::string(error));
        _policies.timeout.clearPending(order.id);
      });
}

template <typename Policies>
void BybitOrderExecutorT<Policies>::cancelOrder(OrderId orderId)
{
  auto state = _orderTracker->get(orderId);
  if (!state)
  {
    FLOX_LOG_ERROR("[BybitOrderExecutor] Cannot cancel, unknown orderId=" << orderId);
    return;
  }

  // Resolved before the rate-limit check (rather than after, as submit/
  // replace check first) so a rejected cancel can still be reported against
  // the order it was trying to cancel: a silently dropped cancel
  // is worse than a silently dropped submit, since the tracker keeps
  // reporting the order active with no hint that the cancel never left the
  // process.
  if (!_policies.rateLimit.tryAcquire(orderId,
                                      [this, &state]
                                      {
                                        publishRateLimited(state->localOrder);
                                      }))
  {
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

  _policies.timeout.trackCancel(orderId);

  _client->post(
      "/v5/order/cancel", body,
      [this, orderId](std::string_view response)
      {
        _policies.timeout.clearPending(orderId);

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
      [this, orderId](std::string_view err)
      {
        _policies.timeout.clearPending(orderId);
        FLOX_LOG_ERROR("[BybitOrderExecutor] Cancel transport error: orderId=" << orderId
                                                                               << " err=" << err);
      });
}

template <typename Policies>
void BybitOrderExecutorT<Policies>::replaceOrder(OrderId oldOrderId, const Order& newOrder)
{
  auto info = _registry->getSymbolInfo(newOrder.symbol);
  if (!info.has_value())
  {
    FLOX_LOG_ERROR("[BybitOrderExecutor] No symbol info for symbolId=" << newOrder.symbol);
    return;
  }

  auto state = _orderTracker->get(oldOrderId);
  if (!state)
  {
    FLOX_LOG_ERROR("[BybitOrderExecutor] Cannot replace, unknown orderId=" << oldOrderId);
    return;
  }

  if (!_policies.rateLimit.tryAcquire(oldOrderId,
                                      [this, &state]
                                      {
                                        publishRateLimited(state->localOrder);
                                      }))
  {
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

  _policies.timeout.trackReplace(oldOrderId);

  _client->post(
      "/v5/order/amend", body,
      [this, oldOrderId, newOrder, exchangeOrderId](std::string_view response)
      {
        _policies.timeout.clearPending(oldOrderId);

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(response);
        auto doc = parser.iterate(padded);

        int64_t retCode = doc["retCode"].get_int64().value();
        if (retCode != 0)
        {
          std::string_view msg = doc["retMsg"].get_string().value();
          FLOX_LOG_ERROR("[BybitOrderExecutor] Replace failed: orderId="
                         << oldOrderId << " retCode=" << retCode << " msg=" << msg);
          return;
        }

        // The amend response carries the (unchanged) exchange order id in
        // result.orderId; onReplaced() used to be called with a literal ""
        // here instead of reading it, so the tracker's new record had no
        // exchange id at all -- a subsequent cancelOrder() built
        // {"orderId":""}, the venue rejected it, and that rejection was
        // itself swallowed, leaving the order live on the
        // exchange and locally stuck "active" forever.
        std::string_view newExchangeId = exchangeOrderId;
        auto resultOrderId = doc["result"]["orderId"];
        if (!resultOrderId.error())
        {
          auto s = resultOrderId.get_string();
          if (!s.error())
          {
            newExchangeId = s.value_unsafe();
          }
        }

        FLOX_LOG_INFO("[BybitOrderExecutor] Replace successful: orderId=" << oldOrderId);
        _orderTracker->onReplaced(oldOrderId, newOrder, newExchangeId);
      },
      [this, oldOrderId](std::string_view err)
      {
        _policies.timeout.clearPending(oldOrderId);
        FLOX_LOG_ERROR("[BybitOrderExecutor] Replace transport error: orderId=" << oldOrderId
                                                                                << " err=" << err);
      });
}

// Explicit instantiations
template class BybitOrderExecutorT<NoPolicies>;
template class BybitOrderExecutorT<WithRateLimit>;
template class BybitOrderExecutorT<WithTimeout>;
template class BybitOrderExecutorT<FullPolicies>;

}  // namespace flox
