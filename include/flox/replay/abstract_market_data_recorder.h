/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/engine/abstract_subsystem.h"

#include <cstdint>
#include <filesystem>

namespace flox
{

struct RecorderStats
{
  uint64_t trades_written{0};
  uint64_t book_updates_written{0};
  uint64_t bytes_written{0};
  uint64_t files_created{0};
  uint64_t errors{0};
};

class IMarketDataRecorder : public ISubsystem, public IMarketDataSubscriber
{
 public:
  virtual ~IMarketDataRecorder() = default;

  virtual void setOutputDir(const std::filesystem::path& dir) = 0;
  virtual void flush() = 0;
  virtual RecorderStats stats() const = 0;
  virtual bool isRecording() const = 0;
};

}  // namespace flox
