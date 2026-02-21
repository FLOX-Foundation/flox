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
#include <string>
#include <string_view>
#include <vector>

namespace flox
{

struct AuditEntry
{
  int64_t seq = 0;
  std::string ts;
  std::string actor;
  std::string action;
  int64_t trade_id = 0;
  std::string details;
  std::string source;
  std::string prev_hash;
  std::string hash;
};

struct AuditVerifyResult
{
  int64_t total = 0;
  int64_t verified = 0;
  bool intact = false;
  int64_t broken_at_seq = 0;
  std::string error;
};

struct IAuditLog
{
  virtual ~IAuditLog() = default;

  virtual void audit(std::string_view actor, std::string_view action,
                     int64_t tradeId, std::string_view details,
                     std::string_view source = "") = 0;

  virtual std::vector<AuditEntry> getLog(int limit = 100, int64_t beforeSeq = 0,
                                         std::string_view actor = "",
                                         int64_t tradeId = 0) = 0;

  virtual AuditVerifyResult verify() = 0;
};

}  // namespace flox
