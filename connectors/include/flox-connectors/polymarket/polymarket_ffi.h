/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#ifndef POLYMARKET_FFI_H
#define POLYMARKET_FFI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Error codes */
#define POLYMARKET_OK 0
#define POLYMARKET_ERR_NOT_INITIALIZED -1
#define POLYMARKET_ERR_INVALID_PK -2
#define POLYMARKET_ERR_AUTH_FAILED -3
#define POLYMARKET_ERR_INVALID_TOKEN -4
#define POLYMARKET_ERR_ORDER_FAILED -5
#define POLYMARKET_ERR_CANCEL_FAILED -6
#define POLYMARKET_ERR_MIN_ORDER_SIZE -7
#define POLYMARKET_ERR_MIN_SHARES -8

/**
 * Decimal scale: all raw values use 6 decimals
 * 1_000_000 raw = 1.0 actual
 */
#define POLYMARKET_DECIMAL_SCALE 1000000

  /**
 * Order execution result
 * All monetary values are in raw units (6 decimals)
 */
  typedef struct
  {
    bool success;           /* True if order was successful */
    int64_t filled_qty_raw; /* Shares filled in raw units (divide by 1M for actual) */
    int64_t avg_price_raw;  /* Average price in raw units (divide by 1M for actual) */
    uint64_t latency_ms;    /* Total execution latency */
    int32_t error_code;     /* Error code if failed */
    char order_id[128];     /* Order ID string */
  } PolymarketOrderResult;

  /**
 * Initialize the executor with a private key and funder wallet.
 *
 * @param private_key    Hex-encoded private key (with or without 0x prefix)
 * @param funder_wallet  Hex-encoded funder/proxy wallet address (0x...)
 * @return POLYMARKET_OK on success, negative error code on failure
 *
 * Thread safety: Call once at startup, before any other polymarket_* functions.
 */
  int32_t polymarket_init(const char* private_key, const char* funder_wallet);

  /**
 * Warm up connection pool by making a simple request.
 * Call this after polymarket_init() to pre-establish TLS connection.
 *
 * @return POLYMARKET_OK on success, negative error code on failure
 */
  int32_t polymarket_warmup(void);

  /**
 * Prefetch token metadata to warm up cache.
 *
 * @param token_id  Polymarket token ID (numeric string)
 * @return POLYMARKET_OK on success, negative error code on failure
 *
 * Call this for each token BEFORE trading to avoid HTTP latency
 * during order execution. Caches tick_size, fee_rate, and neg_risk.
 */
  int32_t polymarket_prefetch(const char* token_id);

  /**
 * Execute a market buy order (FAK - Fill and Kill).
 * Sweeps orderbook at price 0.99 to fill immediately.
 *
 * @param token_id    Polymarket token ID (numeric string)
 * @param usdc_amount Amount in USDC to spend
 * @return Result struct with fill info and latency
 *
 * Thread safety: Can be called from multiple threads after polymarket_init().
 */
  PolymarketOrderResult polymarket_market_buy(const char* token_id, double usdc_amount);

  /**
 * Execute a market sell order (FAK - Fill and Kill).
 * Sells at price 0.01 to fill immediately.
 *
 * @param token_id  Polymarket token ID (numeric string)
 * @param size      Number of shares to sell (fractional supported)
 * @return Result struct with fill info and latency
 *
 * Thread safety: Can be called from multiple threads after polymarket_init().
 */
  PolymarketOrderResult polymarket_market_sell(const char* token_id, double size);

  /**
 * Place a GTC limit buy order.
 *
 * @param token_id    Polymarket token ID (numeric string)
 * @param price       Limit price (0.01-0.99)
 * @param usdc_amount Amount in USDC to spend
 * @return Result struct with order info
 *
 * Thread safety: Can be called from multiple threads after polymarket_init().
 */
  PolymarketOrderResult polymarket_limit_buy(const char* token_id, double price,
                                             double usdc_amount);

  /**
 * Place a GTC limit sell order.
 *
 * @param token_id  Polymarket token ID (numeric string)
 * @param price     Limit price (0.01-0.99)
 * @param size      Number of shares to sell
 * @return Result struct with order info
 *
 * Thread safety: Can be called from multiple threads after polymarket_init().
 */
  PolymarketOrderResult polymarket_limit_sell(const char* token_id, double price, double size);

  /**
 * Cancel a specific order by ID.
 *
 * @param order_id  Order ID to cancel
 * @return POLYMARKET_OK on success, negative error code on failure
 */
  int32_t polymarket_cancel(const char* order_id);

  /**
 * Cancel all open orders.
 *
 * @return POLYMARKET_OK on success, negative error code on failure
 */
  int32_t polymarket_cancel_all(void);

  /**
 * Get current USDC balance.
 *
 * @return Raw balance (6 decimals), or negative value on error
 */
  int64_t polymarket_get_balance(void);

  /**
 * Get token balance (shares held).
 *
 * @param token_id  Polymarket token ID (numeric string)
 * @return Raw shares (6 decimals), or negative value on error
 *
 * Call this after BUY to get actual shares (net of fees).
 */
  int64_t polymarket_get_token_balance(const char* token_id);

  /**
 * Shutdown the executor.
 * Call before program exit for clean shutdown.
 */
  void polymarket_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* POLYMARKET_FFI_H */
