/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/polymarket/polymarket_order_executor.h"
#include "flox-connectors/polymarket/polymarket_ffi.h"

namespace flox
{

const char* PolymarketOrderResult::errorMessage() const
{
  switch (errorCode)
  {
    case POLYMARKET_OK:
      return "OK";
    case POLYMARKET_ERR_NOT_INITIALIZED:
      return "Executor not initialized";
    case POLYMARKET_ERR_INVALID_PK:
      return "Invalid private key";
    case POLYMARKET_ERR_AUTH_FAILED:
      return "Authentication failed";
    case POLYMARKET_ERR_INVALID_TOKEN:
      return "Invalid token ID";
    case POLYMARKET_ERR_ORDER_FAILED:
      return "Order failed (check API response)";
    case POLYMARKET_ERR_CANCEL_FAILED:
      return "Cancel failed";
    case POLYMARKET_ERR_MIN_ORDER_SIZE:
      return "Order size below minimum ($1)";
    case POLYMARKET_ERR_MIN_SHARES:
      return "Shares below market minimum (call prefetch first)";
    default:
      return "Unknown error";
  }
}

PolymarketOrderExecutor::PolymarketOrderExecutor(std::string privateKey, std::string funderWallet,
                                                 std::shared_ptr<ILogger> logger)
    : _privateKey(std::move(privateKey)),
      _funderWallet(std::move(funderWallet)),
      _logger(std::move(logger))
{
}

PolymarketOrderExecutor::~PolymarketOrderExecutor()
{
  if (_initialized)
  {
    polymarket_shutdown();
  }
}

bool PolymarketOrderExecutor::init()
{
  if (_initialized)
  {
    return true;
  }

  int32_t result = polymarket_init(_privateKey.c_str(), _funderWallet.c_str());
  if (result != POLYMARKET_OK)
  {
    if (_logger)
    {
      _logger->error("[PolymarketOrderExecutor] Init failed: " + std::to_string(result));
    }
    return false;
  }

  _initialized = true;
  if (_logger)
  {
    _logger->info("[PolymarketOrderExecutor] Initialized");
  }
  return true;
}

void PolymarketOrderExecutor::warmup()
{
  if (!_initialized)
  {
    return;
  }

  int32_t result = polymarket_warmup();
  if (result != POLYMARKET_OK && _logger)
  {
    _logger->warn("[PolymarketOrderExecutor] Warmup failed: " + std::to_string(result));
  }
}

void PolymarketOrderExecutor::prefetch(const std::string& tokenId)
{
  if (!_initialized)
  {
    return;
  }

  int32_t result = polymarket_prefetch(tokenId.c_str());
  if (result != POLYMARKET_OK && _logger)
  {
    _logger->warn("[PolymarketOrderExecutor] Prefetch failed for " + tokenId);
  }
}

PolymarketOrderResult PolymarketOrderExecutor::buy(const std::string& tokenId, Volume usdcAmount)
{
  PolymarketOrderResult result;

  if (!_initialized)
  {
    result.errorCode = POLYMARKET_ERR_NOT_INITIALIZED;
    return result;
  }

  // Convert Volume (8 decimals) to double for FFI
  double usdc = static_cast<double>(usdcAmount.raw()) / static_cast<double>(FLOX_SCALE);

  ::PolymarketOrderResult ffiResult = polymarket_market_buy(tokenId.c_str(), usdc);

  result.success = ffiResult.success;
  result.latencyMs = ffiResult.latency_ms;
  result.errorCode = ffiResult.error_code;
  result.orderId = std::string(ffiResult.order_id);

  // Convert raw i64 (6 decimals) to flox types (8 decimals)
  result.filledQty = Quantity::fromRaw(ffiResult.filled_qty_raw * SCALE_FACTOR);
  result.avgPrice = Price::fromRaw(ffiResult.avg_price_raw * SCALE_FACTOR);

  return result;
}

PolymarketOrderResult PolymarketOrderExecutor::sell(const std::string& tokenId, Quantity size)
{
  PolymarketOrderResult result;

  if (!_initialized)
  {
    result.errorCode = POLYMARKET_ERR_NOT_INITIALIZED;
    return result;
  }

  // Convert Quantity (8 decimals) to double for FFI
  double shares = static_cast<double>(size.raw()) / static_cast<double>(FLOX_SCALE);

  ::PolymarketOrderResult ffiResult = polymarket_market_sell(tokenId.c_str(), shares);

  result.success = ffiResult.success;
  result.latencyMs = ffiResult.latency_ms;
  result.errorCode = ffiResult.error_code;
  result.orderId = std::string(ffiResult.order_id);

  // Convert raw i64 (6 decimals) to flox types (8 decimals)
  result.filledQty = Quantity::fromRaw(ffiResult.filled_qty_raw * SCALE_FACTOR);
  result.avgPrice = Price::fromRaw(ffiResult.avg_price_raw * SCALE_FACTOR);

  return result;
}

PolymarketOrderResult PolymarketOrderExecutor::limitBuy(const std::string& tokenId, Price price,
                                                        Volume usdcAmount)
{
  PolymarketOrderResult result;

  if (!_initialized)
  {
    result.errorCode = POLYMARKET_ERR_NOT_INITIALIZED;
    return result;
  }

  // Convert Volume and Price (8 decimals) to double for FFI
  double usdc = static_cast<double>(usdcAmount.raw()) / static_cast<double>(FLOX_SCALE);
  double priceDouble = static_cast<double>(price.raw()) / static_cast<double>(FLOX_SCALE);

  ::PolymarketOrderResult ffiResult = polymarket_limit_buy(tokenId.c_str(), priceDouble, usdc);

  result.success = ffiResult.success;
  result.latencyMs = ffiResult.latency_ms;
  result.errorCode = ffiResult.error_code;
  result.orderId = std::string(ffiResult.order_id);

  // Convert raw i64 (6 decimals) to flox types (8 decimals)
  result.filledQty = Quantity::fromRaw(ffiResult.filled_qty_raw * SCALE_FACTOR);
  result.avgPrice = Price::fromRaw(ffiResult.avg_price_raw * SCALE_FACTOR);

  return result;
}

PolymarketOrderResult PolymarketOrderExecutor::limitSell(const std::string& tokenId, Price price,
                                                         Quantity size)
{
  PolymarketOrderResult result;

  if (!_initialized)
  {
    result.errorCode = POLYMARKET_ERR_NOT_INITIALIZED;
    return result;
  }

  // Convert Quantity and Price (8 decimals) to double for FFI
  double shares = static_cast<double>(size.raw()) / static_cast<double>(FLOX_SCALE);
  double priceDouble = static_cast<double>(price.raw()) / static_cast<double>(FLOX_SCALE);

  ::PolymarketOrderResult ffiResult = polymarket_limit_sell(tokenId.c_str(), priceDouble, shares);

  result.success = ffiResult.success;
  result.latencyMs = ffiResult.latency_ms;
  result.errorCode = ffiResult.error_code;
  result.orderId = std::string(ffiResult.order_id);

  // Convert raw i64 (6 decimals) to flox types (8 decimals)
  result.filledQty = Quantity::fromRaw(ffiResult.filled_qty_raw * SCALE_FACTOR);
  result.avgPrice = Price::fromRaw(ffiResult.avg_price_raw * SCALE_FACTOR);

  return result;
}

bool PolymarketOrderExecutor::cancel(const std::string& orderId)
{
  if (!_initialized)
  {
    return false;
  }

  return polymarket_cancel(orderId.c_str()) == POLYMARKET_OK;
}

bool PolymarketOrderExecutor::cancelAll()
{
  if (!_initialized)
  {
    return false;
  }

  return polymarket_cancel_all() == POLYMARKET_OK;
}

Volume PolymarketOrderExecutor::getBalance()
{
  if (!_initialized)
  {
    return Volume::fromRaw(0);
  }

  int64_t raw = polymarket_get_balance();
  if (raw < 0)
  {
    return Volume::fromRaw(0);
  }

  // Convert 6 decimals to 8 decimals
  return Volume::fromRaw(raw * SCALE_FACTOR);
}

Quantity PolymarketOrderExecutor::getTokenBalance(const std::string& tokenId)
{
  if (!_initialized)
  {
    return Quantity::fromRaw(0);
  }

  int64_t raw = polymarket_get_token_balance(tokenId.c_str());
  if (raw < 0)
  {
    return Quantity::fromRaw(0);
  }

  // Convert 6 decimals to 8 decimals
  return Quantity::fromRaw(raw * SCALE_FACTOR);
}

}  // namespace flox
