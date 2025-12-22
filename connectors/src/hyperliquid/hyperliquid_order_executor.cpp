#include "flox-connectors/hyperliquid/hyperliquid_order_executor.h"
#include "flox-connectors/hyperliquid/hl_signer.h"
#include "flox-connectors/net/curl_transport.h"

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

}  // namespace

HyperliquidOrderExecutor::HyperliquidOrderExecutor(
    std::string url, std::string privateKey, SymbolRegistry* registry, OrderTracker* orderTracker,
    std::shared_ptr<ILogger> logger, std::string accountAddress,
    std::optional<std::string> vaultAddress, bool mainnet)
    : _url(std::move(url)),
      _privateKey(std::move(privateKey)),
      _registry(registry),
      _orderTracker(orderTracker),
      _logger(std::move(logger)),
      _transport(std::make_unique<CurlTransport>()),
      _accountAddress(accountAddress),
      _vaultAddress(vaultAddress ? std::make_optional(*vaultAddress) : std::nullopt),
      _mainnet(mainnet)
{
  loadAssetIds();
}

void HyperliquidOrderExecutor::loadAssetIds()
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

int HyperliquidOrderExecutor::assetIdFor(std::string_view coin)
{
  std::lock_guard lock(_assetMutex);
  auto it = _assetIds.find(std::string(coin));
  if (it == _assetIds.end())
  {
    return -1;
  }
  return it->second;
}

void HyperliquidOrderExecutor::submitOrder(const Order& order)
{
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

  const std::string px = tidy(order.price.toDouble(), 8);
  const std::string qty = tidy(order.quantity.toDouble(), 8);

  auto cloid = genCloid128();

  std::string orderObj;
  orderObj.reserve(160);
  orderObj += "{\"a\":" + std::to_string(asset);
  orderObj += ",\"b\":" + std::string(order.side == Side::BUY ? "true" : "false");
  orderObj += ",\"p\":\"" + px + "\"";
  orderObj += ",\"s\":\"" + qty + "\"";
  orderObj += ",\"r\":false";
  orderObj += ",\"t\":{\"limit\":{\"tif\":\"Gtc\"}}";
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

  _transport->post(
      _url, body, {{"Content-Type", "application/json"}},
      [this, order, cloid](std::string_view resp)
      {
        simdjson::ondemand::parser p;
        simdjson::padded_string ps(resp);
        auto doc = p.iterate(ps);

        std::string exId;
        auto statuses = doc["response"]["data"]["statuses"];
        if (!statuses.error())
        {
          auto s0 = statuses.at(0);
          if (!s0.error())
          {
            auto roid = s0["resting"]["oid"].get_uint64();
            if (!roid.error())
            {
              exId = std::to_string(roid.value_unsafe());
            }
            auto foid = s0["filled"]["oid"].get_uint64();
            if (!foid.error())
            {
              exId = std::to_string(foid.value_unsafe());
            }
          }
        }

        auto endTime = now();
        auto durationMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(endTime - order.createdAt)
                .count();

        _orderTracker->onSubmitted(order, exId, cloid);
      },
      [this](std::string_view err)
      {
        FLOX_LOG_ERROR("[HL] submit error: " << err);
      });
}

void HyperliquidOrderExecutor::cancelOrder(OrderId localId)
{
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

  _transport->post(
      _url, body, {{"Content-Type", "application/json"}},
      [this, localId](std::string_view resp)
      {
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
      [this](std::string_view err)
      {
        FLOX_LOG_ERROR("[HL] cancel error: " << err);
      });
}

void HyperliquidOrderExecutor::replaceOrder(OrderId oldLocalId, const Order& n)
{
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

  const std::string px = tidy(n.price.toDouble(), 8);
  const std::string qty = tidy(n.quantity.toDouble(), 8);

  std::string orderObj;
  orderObj.reserve(160);
  orderObj += "{\"a\":" + std::to_string(asset);
  orderObj += ",\"b\":" + std::string(n.side == Side::BUY ? "true" : "false");
  orderObj += ",\"p\":\"" + px + "\"";
  orderObj += ",\"s\":\"" + qty + "\"";
  orderObj += ",\"r\":false";
  orderObj += ",\"t\":{\"limit\":{\"tif\":\"Gtc\"}}";
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

  _transport->post(
      _url, body, {{"Content-Type", "application/json"}},
      [this, oldLocalId, exId, n, cloid](std::string_view resp)
      {
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
      [this](std::string_view err)
      {
        FLOX_LOG_ERROR("[HL] modify error: " << err);
      });
}

}  // namespace flox
