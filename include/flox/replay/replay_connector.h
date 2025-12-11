/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/replay/abstract_replay_source.h"
#include "flox/replay/readers/binary_log_reader.h"

#include <atomic>
#include <memory>
#include <thread>

namespace flox
{

struct ReplayConnectorConfig
{
  std::filesystem::path data_dir;
  ReplaySpeed speed{ReplaySpeed::max()};
  std::optional<int64_t> from_ns;
  std::optional<int64_t> to_ns;
  std::set<uint32_t> symbols;
};

class ReplayConnector : public IReplaySource
{
 public:
  explicit ReplayConnector(ReplayConnectorConfig config);
  ~ReplayConnector() override;

  void start() override;
  void stop() override;

  std::string exchangeId() const override { return "replay"; }

  std::optional<TimeRange> dataRange() const override;
  void setSpeed(ReplaySpeed speed) override;
  bool seekTo(int64_t timestamp_ns) override;
  bool isFinished() const override;
  int64_t currentPosition() const override;

 private:
  void replayLoop();
  void emitTradeFromRecord(const replay::TradeRecord& record);
  void emitBookFromRecord(const replay::BookRecordHeader& header,
                          const std::vector<replay::BookLevel>& bids,
                          const std::vector<replay::BookLevel>& asks);

  ReplayConnectorConfig _config;
  std::unique_ptr<replay::BinaryLogReader> _reader;

  std::thread _replay_thread;
  std::atomic<bool> _running{false};
  std::atomic<bool> _finished{false};
  std::atomic<int64_t> _current_pos{0};
  std::atomic<double> _speed_multiplier{0.0};

  std::atomic<int64_t> _seek_target{-1};
  mutable std::mutex _seek_mutex;

  std::unique_ptr<std::pmr::monotonic_buffer_resource> _pmr_resource;
};

}  // namespace flox
