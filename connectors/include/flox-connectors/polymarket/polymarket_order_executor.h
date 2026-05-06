/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <flox/common.h>
#include <flox/log/abstract_logger.h>

#include <memory>
#include <string>

namespace flox
{

/// Result of order execution
struct PolymarketOrderResult
{
  bool success{false};
  Quantity filledQty;
  Price avgPrice;
  uint64_t latencyMs{0};
  int32_t errorCode{0};
  std::string orderId;

  /// Get human-readable error message
  const char* errorMessage() const;
};

/// C++ wrapper for Rust FFI order executor
/// Provides fixed-point interface (Price/Quantity) over raw i64 FFI
class PolymarketOrderExecutor
{
 public:
  /// @param privateKey  Hex-encoded private key (with or without 0x prefix)
  /// @param funderWallet Hex-encoded funder/proxy wallet address (0x...)
  /// @param logger Optional logger for diagnostics
  explicit PolymarketOrderExecutor(std::string privateKey, std::string funderWallet,
                                   std::shared_ptr<ILogger> logger = nullptr);
  ~PolymarketOrderExecutor();

  PolymarketOrderExecutor(const PolymarketOrderExecutor&) = delete;
  PolymarketOrderExecutor& operator=(const PolymarketOrderExecutor&) = delete;

  /// Initialize Rust runtime and authenticate
  /// @return true on success
  bool init();

  /// Pre-establish TLS connections for lower latency
  void warmup();

  /// Prefetch token metadata (tick_size, fee_rate)
  /// Call before trading to avoid latency during orders
  void prefetch(const std::string& tokenId);

  /// Execute market buy (FAK - fills immediately at best price)
  /// @param tokenId Polymarket token ID
  /// @param usdcAmount Amount in USDC to spend
  PolymarketOrderResult buy(const std::string& tokenId, Volume usdcAmount);

  /// Execute market sell (FAK - fills immediately at best price)
  /// @param tokenId Polymarket token ID
  /// @param size Number of shares to sell
  PolymarketOrderResult sell(const std::string& tokenId, Quantity size);

  /// Place GTC limit buy order
  /// @param tokenId Polymarket token ID
  /// @param price Limit price (0.01-0.99)
  /// @param usdcAmount Amount in USDC to spend
  PolymarketOrderResult limitBuy(const std::string& tokenId, Price price, Volume usdcAmount);

  /// Place GTC limit sell order
  /// @param tokenId Polymarket token ID
  /// @param price Limit price (0.01-0.99)
  /// @param size Number of shares to sell
  PolymarketOrderResult limitSell(const std::string& tokenId, Price price, Quantity size);

  /// Cancel specific order
  bool cancel(const std::string& orderId);

  /// Cancel all open orders
  bool cancelAll();

  /// Get USDC balance
  Volume getBalance();

  /// Get token balance (shares held)
  Quantity getTokenBalance(const std::string& tokenId);

  /// Check if initialized
  bool isInitialized() const { return _initialized; }

 private:
  std::string _privateKey;
  std::string _funderWallet;
  std::shared_ptr<ILogger> _logger;
  bool _initialized{false};

  /// FFI decimal scale (6 decimals)
  static constexpr int64_t FFI_SCALE = 1'000'000;

  /// Flox decimal scale (8 decimals)
  static constexpr int64_t FLOX_SCALE = 100'000'000;

  /// Scale factor: FLOX / FFI = 100
  static constexpr int64_t SCALE_FACTOR = FLOX_SCALE / FFI_SCALE;
};

}  // namespace flox
