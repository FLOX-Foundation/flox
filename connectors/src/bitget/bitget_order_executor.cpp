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
#include "flox-connectors/execution/order_tif.h"

#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/events/order_event.h>
#include <flox/execution/order_tracker.h>
#include <flox/log/log.h>

#include <simdjson.h>

#include <cmath>
#include <cstdio>

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
static constexpr std::string_view kPathPosTpsl = "/api/v2/mix/order/place-pos-tpsl";
static constexpr std::string_view kPathModifyTpsl = "/api/v2/mix/order/modify-tpsl-order";

// Format a double as a fixed-point decimal with up to `max_decimals` fractional
// digits, rounded half-to-even, trailing zeros stripped. Bitget validates
// prices against the symbol's own precision and rejects values like
// "73577.65" on an instrument that quotes to one digit, even though they
// parse to the same number -- so the digit count is a property of the
// instrument and must come from the registry (decimalsForTick below), never
// from a default.
static std::string trimDouble(double v, int max_decimals)
{
  // Round to max_decimals first.
  double scale = 1.0;
  for (int i = 0; i < max_decimals; ++i)
  {
    scale *= 10.0;
  }
  double rounded = std::round(v * scale) / scale;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", max_decimals, rounded);
  std::string s(buf);
  if (s.find('.') != std::string::npos)
  {
    while (!s.empty() && s.back() == '0')
    {
      s.pop_back();
    }
    if (!s.empty() && s.back() == '.')
    {
      s.pop_back();
    }
  }
  return s;
}

// Fractional digits the Price type itself can carry (its scale is a power of
// ten), which is the cap on anything formatted below.
static constexpr int priceDecimals()
{
  int digits = 0;
  for (int64_t scale = Price::Scale; scale > 1; scale /= 10)
  {
    ++digits;
  }
  return digits;
}

static constexpr int kPriceDecimals = priceDecimals();

// How many fractional digits this instrument quotes in, from the tick size the
// registry already holds: 0.1 -> 1, 0.0001 -> 4, 1e-8 -> 8. Every trigger and
// limit price this connector sends used to be formatted with one digit, an
// assumption spelled out in trimDouble's own comment ("BTC perp = 1 digit"),
// which moved a protective stop on anything finer and sent "0" for a symbol
// priced below 0.05.
static int decimalsForTick(Price tickSize)
{
  int64_t raw = tickSize.raw();
  if (raw <= 0)
  {
    // No usable tick in the registry: send everything the fixed-point type can
    // carry rather than silently rounding the strategy's price away.
    return kPriceDecimals;
  }

  int decimals = kPriceDecimals;
  while (decimals > 0 && raw % 10 == 0)
  {
    raw /= 10;
    --decimals;
  }
  return decimals;
}

namespace
{

std::string_view bitgetForceToken(NormalizedTif tif)
{
  switch (tif)
  {
    case NormalizedTif::GTC:
      return "gtc";
    case NormalizedTif::IOC:
      return "ioc";
    case NormalizedTif::FOK:
      return "fok";
    case NormalizedTif::POST_ONLY:
      return "post_only";
    case NormalizedTif::UNSUPPORTED:
      return "";
  }
  return "";
}

// tradeSide/posSide only get sent once the account's position mode is
// actually known. Sending them unconditionally off reduceOnly alone (the
// pre-fix behaviour) meant the field's meaning depended on an account
// setting this connector never tracked.
void appendPositionFields(std::string& body, const Order& order, const Bitget::Params& params)
{
  if (params.positionMode != Bitget::PositionMode::Hedge)
  {
    return;
  }
  body.append("\"tradeSide\":\"").append(order.flags.reduceOnly ? "close" : "open").append("\",");
  const auto holdSide = static_cast<HoldSide>(order.flags.holdSide);
  if (holdSide == HoldSide::Long)
  {
    body.append("\"posSide\":\"long\",");
  }
  else if (holdSide == HoldSide::Short)
  {
    body.append("\"posSide\":\"short\",");
  }
}

}  // namespace

// setLeverage, submitOrderWithLeverage, placePosTpsl and modifyPosTpsl are
// ordinary REST requests against the same venue budget as a submit, and the
// trailing stop walks modifyPosTpsl on every bar -- the paths that used to
// skip the limiter were the ones that fire most often. Neither setLeverage
// nor modifyPosTpsl carries an OrderId to report against, so a refusal is a
// log line rather than an event; kNoOrderId only names that in the log.
static constexpr OrderId kNoOrderId = 0;

template <typename Policies>
void BitgetOrderExecutorT<Policies>::setLeverage(const std::string& symbol, int leverage)
{
  _policies.rateLimit.gate(
      kNoOrderId,
      [this, symbol, leverage]
      {
        sendSetLeverage(symbol, leverage);
      },
      [symbol]
      {
        FLOX_LOG_WARN("[BitgetOE] setLeverage for " << symbol
                                                    << " refused by the client-side rate limit");
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::sendSetLeverage(const std::string& symbol, int leverage)
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
  _policies.rateLimit.gate(
      order.id,
      [this, order, leverage, slPrice, tpPrice]
      {
        sendSubmitOrderWithLeverage(order, leverage, slPrice, tpPrice);
      },
      [this, order]
      {
        publishRateLimited(order);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::sendSubmitOrderWithLeverage(const Order& order, int leverage,
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

        if (order.type == OrderType::TRAILING_STOP || order.type == OrderType::ICEBERG)
        {
          publishRejection(order, "order type not implemented by this connector");
          return;
        }

        bool isMarket = (order.type == OrderType::MARKET);
        const NormalizedTif tif = normalizeTif(order.timeInForce, order.flags.postOnly);
        if (!isMarket && tif == NormalizedTif::UNSUPPORTED)
        {
          publishRejection(order, "Bitget has no native good-till-date order");
          return;
        }

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

        body.append("\"side\":\"").append(order.side == Side::BUY ? "buy" : "sell").append("\",");

        appendPositionFields(body, order, _params);

        body.append("\"orderType\":\"").append(isMarket ? "market" : "limit").append("\",");

        if (!isMarket)
        {
          body.append("\"force\":\"").append(bitgetForceToken(tif)).append("\",");
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

// A client-side rate-limit rejection never reached the venue and left no
// trace anywhere -- the tracker (if any record existed) kept reporting the
// order active with no signal that a cancel or replace silently never left
// the process. Deliberately does not touch OrderTracker: unlike
// publishRejection, there was no submission attempt to mark rejected.
template <typename Policies>
void BitgetOrderExecutorT<Policies>::publishRateLimited(const Order& order)
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
void BitgetOrderExecutorT<Policies>::submitOrder(const Order& order)
{
  _policies.rateLimit.gate(
      order.id,
      [this, order]
      {
        sendSubmitOrder(order);
      },
      [this, order]
      {
        publishRateLimited(order);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::sendSubmitOrder(const Order& order)
{
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

  if (order.type == OrderType::TRAILING_STOP)
  {
    publishRejection(order, "Bitget trailing stop is not implemented by this connector");
    return;
  }
  if (order.type == OrderType::ICEBERG)
  {
    publishRejection(order, "Bitget iceberg (visible-quantity) orders are not implemented");
    return;
  }

  bool isMarket = (order.type == OrderType::MARKET);

  const NormalizedTif tif = normalizeTif(order.timeInForce, order.flags.postOnly);
  if (!isMarket && tif == NormalizedTif::UNSUPPORTED)
  {
    publishRejection(order, "Bitget has no native good-till-date order");
    return;
  }

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

  body.append("\"side\":\"").append(order.side == Side::BUY ? "buy" : "sell").append("\",");

  appendPositionFields(body, order, _params);

  body.append("\"orderType\":\"").append(isMarket ? "market" : "limit").append("\",");

  if (!isMarket)
  {
    // force carries the order's own timeInForce/postOnly now, not a static
    // config value that ignored per-order intent.
    body.append("\"force\":\"").append(bitgetForceToken(tif)).append("\",");
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
  const int decimals = decimalsForTick(info.tickSize);

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
      .append(trimDouble(order.triggerPrice.toDouble(), decimals))
      .append("\",")
      .append("\"triggerType\":\"mark_price\",")
      .append("\"side\":\"")
      .append(order.side == Side::BUY ? "buy" : "sell")
      .append("\",");

  appendPositionFields(body, order, _params);

  body.append("\"orderType\":\"market\",");

  body.append("\"clientOid\":\"").append(std::to_string(order.id)).append("\"}");

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
  // Looked up before the rate-limit gate so a refused cancel can still be
  // reported against the order it targeted instead of vanishing with no event
  // while the tracker keeps reporting the order active. The send path reads
  // the tracker again: a deferred cancel runs later, and the order it names
  // may have moved on in the meantime.
  auto st = _orderTracker->get(id);
  if (!st)
  {
    FLOX_LOG_ERROR("[BitgetOE] cancelOrder: unknown id=" << id);
    return;
  }

  Order target = st->localOrder;
  _policies.rateLimit.gate(
      id,
      [this, id]
      {
        sendCancelOrder(id);
      },
      [this, target]
      {
        publishRateLimited(target);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::sendCancelOrder(OrderId id)
{
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
  auto st = _orderTracker->get(oldId);
  if (!st)
  {
    FLOX_LOG_ERROR("[BitgetOE] replaceOrder: unknown id=" << oldId);
    return;
  }

  Order target = st->localOrder;
  _policies.rateLimit.gate(
      oldId,
      [this, oldId, newOrd]
      {
        sendReplaceOrder(oldId, newOrd);
      },
      [this, target]
      {
        publishRateLimited(target);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::sendReplaceOrder(OrderId oldId, const Order& newOrd)
{
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

template <typename Policies>
void BitgetOrderExecutorT<Policies>::placePosTpsl(SymbolId symbol, HoldSide holdSide,
                                                  double slPrice, double tpPrice, OrderId localId)
{
  _policies.rateLimit.gate(
      localId,
      [this, symbol, holdSide, slPrice, tpPrice, localId]
      {
        sendPlacePosTpsl(symbol, holdSide, slPrice, tpPrice, localId);
      },
      [this, symbol, localId]
      {
        // A protective stop that never left the process is worth an event:
        // the position is unprotected and only this connector knows it.
        Order refused;
        refused.id = localId;
        refused.symbol = symbol;
        publishRateLimited(refused);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::sendPlacePosTpsl(SymbolId symbol, HoldSide holdSide,
                                                      double slPrice, double tpPrice,
                                                      OrderId localId)
{
  auto info = _registry->getSymbolInfo(symbol);
  if (!info)
  {
    FLOX_LOG_ERROR("[BitgetOE] placePosTpsl: unknown symbolId=" << symbol);
    return;
  }
  if (holdSide == HoldSide::Unspecified)
  {
    FLOX_LOG_ERROR("[BitgetOE] placePosTpsl: holdSide must be Long or Short");
    return;
  }

  std::string body;
  body.reserve(384);
  body.append("{\"symbol\":\"")
      .append(info->symbol)
      .append("\",")
      .append("\"productType\":\"")
      .append(_params.productType)
      .append("\",")
      .append("\"marginCoin\":\"")
      .append(_params.marginCoin)
      .append("\",")
      .append("\"holdSide\":\"")
      .append(holdSide == HoldSide::Long ? "long" : "short")
      .append("\",")
      .append("\"planType\":\"")
      .append(slPrice > 0 && tpPrice > 0 ? "profit_loss"
              : slPrice > 0              ? "pos_loss"
                                         : "pos_profit")
      .append("\",");
  const int decimals = decimalsForTick(info->tickSize);
  if (slPrice > 0)
  {
    body.append("\"stopLossTriggerPrice\":\"")
        .append(trimDouble(slPrice, decimals))
        .append("\",")
        .append("\"stopLossTriggerType\":\"mark_price\",");
  }
  if (tpPrice > 0)
  {
    body.append("\"stopSurplusTriggerPrice\":\"")
        .append(trimDouble(tpPrice, decimals))
        .append("\",")
        .append("\"stopSurplusTriggerType\":\"mark_price\",");
  }
  body.append("\"clientOid\":\"").append(std::to_string(localId)).append("\"}");

  _client->post(
      std::string(kPathPosTpsl), body,
      [this, localId](std::string_view resp)
      {
        simdjson::ondemand::parser p;
        simdjson::padded_string ps(resp);
        simdjson::ondemand::document doc;
        if (p.iterate(ps).get(doc))
        {
          FLOX_LOG_ERROR("[BitgetOE] placePosTpsl: bad json");
          return;
        }
        std::string_view code;
        if (doc["code"].get_string().get(code) || code != "00000")
        {
          std::string_view msg;
          (void)doc["msg"].get_string().get(msg);
          FLOX_LOG_ERROR("[BitgetOE] placePosTpsl failed: " << std::string(msg));
          return;
        }
        // /data is an array; extract first orderId.
        simdjson::ondemand::array arr;
        if (doc["data"].get_array().get(arr))
        {
          return;
        }
        for (auto el : arr)
        {
          std::string_view oid;
          if (el["orderId"].get_string().get(oid))
          {
            continue;
          }
          Order tmp;
          tmp.id = localId;
          _orderTracker->onSubmitted(tmp, std::string(oid));
          break;
        }
      },
      [](std::string_view err)
      {
        FLOX_LOG_ERROR("[BitgetOE] placePosTpsl transport: " << err);
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::modifyPosTpsl(SymbolId symbol,
                                                   const std::string& exchangeOrderId,
                                                   double newTriggerPrice, double qty)
{
  _policies.rateLimit.gate(
      kNoOrderId,
      [this, symbol, exchangeOrderId, newTriggerPrice, qty]
      {
        sendModifyPosTpsl(symbol, exchangeOrderId, newTriggerPrice, qty);
      },
      [exchangeOrderId]
      {
        FLOX_LOG_WARN("[BitgetOE] modifyPosTpsl for "
                      << exchangeOrderId
                      << " refused by the client-side rate limit: the stop "
                         "stays where the venue last accepted it");
      });
}

template <typename Policies>
void BitgetOrderExecutorT<Policies>::sendModifyPosTpsl(SymbolId symbol,
                                                       const std::string& exchangeOrderId,
                                                       double newTriggerPrice, double qty)
{
  auto info = _registry->getSymbolInfo(symbol);
  if (!info)
  {
    return;
  }

  std::string body;
  body.reserve(256);
  body.append("{\"orderId\":\"")
      .append(exchangeOrderId)
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
      .append("\"size\":\"")
      .append(trimDouble(qty, /*max_decimals=*/4))
      .append("\",")
      .append("\"triggerPrice\":\"")
      .append(trimDouble(newTriggerPrice, decimalsForTick(info->tickSize)))
      .append("\",")
      .append("\"triggerType\":\"mark_price\"}");

  _client->post(
      std::string(kPathModifyTpsl), body,
      [exchangeOrderId, newTriggerPrice](std::string_view resp)
      {
        simdjson::ondemand::parser p;
        simdjson::padded_string ps(resp);
        simdjson::ondemand::document doc;
        if (p.iterate(ps).get(doc))
        {
          return;
        }
        std::string_view code;
        if (doc["code"].get_string().get(code) || code != "00000")
        {
          std::string_view msg;
          (void)doc["msg"].get_string().get(msg);
          FLOX_LOG_ERROR("[BitgetOE] modifyPosTpsl failed: " << std::string(msg)
                                                             << " (orderId=" << exchangeOrderId
                                                             << " new=" << newTriggerPrice << ")");
        }
      },
      [](std::string_view err)
      {
        FLOX_LOG_ERROR("[BitgetOE] modifyPosTpsl transport: " << err);
      });
}

// Explicit instantiations
template class BitgetOrderExecutorT<NoPolicies>;
template class BitgetOrderExecutorT<WithRateLimit>;
template class BitgetOrderExecutorT<WithTimeout>;
template class BitgetOrderExecutorT<FullPolicies>;

}  // namespace flox
