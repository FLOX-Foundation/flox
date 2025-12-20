/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/replay/abstract_market_data_recorder.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <atomic>
#include <memory>

namespace flox
{

struct MarketDataRecorderConfig
{
  std::filesystem::path output_dir;
  uint64_t max_segment_bytes{256ull << 20};  // 256 MB
  uint8_t exchange_id{0};
};

class MarketDataRecorder : public IMarketDataRecorder
{
 public:
  explicit MarketDataRecorder(MarketDataRecorderConfig config);
  ~MarketDataRecorder() override;

  // ISubsystem
  void start() override;
  void stop() override;

  // IMarketDataSubscriber
  SubscriberId id() const override { return _id; }
  void onBookUpdate(const BookUpdateEvent& event) override;
  void onTrade(const TradeEvent& event) override;
  void onBar(const BarEvent& event) override;

  // IMarketDataRecorder
  void setOutputDir(const std::filesystem::path& dir) override;
  void flush() override;
  RecorderStats stats() const override;
  bool isRecording() const override { return _recording.load(std::memory_order_relaxed); }

 private:
  MarketDataRecorderConfig _config;
  std::unique_ptr<replay::BinaryLogWriter> _writer;
  std::atomic<bool> _recording{false};
  SubscriberId _id{0};

  mutable RecorderStats _stats;
};

}  // namespace flox
