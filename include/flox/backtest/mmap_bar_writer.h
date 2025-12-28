/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/bar.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/aggregator/timeframe.h"
#include "flox/common.h"
#include "flox/engine/abstract_market_data_subscriber.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace flox
{

class MmapBarWriter : public IMarketDataSubscriber
{
 public:
  explicit MmapBarWriter(const std::filesystem::path& symbolDir);
  ~MmapBarWriter();

  MmapBarWriter(const MmapBarWriter&) = delete;
  MmapBarWriter& operator=(const MmapBarWriter&) = delete;

  SubscriberId id() const override { return _subscriberId; }
  void onBar(const BarEvent& event) noexcept override;

  void flush();

  size_t bufferedBars(TimeframeId tf) const;
  size_t totalBufferedBars() const;

  void clear();

  void writeBars(TimeframeId tf, std::span<const Bar> bars);

  void setMetadata(const std::string& key, const std::string& value);
  void setMetadata(const std::map<std::string, std::string>& metadata);
  void writeMetadata();

 private:
  std::filesystem::path makeFilename(TimeframeId tf) const;

  std::filesystem::path _symbolDir;
  SubscriberId _subscriberId;
  mutable std::mutex _mutex;
  std::map<TimeframeId, std::vector<Bar>> _buffers;
  std::map<std::string, std::string> _metadata;
};

}  // namespace flox
