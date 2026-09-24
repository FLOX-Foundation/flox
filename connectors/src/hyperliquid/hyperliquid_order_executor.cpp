/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/hyperliquid/hyperliquid_order_executor.h"
#include "flox-connectors/execution/order_tif.h"
#include "flox-connectors/hyperliquid/hl_signer.h"
#include "flox-connectors/net/curl_transport.h"
#include "flox-connectors/util/safe_parse.h"

#include <flox/execution/events/order_event.h>
#include <flox/log/log.h>

#include <simdjson.h>

#include <chrono>
#include <cstdio>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace flox
{

template <typename Policies>
HyperliquidOrderExecutorT<Policies>::~HyperliquidOrderExecutorT() = default;

namespace
{

inline int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string tidy(double v, int prec)
{
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
  std::string s(buf);
  while (!s.empty() && s.back() == '0')
  {
    s.pop_back();
  }
  if (!s.empty() && s.back() == '.')
  {
    s.pop_back();
  }
  return s;
}

std::string genCloid128()
{
  std::random_device rd;
  std::uniform_int_distribution<uint64_t> dist;
  uint64_t hi = dist(rd);
  uint64_t lo = dist(rd);

  std::ostringstream oss;
  oss << "0x" << std::hex << std::setfill('0') << std::setw(16) << hi << std::setw(16) << lo;
  return oss.str();
}

// Hyperliquid's tif token spelling for the "limit" order-type object. Alo =
// Add Liquidity Only, HL's post-only. There is no native FOK or
// good-till-date tif on this venue.
std::string_view hlTifToken(NormalizedTif tif)
{
  switch (tif)
  {
    case NormalizedTif::GTC:
      return "Gtc";
    case NormalizedTif::IOC:
      return "Ioc";
    case NormalizedTif::POST_ONLY:
      return "Alo";
    case NormalizedTif::FOK:
    case NormalizedTif::UNSUPPORTED:
      return "";
  }
  return "";
}

// This connector only ever builds HL's "limit" order-type object (used for
// both resting limit orders and, per HL's own SDK convention, IOC-priced
// market orders). Stop/take-profit/trailing need HL's separate "trigger"
// order shape and trailing has no server-side primitive on HL at all; none
// of that is implemented here. Returns the tif token to send, or empty if
// the order should be rejected outright -- silently sending an
// unsupported order type as a plain resting Gtc limit used to make
// a strategy believe a stop or an IOC/reduce-only
// order exists on the venue when what actually rests there is neither).
std::optional<std::string_view> hlOrderTifOrReject(const Order& order)
{
  if (order.type == OrderType::STOP_MARKET || order.type == OrderType::STOP_LIMIT ||
      order.type == OrderType::TAKE_PROFIT_MARKET || order.type == OrderType::TAKE_PROFIT_LIMIT ||
      order.type == OrderType::TRAILING_STOP || order.type == OrderType::ICEBERG)
  {
    return std::nullopt;
  }

  if (order.type == OrderType::MARKET)
  {
    // HL has no distinct market-order primitive; the standard SDK
    // convention is an IOC limit at a slippage-adjusted price, which the
    // caller already supplied via order.price.
    return "Ioc";
  }

  const NormalizedTif tif = normalizeTif(order.timeInForce, order.flags.postOnly);
  const auto token = hlTifToken(tif);
  if (token.empty())
  {
    return std::nullopt;
  }
  return token;
}

}  // namespace

template <typename Policies>
HyperliquidOrderExecutorT<Policies>::HyperliquidOrderExecutorT(
    std::string url, std::string privateKey, SymbolRegistry* registry, OrderTracker* orderTracker,
    std::shared_ptr<ILogger> logger, std::string accountAddress,
    std::optional<std::string> vaultAddress, bool mainnet)
    : _url(std::move(url)),
      _privateKey(std::move(privateKey)),
      _registry(registry),
      _orderTracker(orderTracker),
      _logger(std::move(logger)),
      _transport(std::make_unique<CurlTransport>()),
      _accountAddress(std::move(accountAddress)),
      _vaultAddress(std::move(vaultAddress)),
      _mainnet(mainnet)
{
  loadAssetIds();
}

template <typename Policies>
HyperliquidOrderExecutorT<Policies>::HyperliquidOrderExecutorT(
    std::unique_ptr<ITransport> transport, std::string url, std::string privateKey,
    SymbolRegistry* registry, OrderTracker* orderTracker, std::shared_ptr<ILogger> logger,
    std::string accountAddress, std::optional<std::string> vaultAddress, bool mainnet)
    : _url(std::move(url)),
      _privateKey(std::move(privateKey)),
      _registry(registry),
      _orderTracker(orderTracker),
      _logger(std::move(logger)),
      _transport(std::move(transport)),
      _accountAddress(std::move(accountAddress)),
      _vaultAddress(std::move(vaultAddress)),
      _mainnet(mainnet)
{
  loadAssetIds();
}

template <typename Policies>
template <typename P, typename>
HyperliquidOrderExecutorT<Policies>::HyperliquidOrderExecutorT(
    std::string url, std::string privateKey, SymbolRegistry* registry, OrderTracker* orderTracker,
    std::shared_ptr<ILogger> logger, std::string accountAddress,
    std::optional<std::string> vaultAddress, bool mainnet, RateLimitConfig rateLimitConfig)
    : _url(std::move(url)),
      _privateKey(std::move(privateKey)),
      _registry(registry),
      _orderTracker(orderTracker),
      _logger(std::move(logger)),
      _transport(std::make_unique<CurlTransport>()),
      _accountAddress(std::move(accountAddress)),
      _vaultAddress(std::move(vaultAddress)),
      _mainnet(mainnet)
{
  _policies.rateLimit.init(std::move(rateLimitConfig));
  loadAssetIds();
}

template <typename Policies>
template <typename P, typename>
HyperliquidOrderExecutorT<Policies>::HyperliquidOrderExecutorT(
    std::string url, std::string privateKey, SymbolRegistry* registry, OrderTracker* orderTracker,
    std::shared_ptr<ILogger> logger, std::string accountAddress,
    std::optional<std::string> vaultAddress, bool mainnet, OrderTimeoutConfig timeoutConfig)
    : _url(std::move(url)),
      _privateKey(std::move(privateKey)),
      _registry(registry),
      _orderTracker(orderTracker),
      _logger(std::move(logger)),
      _transport(std::make_unique<CurlTransport>()),
      _accountAddress(std::move(accountAddress)),
      _vaultAddress(std::move(vaultAddress)),
      _mainnet(mainnet)
{
  _policies.timeout.init(std::move(timeoutConfig));
  _policies.timeout.start();
  loadAssetIds();
}

template <typename Policies>
template <typename P, typename>
HyperliquidOrderExecutorT<Policies>::HyperliquidOrderExecutorT(
    std::string url, std::string privateKey, SymbolRegistry* registry, OrderTracker* orderTracker,
    std::shared_ptr<ILogger> logger, std::string accountAddress,
    std::optional<std::string> vaultAddress, bool mainnet, RateLimitConfig rateLimitConfig,
    OrderTimeoutConfig timeoutConfig)
    : _url(std::move(url)),
      _privateKey(std::move(privateKey)),
      _registry(registry),
      _orderTracker(orderTracker),
      _logger(std::move(logger)),
      _transport(std::make_unique<CurlTransport>()),
      _accountAddress(std::move(accountAddress)),
      _vaultAddress(std::move(vaultAddress)),
      _mainnet(mainnet)
{
  _policies.rateLimit.init(std::move(rateLimitConfig));
  _policies.timeout.init(std::move(timeoutConfig));
  _policies.timeout.start();
  loadAssetIds();
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::loadAssetIds()
{
  {
    std::lock_guard lock(_assetMutex);
    if (_assetsLoaded)
    {
      return;
    }
    _assetsLoaded = true;
  }

  static constexpr char BODY[] = R"({"type":"meta"})";
  std::vector<std::pair<std::string_view, std::string_view>> hdr = {
      {"Content-Type", "application/json"}};

  _transport->post(
      "https://api.hyperliquid.xyz/info", BODY, hdr,
      [this](std::string_view resp)
      {
        simdjson::ondemand::parser p;
        simdjson::padded_string ps(resp);
        auto doc = p.iterate(ps);
        auto univ = doc["universe"].get_array();
        if (univ.error())
        {
          _logger->warn("[HL] meta parse error");
          return;
        }

        std::lock_guard lock(_assetMutex);
        size_t idx = 0;
        for (auto c : univ.value())
        {
          auto name = c["name"].get_string();
          if (!name.error())
          {
            _assetIds[std::string(name.value_unsafe())] = static_cast<int>(idx);
          }
          ++idx;
        }
        _logger->info("[HL] asset map " + std::to_string(_assetIds.size()));
      },
      [this](std::string_view e)
      {
        _logger->warn(std::string("[HL] meta fetch err ") + std::string(e));
      });
}

template <typename Policies>
int HyperliquidOrderExecutorT<Policies>::assetIdFor(std::string_view coin)
{
  std::lock_guard lock(_assetMutex);
  auto it = _assetIds.find(std::string(coin));
  if (it == _assetIds.end())
  {
    return -1;
  }
  return it->second;
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::publishRejection(const Order& order,
                                                           const std::string& reason)
{
  FLOX_LOG_ERROR("[HL] " << reason << " (orderId=" << order.id << ")");
  if (!_orderBus)
  {
    return;
  }
  OrderEvent ev;
  ev.status = OrderEventStatus::REJECTED;
  ev.order = order;
  ev.rejectReason = reason;
  ev.publishNs = nowMonoNanos();
  _orderBus->publish(std::move(ev));
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::publishSubmitted(const Order& order)
{
  if (!_orderBus)
  {
    return;
  }
  OrderEvent ev;
  ev.status = OrderEventStatus::SUBMITTED;
  ev.order = order;
  ev.publishNs = nowMonoNanos();
  _orderBus->publish(std::move(ev));
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::publishFill(const Order& order, Quantity fillQty,
                                                      Price fillPrice)
{
  if (!_orderBus)
  {
    return;
  }
  OrderEvent ev;
  // Hyperliquid has no private order stream in this connector, so the submit
  // response is the only place a fill is ever reported; the order is complete
  // when the venue says the whole size traded.
  ev.status = (fillQty.raw() >= order.quantity.raw()) ? OrderEventStatus::FILLED
                                                      : OrderEventStatus::PARTIALLY_FILLED;
  ev.order = order;
  ev.order.filledQuantity = fillQty;
  ev.fillQty = fillQty;
  ev.fillPrice = fillPrice;
  ev.publishNs = nowMonoNanos();
  _orderBus->publish(std::move(ev));
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::submitOrder(const Order& order)
{
  if (!_policies.rateLimit.tryAcquire(order.id))
  {
    return;
  }

  auto info = _registry->getSymbolInfo(order.symbol);
  if (!info)
  {
    FLOX_LOG_ERROR("[HL] unknown symbol id");
    return;
  }
  int asset = assetIdFor(info->symbol);
  if (asset < 0)
  {
    FLOX_LOG_ERROR("[HL] assetId not cached for " << info->symbol);
    return;
  }

  auto tifToken = hlOrderTifOrReject(order);
  if (!tifToken)
  {
    FLOX_LOG_ERROR("[HL] order type " << static_cast<int>(order.type)
                                      << " is not supported by this connector (id=" << order.id
                                      << ")");
    return;
  }

  const std::string px = tidy(order.price.toDouble(), 8);
  const std::string qty = tidy(order.quantity.toDouble(), 8);

  auto cloid = genCloid128();

  std::string orderObj;
  orderObj.reserve(160);
  orderObj += "{\"a\":" + std::to_string(asset);
  orderObj += ",\"b\":" + std::string(order.side == Side::BUY ? "true" : "false");
  orderObj += ",\"p\":\"" + px + "\"";
  orderObj += ",\"s\":\"" + qty + "\"";
  orderObj += ",\"r\":" + std::string(order.flags.reduceOnly ? "true" : "false");
  orderObj += ",\"t\":{\"limit\":{\"tif\":\"" + std::string(*tifToken) + "\"}}";
  orderObj += ",\"c\":\"" + cloid + "\"}";

  std::string actionJson =
      std::string("{\"type\":\"order\",\"orders\":[") + orderObj + "],\"grouping\":\"na\"}";

  uint64_t nonceMs = static_cast<uint64_t>(nowMs());

  hl::HlSignParams sp;
  sp.actionJson = actionJson;
  sp.nonceMs = nonceMs;
  sp.isMainnet = _mainnet;
  sp.privateKeyHex = _privateKey;
  if (_vaultAddress && !_vaultAddress->empty())
  {
    sp.activePoolJson = *_vaultAddress;
  }
  else
  {
    sp.activePoolJson = std::nullopt;
  }
  sp.expiresAfterMs = std::nullopt;

  auto sigOpt = hl::hl_sign_with_sdk(sp);
  if (!sigOpt)
  {
    FLOX_LOG_ERROR("[HL] sign via SDK helper failed");
    return;
  }

  const auto& sig = *sigOpt;

  std::string body;
  body.reserve(640);
  body += "{\"action\":" + actionJson;
  body += ",\"nonce\":" + std::to_string(nonceMs);
  if (_vaultAddress)
  {
    body += ",\"vaultAddress\":\"" + *_vaultAddress + "\"";
  }
  body += ",\"signature\":{";
  body += "\"r\":\"" + sig.r + "\",";
  body += "\"s\":\"" + sig.s + "\",";
  body += "\"v\":" + std::to_string(sig.v);
  body += "}}";

  _logger->info(std::string("[HL] body: ") + body);

  _policies.timeout.trackSubmit(order.id);

  _transport->post(
      _url, body, {{"Content-Type", "application/json"}},
      [this, order, cloid](std::string_view resp)
      {
        _policies.timeout.clearPending(order.id);

        // Hyperliquid answers HTTP 200 whatever happens and reports the
        // outcome in the body: a top-level "err", or one status object per
        // submitted order that is exactly one of "error", "filled" or
        // "resting". Only the two oid lookups used to be read, so a rejection
        // was recorded as a submitted order and an inline fill was never seen
        // by anything. dom rather than ondemand because these branches read
        // the same status object several times over.
        simdjson::dom::parser parser;
        simdjson::padded_string padded(resp);
        auto parsed = parser.parse(padded);
        if (parsed.error())
        {
          publishRejection(order, "Unparseable order response from venue");
          return;
        }
        auto doc = parsed.value();

        auto asString = [](auto element) -> std::string_view
        {
          if (element.error())
          {
            return {};
          }
          auto sv = element.get_string();
          return sv.error() ? std::string_view{} : sv.value_unsafe();
        };

        if (asString(doc["status"]) == "err")
        {
          std::string_view reason = asString(doc["response"]);
          publishRejection(order, reason.empty() ? std::string("Venue rejected the order")
                                                 : std::string(reason));
          return;
        }

        auto s0 = doc["response"]["data"]["statuses"].at(0);
        if (s0.error())
        {
          publishRejection(order, "Order response carried no status");
          return;
        }

        if (std::string_view reason = asString(s0["error"]); !reason.empty())
        {
          // The order never came into existence at the venue, so it is not
          // handed to the tracker at all -- same convention as the Bybit
          // executor, whose rejected submits leave no tracker record either.
          publishRejection(order, std::string(reason));
          return;
        }

        auto oidOf = [](auto element) -> std::string
        {
          auto oid = element["oid"].get_uint64();
          return oid.error() ? std::string{} : std::to_string(oid.value_unsafe());
        };

        if (auto filled = doc["response"]["data"]["statuses"].at(0)["filled"]; !filled.error())
        {
          Quantity fillQty{};
          Price fillPrice{};
          if (auto totalSz = asString(filled["totalSz"]); !totalSz.empty())
          {
            if (auto q = util::parseQty(totalSz))
            {
              fillQty = *q;
            }
          }
          if (auto avgPx = asString(filled["avgPx"]); !avgPx.empty())
          {
            if (auto px = util::parsePrice(avgPx))
            {
              fillPrice = *px;
            }
          }

          _orderTracker->onSubmitted(order, oidOf(filled.value()), cloid);
          _orderTracker->onFilled(order.id, fillQty);
          publishFill(order, fillQty, fillPrice);
          return;
        }

        if (auto resting = doc["response"]["data"]["statuses"].at(0)["resting"]; !resting.error())
        {
          _orderTracker->onSubmitted(order, oidOf(resting.value()), cloid);
          publishSubmitted(order);
          return;
        }

        // A status shape this connector does not know. The order may well be
        // live at the venue, so it is recorded, but with no exchange id there
        // is nothing to cancel it by later.
        _logger->warn("[HL] unrecognised order status in submit response");
        _orderTracker->onSubmitted(order, "", cloid);
        publishSubmitted(order);
      },
      [this, order](std::string_view err)
      {
        _policies.timeout.clearPending(order.id);
        publishRejection(order, std::string("Transport error: ") + std::string(err));
      });
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::cancelOrder(OrderId localId)
{
  if (!_policies.rateLimit.tryAcquire(localId))
  {
    return;
  }

  auto orderState = _orderTracker->get(localId);
  if (!orderState)
  {
    FLOX_LOG_ERROR("[HL] cancelOrder: no orderState for localId " << localId);
    return;
  }
  if (orderState->clientOrderId.empty())
  {
    FLOX_LOG_ERROR("[HL] cancelOrder: no clientOrderId for localId " << localId);
    return;
  }

  auto symbol = orderState->localOrder.symbol;
  auto info = _registry->getSymbolInfo(symbol);
  if (!info)
  {
    FLOX_LOG_ERROR("[HL] cancelOrder: no symbolInfo for " << symbol);
    return;
  }

  int asset = assetIdFor(info->symbol);
  if (asset < 0)
  {
    FLOX_LOG_ERROR("[HL] cancelOrder: no assetId for " << info->symbol);
    return;
  }

  std::string action;
  action.reserve(128);
  action += "{\"type\":\"cancelByCloid\",\"cancels\":[{\"asset\":" + std::to_string(asset) +
            ",\"cloid\":\"" + orderState->clientOrderId + "\"}]}";

  uint64_t nonceMs = static_cast<uint64_t>(nowMs());

  hl::HlSignParams sp{
      .actionJson = action,
      .nonceMs = static_cast<long long>(nonceMs),
      .privateKeyHex = _privateKey,
      .isMainnet = _mainnet,
  };
  auto sigOpt = hl_sign_with_sdk(sp);
  if (!sigOpt)
  {
    FLOX_LOG_ERROR("[HL] cancelOrder: signing failed");
    return;
  }

  std::string body;
  body.reserve(256);
  body += "{\"action\":" + action;
  body += ",\"nonce\":" + std::to_string(nonceMs);
  body += ",\"signature\":{";
  body += "\"r\":\"" + sigOpt->r + "\",";
  body += "\"s\":\"" + sigOpt->s + "\",";
  body += "\"v\":" + std::to_string(sigOpt->v);
  body += "}}";

  _logger->info(std::string("[HL] cancel body: ") + body);

  _policies.timeout.trackCancel(localId);

  _transport->post(
      _url, body, {{"Content-Type", "application/json"}},
      [this, localId](std::string_view resp)
      {
        _policies.timeout.clearPending(localId);

        simdjson::ondemand::parser p;
        simdjson::padded_string ps(resp);
        auto doc = p.iterate(ps);
        auto st = doc["status"].get_string();
        if (!st.error() && std::string_view(st.value_unsafe()) == "ok")
        {
          _orderTracker->onCanceled(localId);
        }
        else
        {
          FLOX_LOG_ERROR("[HL] cancel failed: " << resp);
        }
      },
      [this, localId](std::string_view err)
      {
        _policies.timeout.clearPending(localId);
        FLOX_LOG_ERROR("[HL] cancel error: " << err);
      });
}

template <typename Policies>
void HyperliquidOrderExecutorT<Policies>::replaceOrder(OrderId oldLocalId, const Order& n)
{
  if (!_policies.rateLimit.tryAcquire(oldLocalId))
  {
    return;
  }

  auto orderState = _orderTracker->get(oldLocalId);
  if (!orderState)
  {
    FLOX_LOG_ERROR("[HL] cancelOrder: no replaceOrder for oldLocalId " << oldLocalId);
    return;
  }

  auto exId = orderState->exchangeOrderId;
  auto cloid = orderState->clientOrderId;

  auto info = _registry->getSymbolInfo(n.symbol);
  if (!info)
  {
    FLOX_LOG_ERROR("[HL] unknown symbol id in replaceOrder");
    return;
  }
  int asset = assetIdFor(info->symbol);
  if (asset < 0)
  {
    FLOX_LOG_ERROR("[HL] assetId not cached for " << info->symbol);
    return;
  }

  auto tifToken = hlOrderTifOrReject(n);
  if (!tifToken)
  {
    FLOX_LOG_ERROR("[HL] replaceOrder: order type "
                   << static_cast<int>(n.type)
                   << " is not supported by this connector (oldLocalId=" << oldLocalId << ")");
    return;
  }

  const std::string px = tidy(n.price.toDouble(), 8);
  const std::string qty = tidy(n.quantity.toDouble(), 8);

  std::string orderObj;
  orderObj.reserve(160);
  orderObj += "{\"a\":" + std::to_string(asset);
  orderObj += ",\"b\":" + std::string(n.side == Side::BUY ? "true" : "false");
  orderObj += ",\"p\":\"" + px + "\"";
  orderObj += ",\"s\":\"" + qty + "\"";
  orderObj += ",\"r\":" + std::string(n.flags.reduceOnly ? "true" : "false");
  orderObj += ",\"t\":{\"limit\":{\"tif\":\"" + std::string(*tifToken) + "\"}}";
  orderObj += ",\"c\":\"" + cloid + "\"}";

  std::string action = "{\"type\":\"modify\",\"oid\":" + exId + ",\"order\":" + orderObj + "}";

  uint64_t nonceMs = static_cast<uint64_t>(nowMs());

  hl::HlSignParams sp{
      .actionJson = action,
      .nonceMs = static_cast<long long>(nonceMs),
      .privateKeyHex = _privateKey,
      .isMainnet = _mainnet,
  };
  auto sigOpt = hl_sign_with_sdk(sp);
  if (!sigOpt)
  {
    FLOX_LOG_ERROR("[HL] replaceOrder: signing failed");
    return;
  }

  std::string body;
  body.reserve(640);
  body += "{\"action\":" + action;
  body += ",\"nonce\":" + std::to_string(nonceMs);
  if (_vaultAddress)
  {
    body += ",\"vaultAddress\":\"";
    body += *_vaultAddress;
    body += "\"";
  }
  body += ",\"signature\":{";
  body += "\"r\":\"" + sigOpt->r + "\",";
  body += "\"s\":\"" + sigOpt->s + "\",";
  body += "\"v\":" + std::to_string(sigOpt->v);
  body += "}}";

  _logger->info(std::string("[HL] modify body: ") + body);

  _policies.timeout.trackReplace(oldLocalId);

  _transport->post(
      _url, body, {{"Content-Type", "application/json"}},
      [this, oldLocalId, exId, n, cloid](std::string_view resp)
      {
        _policies.timeout.clearPending(oldLocalId);

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(resp);
        auto doc = parser.iterate(padded);

        auto status = doc["status"].get_string();
        if (!status.error() && status.value() == "ok")
        {
          _orderTracker->onReplaced(oldLocalId, n, exId, cloid);
        }
        else
        {
          FLOX_LOG_ERROR("[HL] modify error: " << status);
        }
      },
      [this, oldLocalId](std::string_view err)
      {
        _policies.timeout.clearPending(oldLocalId);
        FLOX_LOG_ERROR("[HL] modify error: " << err);
      });
}

template class HyperliquidOrderExecutorT<NoPolicies>;
template class HyperliquidOrderExecutorT<WithRateLimit>;
template class HyperliquidOrderExecutorT<WithTimeout>;
template class HyperliquidOrderExecutorT<FullPolicies>;

}  // namespace flox
