/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace flox::replay
{

/// Symbol mapping entry: symbol_id -> symbol name and properties
struct SymbolInfo
{
  uint32_t symbol_id{0};
  std::string name;           // e.g., "BTCUSDT"
  std::string base_asset;     // e.g., "BTC"
  std::string quote_asset;    // e.g., "USDT"
  int8_t price_precision{8};  // decimal places
  int8_t qty_precision{8};    // decimal places
};

/// Recording metadata stored as JSON alongside .floxlog files
struct RecordingMetadata
{
  // Recording identification
  std::string recording_id;  // UUID or unique identifier
  std::string description;   // Human-readable description

  // Source information
  std::string exchange;         // "binance", "bybit", "hyperliquid", etc.
  std::string exchange_type;    // "cex", "dex"
  std::string instrument_type;  // "spot", "perpetual", "futures", "option"
  std::string connector_version;

  // Symbol mappings (symbol_id -> info)
  std::vector<SymbolInfo> symbols;

  // Data content flags
  bool has_trades{false};
  bool has_book_snapshots{false};
  bool has_book_deltas{false};

  // Book depth info (if applicable)
  uint16_t book_depth{0};  // Max levels recorded

  // Time range (ISO 8601)
  std::string recording_start;  // "2025-01-15T10:30:00.000Z"
  std::string recording_end;    // Updated on close

  // Data scale (for fixed-point interpretation)
  int64_t price_scale{100000000};  // 1e8 default
  int64_t qty_scale{100000000};    // 1e8 default

  // Recording environment
  std::string hostname;
  std::string timezone;
  std::string flox_version;

  // Custom key-value pairs
  std::map<std::string, std::string> custom;

  /// Save metadata to JSON file
  bool save(const std::filesystem::path& path) const;

  /// Load metadata from JSON file
  static std::optional<RecordingMetadata> load(const std::filesystem::path& path);

  /// Get default metadata path for a data directory
  static std::filesystem::path metadataPath(const std::filesystem::path& data_dir)
  {
    return data_dir / "metadata.json";
  }
};

}  // namespace flox::replay
