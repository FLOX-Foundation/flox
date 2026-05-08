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
#include <optional>
#include <string>
#include <vector>

namespace flox::replay
{

// One trade-record snapshot used for both sides of a diff. Holds the
// fields the diff actually compares; the rest of TradeRecord (recv_ts,
// trade_id) is excluded because it differs across recorders even for
// byte-equal input streams.
struct TapeDiffTrade
{
  int64_t exchange_ts_ns{0};
  int64_t price_raw{0};
  int64_t qty_raw{0};
  uint32_t symbol_id{0};
  uint8_t side{0};
};

// One pair of records that did not match.
struct TapeDiffMismatch
{
  uint64_t index{0};
  TapeDiffTrade left{};
  TapeDiffTrade right{};
};

struct TapeDiffOptions
{
  // Maximum mismatches to record. Beyond this, the diff still walks
  // both tapes but stops appending. 0 means "no cap".
  uint32_t max_mismatches{16};

  // Allowed symmetric tolerance on exchange_ts_ns. Useful when
  // comparing live captures whose record-side wallclock drifts.
  int64_t field_tolerance_ns{0};
};

struct TapeDiffResult
{
  std::string left_path;
  std::string right_path;
  uint64_t left_count{0};
  uint64_t right_count{0};
  std::optional<uint64_t> first_divergence_index{};
  std::vector<TapeDiffMismatch> mismatches{};
  bool equal{false};
};

// Compare two .floxlog directories trade by trade. equal=true iff
// counts match and every paired record matches on
// (exchange_ts_ns, symbol_id, price_raw, qty_raw, side). When the
// shorter tape is a prefix of the longer one, first_divergence_index
// is set to the shorter length.
TapeDiffResult diffTapes(const std::filesystem::path& left,
                         const std::filesystem::path& right,
                         const TapeDiffOptions& opts = {});

}  // namespace flox::replay
