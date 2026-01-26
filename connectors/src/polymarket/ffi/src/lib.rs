/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

//! Polymarket Order Executor - FFI Library
//!
//! C-compatible API for integration with C++ trader.
//! Provides direct function calls instead of socket IPC.

use std::collections::HashMap;
use std::ffi::CStr;
use std::os::raw::c_char;
use std::str::FromStr;
use std::sync::{OnceLock, RwLock};
use std::time::{Duration, Instant};

use alloy::primitives::Address;
use alloy::signers::local::LocalSigner;
use alloy::signers::Signer;
use polymarket_client_sdk::auth::{state::Authenticated, Normal};
use polymarket_client_sdk::clob::types::request::{BalanceAllowanceRequest, OrderBookSummaryRequest};
use polymarket_client_sdk::clob::types::{Amount, AssetType, OrderType, Side, SignatureType};
use polymarket_client_sdk::clob::{Client, Config};
use polymarket_client_sdk::types::{Decimal, U256};
use polymarket_client_sdk::POLYGON;
use tokio::runtime::Runtime;
use tracing::error;

const CLOB_HOST: &str = "https://clob.polymarket.com";

type AuthClient = Client<Authenticated<Normal>>;
type SignerType = LocalSigner<k256::ecdsa::SigningKey>;

/// Global executor state
struct Executor {
    client: AuthClient,
    signer: SignerType,
    runtime: Runtime,
    /// Cached min_order_size per token (in shares)
    min_order_sizes: RwLock<HashMap<String, Decimal>>,
}

static EXECUTOR: OnceLock<RwLock<Option<Executor>>> = OnceLock::new();

/// Default timeout for API operations (10 seconds)
const API_TIMEOUT: Duration = Duration::from_secs(10);

/// Get executor reference, returns None if not initialized
fn get_executor() -> Option<std::sync::RwLockReadGuard<'static, Option<Executor>>> {
    let lock = EXECUTOR.get_or_init(|| RwLock::new(None));
    let guard = lock.read().ok()?;
    if guard.is_some() {
        Some(guard)
    } else {
        None
    }
}

/// Decimal scale: 6 decimals (1_000_000 = 1.0)
/// USDC uses 6 decimals, Polymarket shares use 6 decimals
pub const DECIMAL_SCALE: i64 = 1_000_000;

/// Result structure returned to C++
/// All monetary values are in raw units (6 decimals)
#[repr(C)]
pub struct PolymarketOrderResult {
    pub success: bool,
    pub filled_qty_raw: i64,  // Raw value, divide by 1_000_000 for actual
    pub avg_price_raw: i64,   // Raw value, divide by 1_000_000 for actual
    pub latency_ms: u64,
    pub error_code: i32,
    pub order_id: [c_char; 128],
}

impl Default for PolymarketOrderResult {
    fn default() -> Self {
        Self {
            success: false,
            filled_qty_raw: 0,
            avg_price_raw: 0,
            latency_ms: 0,
            error_code: 0,
            order_id: [0; 128],
        }
    }
}

impl PolymarketOrderResult {
    fn with_error(code: i32) -> Self {
        Self {
            success: false,
            error_code: code,
            ..Default::default()
        }
    }

    fn set_order_id(&mut self, id: &str) {
        let bytes = id.as_bytes();
        let len = bytes.len().min(127);
        for (i, &b) in bytes[..len].iter().enumerate() {
            self.order_id[i] = b as c_char;
        }
        self.order_id[len] = 0;
    }
}

/// Convert Decimal to raw i64 (6 decimals)
/// Polymarket uses 6 decimal places for USDC and shares
fn decimal_to_raw(d: Decimal) -> i64 {
    // Decimal internally stores mantissa and scale
    // We need to normalize to 6 decimal places
    // Example: 1.5 (mantissa=15, scale=1) -> 1_500_000
    let mantissa = d.mantissa();
    let scale = d.scale();

    // Target scale is 6 decimals
    const TARGET_SCALE: u32 = 6;

    if scale == TARGET_SCALE {
        mantissa as i64
    } else if scale < TARGET_SCALE {
        // Need to multiply (e.g., scale=2 -> multiply by 10^4)
        let factor = 10i128.pow(TARGET_SCALE - scale);
        (mantissa * factor) as i64
    } else {
        // Need to divide (e.g., scale=8 -> divide by 10^2)
        let factor = 10i128.pow(scale - TARGET_SCALE);
        (mantissa / factor) as i64
    }
}

/// Error codes
pub const POLYMARKET_OK: i32 = 0;
pub const POLYMARKET_ERR_NOT_INITIALIZED: i32 = -1;
pub const POLYMARKET_ERR_INVALID_PK: i32 = -2;
pub const POLYMARKET_ERR_AUTH_FAILED: i32 = -3;
pub const POLYMARKET_ERR_INVALID_TOKEN: i32 = -4;
pub const POLYMARKET_ERR_ORDER_FAILED: i32 = -5;
pub const POLYMARKET_ERR_CANCEL_FAILED: i32 = -6;
pub const POLYMARKET_ERR_MIN_ORDER_SIZE: i32 = -7;  // Order below $1 minimum
pub const POLYMARKET_ERR_MIN_SHARES: i32 = -8;      // Shares below market minimum

/// Warm up connection pool by making simple requests
/// Call this after init to pre-establish TLS connection
#[unsafe(no_mangle)]
pub extern "C" fn polymarket_warmup() -> i32 {
    let guard = match get_executor() {
        Some(g) => g,
        None => return POLYMARKET_ERR_NOT_INITIALIZED,
    };
    let executor = guard.as_ref().unwrap();

    // Make 3 requests to warm up TLS connection pool
    let result = executor.runtime.block_on(async {
        tokio::time::timeout(API_TIMEOUT, async {
            executor.client.ok().await?;
            executor.client.ok().await?;
            executor.client.ok().await?;
            Ok::<_, anyhow::Error>(())
        }).await.map_err(|_| anyhow::anyhow!("timeout"))?
    });

    match result {
        Ok(_) => POLYMARKET_OK,
        Err(e) => {
            error!("[WARMUP ERROR] {}", e);
            POLYMARKET_ERR_AUTH_FAILED
        }
    }
}

/// Initialize the executor with a private key and funder wallet
/// Returns 0 on success, negative error code on failure
/// Can be called again after polymarket_shutdown() to re-initialize
#[unsafe(no_mangle)]
pub extern "C" fn polymarket_init(
    private_key: *const c_char,
    funder_wallet: *const c_char,
) -> i32 {
    // Check if already initialized
    let lock = EXECUTOR.get_or_init(|| RwLock::new(None));
    {
        let guard = match lock.read() {
            Ok(g) => g,
            Err(_) => return POLYMARKET_ERR_AUTH_FAILED,
        };
        if guard.is_some() {
            return POLYMARKET_OK; // Already initialized
        }
    }

    // Setup tracing
    let _ = tracing_subscriber::fmt()
        .with_env_filter("polymarket_executor=info")
        .try_init();

    let pk = unsafe {
        if private_key.is_null() {
            return POLYMARKET_ERR_INVALID_PK;
        }
        match CStr::from_ptr(private_key).to_str() {
            Ok(s) => s.to_string(),
            Err(_) => return POLYMARKET_ERR_INVALID_PK,
        }
    };

    let funder_str = unsafe {
        if funder_wallet.is_null() {
            return POLYMARKET_ERR_AUTH_FAILED;
        }
        match CStr::from_ptr(funder_wallet).to_str() {
            Ok(s) => s.to_string(),
            Err(_) => return POLYMARKET_ERR_AUTH_FAILED,
        }
    };

    // Create tokio runtime
    let runtime = match Runtime::new() {
        Ok(rt) => rt,
        Err(_) => return POLYMARKET_ERR_AUTH_FAILED,
    };

    // Initialize client in runtime
    let result = runtime.block_on(async {
        let signer = LocalSigner::from_str(&pk)
            .map_err(|_| POLYMARKET_ERR_INVALID_PK)?
            .with_chain_id(Some(POLYGON));

        // Parse funder address from parameter
        let funder: Address = funder_str.parse()
            .map_err(|_| POLYMARKET_ERR_AUTH_FAILED)?;

        // IMPORTANT: use_server_time(false) to avoid extra HTTP request per order
        // Server time sync adds ~80-100ms latency per request
        let config = Config::builder().use_server_time(false).build();
        let client = Client::new(CLOB_HOST, config)
            .map_err(|_| POLYMARKET_ERR_AUTH_FAILED)?
            .authentication_builder(&signer)
            .funder(funder)
            .signature_type(SignatureType::Proxy)
            .authenticate()
            .await
            .map_err(|_| POLYMARKET_ERR_AUTH_FAILED)?;

        Ok::<_, i32>((client, signer))
    });

    match result {
        Ok((client, signer)) => {
            let executor = Executor {
                client,
                signer,
                runtime,
                min_order_sizes: RwLock::new(HashMap::new()),
            };
            // Store in RwLock
            if let Ok(mut guard) = lock.write() {
                *guard = Some(executor);
                POLYMARKET_OK
            } else {
                POLYMARKET_ERR_AUTH_FAILED
            }
        }
        Err(code) => code,
    }
}

/// Prefetch token metadata to avoid HTTP calls during order execution
/// Call this for each token before trading to warm up the cache
/// Returns 0 on success, negative error code on failure
#[unsafe(no_mangle)]
pub extern "C" fn polymarket_prefetch(token_id: *const c_char) -> i32 {
    let guard = match get_executor() {
        Some(g) => g,
        None => return POLYMARKET_ERR_NOT_INITIALIZED,
    };
    let executor = guard.as_ref().unwrap();

    let token_str = unsafe {
        if token_id.is_null() {
            return POLYMARKET_ERR_INVALID_TOKEN;
        }
        match CStr::from_ptr(token_id).to_str() {
            Ok(s) => s,
            Err(_) => return POLYMARKET_ERR_INVALID_TOKEN,
        }
    };

    let token = match U256::from_str(token_str) {
        Ok(t) => t,
        Err(_) => return POLYMARKET_ERR_INVALID_TOKEN,
    };

    // Fetch and cache all metadata with timeout
    let result = executor.runtime.block_on(async {
        tokio::time::timeout(API_TIMEOUT, async {
            // These calls populate the internal cache
            let tick = executor.client.tick_size(token).await?;
            let fee = executor.client.fee_rate_bps(token).await?;
            let neg = executor.client.neg_risk(token).await?;

            // Get order book to fetch min_order_size
            let book_req = OrderBookSummaryRequest::builder()
                .token_id(token)
                .build();
            let book = executor.client.order_book(&book_req).await?;

            Ok::<_, anyhow::Error>((tick, fee, neg, book.min_order_size))
        }).await.map_err(|_| anyhow::anyhow!("timeout"))?
    });

    match result {
        Ok((_tick, _fee, _neg, min_size)) => {
            // Cache min_order_size
            if let Ok(mut cache) = executor.min_order_sizes.write() {
                cache.insert(token_str.to_string(), min_size);
            }
            POLYMARKET_OK
        }
        Err(e) => {
            error!("[PREFETCH ERROR] token={} | error={}", token_str, e);
            POLYMARKET_ERR_ORDER_FAILED
        }
    }
}

/// Execute a market buy order (FAK - Fill and Kill)
/// Sweeps orderbook at price 0.99 to fill immediately
/// Returns result with filled quantity, average price, and latency
#[unsafe(no_mangle)]
pub extern "C" fn polymarket_market_buy(
    token_id: *const c_char,
    usdc_amount: f64,  // amount in USDC to spend
) -> PolymarketOrderResult {
    let guard = match get_executor() {
        Some(g) => g,
        None => return PolymarketOrderResult::with_error(POLYMARKET_ERR_NOT_INITIALIZED),
    };
    let executor = guard.as_ref().unwrap();

    let token_str = unsafe {
        if token_id.is_null() {
            return PolymarketOrderResult::with_error(POLYMARKET_ERR_INVALID_TOKEN);
        }
        match CStr::from_ptr(token_id).to_str() {
            Ok(s) => s,
            Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_INVALID_TOKEN),
        }
    };

    let token = match U256::from_str(token_str) {
        Ok(t) => t,
        Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_INVALID_TOKEN),
    };

    let start = Instant::now();

    // Round USDC to 6 decimal places (USDC precision)
    let usdc_rounded = (usdc_amount * 1_000_000.0).floor() / 1_000_000.0;
    let usdc_decimal = match Decimal::try_from(usdc_rounded) {
        Ok(d) => d,
        Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_ORDER_FAILED),
    };

    let result = executor.runtime.block_on(async {
        tokio::time::timeout(API_TIMEOUT, async {
            // Amount::usdc means "spend this much USDC to buy shares"
            // Use price 0.99 to sweep entire orderbook (aggressive market buy)
            let order = executor
                .client
                .market_order()
                .token_id(token)
                .amount(Amount::usdc(usdc_decimal)?)
                .side(Side::Buy)
                .order_type(OrderType::FAK)
                .price(Decimal::try_from(0.99).unwrap())
                .build()
                .await?;

            let signed = executor.client.sign(&executor.signer, order).await?;
            let response = executor.client.post_order(signed).await?;

            Ok::<_, anyhow::Error>(response)
        }).await.map_err(|_| anyhow::anyhow!("timeout"))?
    });

    let latency_ms = start.elapsed().as_millis() as u64;

    match result {
        Ok(resp) => {
            // For BUY: taking_amount = shares received, making_amount = USDC paid
            let filled_shares: f64 = resp.taking_amount.try_into().unwrap_or(0.0);
            let usdc_paid: f64 = resp.making_amount.try_into().unwrap_or(0.0);
            let avg_price = if filled_shares > 0.0 {
                usdc_paid / filled_shares
            } else {
                0.0
            };

            // Calculate fee and net shares received (taker fee)
            // fee_shares = shares * 0.25 * (price * (1 - price))^2
            let fee_factor = 0.25 * (avg_price * (1.0 - avg_price)).powi(2);
            let fee_shares = filled_shares * fee_factor;
            let net_shares = filled_shares - fee_shares;

            // Return net shares (after fee deduction)
            let net_shares_raw = (net_shares * 1_000_000.0) as i64;
            let avg_price_raw = (avg_price * 1_000_000.0) as i64;

            let mut result = PolymarketOrderResult {
                success: resp.success,
                filled_qty_raw: net_shares_raw,
                avg_price_raw,
                latency_ms,
                error_code: POLYMARKET_OK,
                order_id: [0; 128],
            };
            result.set_order_id(&resp.order_id);
            result
        }
        Err(e) => {
            error!("[FFI ORDER ERROR] BUY | error={} | latency={}ms", e, latency_ms);
            let mut result = PolymarketOrderResult::with_error(POLYMARKET_ERR_ORDER_FAILED);
            result.latency_ms = latency_ms;
            result
        }
    }
}

/// Place a GTC limit buy order
/// Returns result with order info
#[unsafe(no_mangle)]
pub extern "C" fn polymarket_limit_buy(
    token_id: *const c_char,
    price: f64,       // limit price (0.01-0.99)
    usdc_amount: f64, // amount in USDC to spend
) -> PolymarketOrderResult {
    let guard = match get_executor() {
        Some(g) => g,
        None => return PolymarketOrderResult::with_error(POLYMARKET_ERR_NOT_INITIALIZED),
    };
    let executor = guard.as_ref().unwrap();

    let token_str = unsafe {
        if token_id.is_null() {
            return PolymarketOrderResult::with_error(POLYMARKET_ERR_INVALID_TOKEN);
        }
        match CStr::from_ptr(token_id).to_str() {
            Ok(s) => s,
            Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_INVALID_TOKEN),
        }
    };

    let token = match U256::from_str(token_str) {
        Ok(t) => t,
        Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_INVALID_TOKEN),
    };

    let start = Instant::now();

    // Check minimum USDC order size
    const MIN_ORDER_USDC: f64 = 1.0;
    if usdc_amount < MIN_ORDER_USDC {
        error!("[FFI LIMIT BUY] order size ${:.4} below minimum ${}", usdc_amount, MIN_ORDER_USDC);
        return PolymarketOrderResult::with_error(POLYMARKET_ERR_MIN_ORDER_SIZE);
    }

    // For limit orders, we need to compute shares with proper precision:
    // - Taker amount (shares): max 2 decimals
    // - Maker amount (USDC): max 4 decimals
    // Use ceil to ensure we don't go below min order size ($1)
    let shares_raw = (usdc_amount / price * 100.0).ceil() / 100.0;

    // Check minimum shares for this market (use try_read to avoid blocking)
    if let Some(cache) = executor.min_order_sizes.try_read().ok() {
        if let Some(&min_shares) = cache.get(token_str) {
            let min_shares_f64: f64 = min_shares.try_into().unwrap_or(0.0);
            if shares_raw < min_shares_f64 {
                error!("[FFI LIMIT BUY] shares {} below market minimum {}", shares_raw, min_shares);
                return PolymarketOrderResult::with_error(POLYMARKET_ERR_MIN_SHARES);
            }
        }
    }

    let shares_decimal = match Decimal::try_from(shares_raw) {
        Ok(d) => d,
        Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_ORDER_FAILED),
    };

    let price_decimal = match Decimal::try_from(price) {
        Ok(d) => d,
        Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_ORDER_FAILED),
    };

    let result = executor.runtime.block_on(async {
        tokio::time::timeout(API_TIMEOUT, async {
            let order = executor
                .client
                .market_order()
                .token_id(token)
                .amount(Amount::shares(shares_decimal)?)
                .side(Side::Buy)
                .order_type(OrderType::GTC)
                .price(price_decimal)
                .build()
                .await?;

            let signed = executor.client.sign(&executor.signer, order).await?;
            let response = executor.client.post_order(signed).await?;

            Ok::<_, anyhow::Error>(response)
        }).await.map_err(|_| anyhow::anyhow!("timeout"))?
    });

    let latency_ms = start.elapsed().as_millis() as u64;

    match result {
        Ok(resp) => {
            // GTC limit orders are maker orders - NO FEE when resting in book
            // Fee only applies if order filled immediately as taker
            // We return raw filled amount - fee calculation should be done
            // by caller based on whether order was maker or taker
            let filled_qty_raw = decimal_to_raw(resp.taking_amount);

            let mut result = PolymarketOrderResult {
                success: resp.success,
                filled_qty_raw,
                avg_price_raw: decimal_to_raw(price_decimal),
                latency_ms,
                error_code: POLYMARKET_OK,
                order_id: [0; 128],
            };
            result.set_order_id(&resp.order_id);
            result
        }
        Err(e) => {
            error!("[FFI ORDER ERROR] LIMIT BUY | error={} | latency={}ms", e, latency_ms);
            let mut result = PolymarketOrderResult::with_error(POLYMARKET_ERR_ORDER_FAILED);
            result.latency_ms = latency_ms;
            result
        }
    }
}

/// Place a GTC limit sell order
/// Returns result with order info
#[unsafe(no_mangle)]
pub extern "C" fn polymarket_limit_sell(
    token_id: *const c_char,
    price: f64,   // limit price (0.01-0.99)
    size: f64,    // number of shares to sell
) -> PolymarketOrderResult {
    let guard = match get_executor() {
        Some(g) => g,
        None => return PolymarketOrderResult::with_error(POLYMARKET_ERR_NOT_INITIALIZED),
    };
    let executor = guard.as_ref().unwrap();

    let token_str = unsafe {
        if token_id.is_null() {
            return PolymarketOrderResult::with_error(POLYMARKET_ERR_INVALID_TOKEN);
        }
        match CStr::from_ptr(token_id).to_str() {
            Ok(s) => s,
            Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_INVALID_TOKEN),
        }
    };

    let token = match U256::from_str(token_str) {
        Ok(t) => t,
        Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_INVALID_TOKEN),
    };

    let start = Instant::now();

    // Round size to 2 decimal places (Polymarket requirement)
    let size_rounded = (size * 100.0).floor() / 100.0;
    let size_decimal = match Decimal::try_from(size_rounded) {
        Ok(d) => d,
        Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_ORDER_FAILED),
    };

    let price_decimal = match Decimal::try_from(price) {
        Ok(d) => d,
        Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_ORDER_FAILED),
    };

    let result = executor.runtime.block_on(async {
        tokio::time::timeout(API_TIMEOUT, async {
            let order = executor
                .client
                .market_order()
                .token_id(token)
                .amount(Amount::shares(size_decimal)?)
                .side(Side::Sell)
                .order_type(OrderType::GTC)
                .price(price_decimal)
                .build()
                .await?;

            let signed = executor.client.sign(&executor.signer, order).await?;
            let response = executor.client.post_order(signed).await?;

            Ok::<_, anyhow::Error>(response)
        }).await.map_err(|_| anyhow::anyhow!("timeout"))?
    });

    let latency_ms = start.elapsed().as_millis() as u64;

    match result {
        Ok(resp) => {
            let mut result = PolymarketOrderResult {
                success: resp.success,
                filled_qty_raw: decimal_to_raw(resp.making_amount),
                avg_price_raw: decimal_to_raw(price_decimal),
                latency_ms,
                error_code: POLYMARKET_OK,
                order_id: [0; 128],
            };
            result.set_order_id(&resp.order_id);
            result
        }
        Err(e) => {
            error!("[FFI ORDER ERROR] LIMIT SELL | error={} | latency={}ms", e, latency_ms);
            let mut result = PolymarketOrderResult::with_error(POLYMARKET_ERR_ORDER_FAILED);
            result.latency_ms = latency_ms;
            result
        }
    }
}

/// Cancel an order by ID
/// Returns 0 on success, negative error code on failure
#[unsafe(no_mangle)]
pub extern "C" fn polymarket_cancel(order_id: *const c_char) -> i32 {
    let guard = match get_executor() {
        Some(g) => g,
        None => return POLYMARKET_ERR_NOT_INITIALIZED,
    };
    let executor = guard.as_ref().unwrap();

    let order_str = unsafe {
        if order_id.is_null() {
            return POLYMARKET_ERR_CANCEL_FAILED;
        }
        match CStr::from_ptr(order_id).to_str() {
            Ok(s) => s,
            Err(_) => return POLYMARKET_ERR_CANCEL_FAILED,
        }
    };

    let result = executor.runtime.block_on(async {
        tokio::time::timeout(API_TIMEOUT, executor.client.cancel_order(order_str))
            .await
            .map_err(|_| anyhow::anyhow!("timeout"))?
            .map_err(|e| anyhow::anyhow!("{}", e))
    });

    match result {
        Ok(_) => POLYMARKET_OK,
        Err(e) => {
            error!("[FFI CANCEL ERROR] order_id={} error={}", order_str, e);
            POLYMARKET_ERR_CANCEL_FAILED
        }
    }
}

/// Cancel all open orders
/// Returns 0 on success, negative error code on failure
#[unsafe(no_mangle)]
pub extern "C" fn polymarket_cancel_all() -> i32 {
    let guard = match get_executor() {
        Some(g) => g,
        None => return POLYMARKET_ERR_NOT_INITIALIZED,
    };
    let executor = guard.as_ref().unwrap();

    let result = executor.runtime.block_on(async {
        tokio::time::timeout(API_TIMEOUT, executor.client.cancel_all_orders())
            .await
            .map_err(|_| anyhow::anyhow!("timeout"))?
            .map_err(|e| anyhow::anyhow!("{}", e))
    });

    match result {
        Ok(_) => POLYMARKET_OK,
        Err(e) => {
            error!("[FFI CANCEL_ALL ERROR] error={}", e);
            POLYMARKET_ERR_CANCEL_FAILED
        }
    }
}

/// Get USDC balance
/// Returns raw balance (6 decimals), or negative on error
#[unsafe(no_mangle)]
pub extern "C" fn polymarket_get_balance() -> i64 {
    let guard = match get_executor() {
        Some(g) => g,
        None => return -1,
    };
    let executor = guard.as_ref().unwrap();

    let result = executor.runtime.block_on(async {
        tokio::time::timeout(
            API_TIMEOUT,
            executor
                .client
                .balance_allowance(BalanceAllowanceRequest::default()),
        )
        .await
        .map_err(|_| anyhow::anyhow!("timeout"))?
        .map_err(|e| anyhow::anyhow!("{}", e))
    });

    match result {
        Ok(balance) => {
            // Use decimal_to_raw for consistency
            decimal_to_raw(balance.balance)
        }
        Err(_) => -1,
    }
}

/// Get token balance (shares held)
/// Returns raw balance (6 decimals), or negative on error
#[unsafe(no_mangle)]
pub extern "C" fn polymarket_get_token_balance(token_id: *const c_char) -> i64 {
    let guard = match get_executor() {
        Some(g) => g,
        None => return -1,
    };
    let executor = guard.as_ref().unwrap();

    let token_str = unsafe {
        if token_id.is_null() {
            return -1;
        }
        match CStr::from_ptr(token_id).to_str() {
            Ok(s) => s,
            Err(_) => return -1,
        }
    };

    let token = match U256::from_str(token_str) {
        Ok(t) => t,
        Err(_) => return -1,
    };

    let result = executor.runtime.block_on(async {
        let req = BalanceAllowanceRequest::builder()
            .asset_type(AssetType::Conditional)
            .token_id(token)
            .signature_type(SignatureType::Proxy)
            .build();
        tokio::time::timeout(API_TIMEOUT, executor.client.balance_allowance(req))
            .await
            .map_err(|_| anyhow::anyhow!("timeout"))?
            .map_err(|e| anyhow::anyhow!("{}", e))
    });

    match result {
        Ok(balance) => {
            // Use decimal_to_raw for consistency
            decimal_to_raw(balance.balance)
        }
        Err(_) => -1,
    }
}

/// Execute a market sell order (FAK - Fill and Kill)
/// Sells at price 0.01 to fill immediately
/// Returns result with filled quantity, average price, and latency
#[unsafe(no_mangle)]
pub extern "C" fn polymarket_market_sell(
    token_id: *const c_char,
    size: f64,    // fractional shares supported
) -> PolymarketOrderResult {
    let guard = match get_executor() {
        Some(g) => g,
        None => return PolymarketOrderResult::with_error(POLYMARKET_ERR_NOT_INITIALIZED),
    };
    let executor = guard.as_ref().unwrap();

    let token_str = unsafe {
        if token_id.is_null() {
            return PolymarketOrderResult::with_error(POLYMARKET_ERR_INVALID_TOKEN);
        }
        match CStr::from_ptr(token_id).to_str() {
            Ok(s) => s,
            Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_INVALID_TOKEN),
        }
    };

    let token = match U256::from_str(token_str) {
        Ok(t) => t,
        Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_INVALID_TOKEN),
    };

    let start = Instant::now();
    // Use aggressive price for true market order - will fill at best available
    let market_price = 0.01;

    // Convert f64 size to Decimal, rounded to 2 decimal places (Polymarket requirement)
    let size_rounded = (size * 100.0).floor() / 100.0;  // Round DOWN to 2 decimals
    let size_decimal = match Decimal::try_from(size_rounded) {
        Ok(d) => d,
        Err(_) => return PolymarketOrderResult::with_error(POLYMARKET_ERR_ORDER_FAILED),
    };

    let result = executor.runtime.block_on(async {
        tokio::time::timeout(API_TIMEOUT, async {
            let order = executor
                .client
                .market_order()
                .token_id(token)
                .amount(Amount::shares(size_decimal)?)
                .side(Side::Sell)
                .order_type(OrderType::FAK)
                .price(Decimal::try_from(market_price).unwrap())
                .build()
                .await?;

            let signed = executor.client.sign(&executor.signer, order).await?;
            let response = executor.client.post_order(signed).await?;

            Ok::<_, anyhow::Error>(response)
        }).await.map_err(|_| anyhow::anyhow!("timeout"))?
    });

    let latency_ms = start.elapsed().as_millis() as u64;

    match result {
        Ok(resp) => {
            // For SELL: making_amount = shares sold, taking_amount = USDC received
            let filled_shares = resp.making_amount;
            let usdc_received = resp.taking_amount;
            let avg_price = if !filled_shares.is_zero() {
                usdc_received / filled_shares
            } else {
                Decimal::ZERO
            };

            let mut result = PolymarketOrderResult {
                success: resp.success,
                filled_qty_raw: decimal_to_raw(filled_shares),
                avg_price_raw: decimal_to_raw(avg_price),
                latency_ms,
                error_code: POLYMARKET_OK,
                order_id: [0; 128],
            };
            result.set_order_id(&resp.order_id);
            result
        }
        Err(e) => {
            error!("[FFI ORDER ERROR] SELL | error={} | latency={}ms", e, latency_ms);
            let mut result = PolymarketOrderResult::with_error(POLYMARKET_ERR_ORDER_FAILED);
            result.latency_ms = latency_ms;
            result
        }
    }
}

/// Shutdown and cleanup
/// After calling this, polymarket_init() can be called again to re-initialize
#[unsafe(no_mangle)]
pub extern "C" fn polymarket_shutdown() {
    let lock = match EXECUTOR.get() {
        Some(l) => l,
        None => return,
    };

    if let Ok(mut guard) = lock.write() {
        *guard = None;
    }
}
