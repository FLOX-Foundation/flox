/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

/**
 * Pre-aggregate bars from raw trade data and save to mmap-friendly binary files.
 *
 * This tool reads binary log files (trades) created by BinaryLogWriter and
 * produces bar files that can be loaded by MmapBarStorage for fast backtesting.
 *
 * Usage: preagg_bars <input_dir> <output_dir> [timeframes...]
 * Example: preagg_bars data/bybit/BTCUSDT data/bybit/BTCUSDT/bars 60 300 900 3600
 *
 * Output files: bars_60s.bin, bars_300s.bin, etc.
 */

#include "flox/aggregator/bar.h"
#include "flox/aggregator/multi_timeframe_aggregator.h"
#include "flox/aggregator/timeframe.h"
#include "flox/backtest/mmap_bar_writer.h"
#include "flox/common.h"
#include "flox/replay/abstract_event_reader.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

using namespace flox;

// Read .symbol_metadata file (key=value format)
std::map<std::string, std::string> readSymbolMetadata(const std::filesystem::path& dir)
{
  std::map<std::string, std::string> metadata;
  auto metadataPath = dir / ".symbol_metadata";

  if (std::filesystem::exists(metadataPath))
  {
    std::ifstream file(metadataPath);
    std::string line;
    while (std::getline(file, line))
    {
      auto pos = line.find('=');
      if (pos != std::string::npos)
      {
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        metadata[key] = value;
      }
    }
  }

  return metadata;
}

// Write .symbol_metadata file
void writeSymbolMetadata(const std::filesystem::path& dir,
                         const std::map<std::string, std::string>& metadata)
{
  auto metadataPath = dir / ".symbol_metadata";
  std::ofstream file(metadataPath);

  for (const auto& [key, value] : metadata)
  {
    file << key << "=" << value << "\n";
  }
}

void printUsage(const char* progName)
{
  std::cerr << "Usage: " << progName << " <input_dir> <output_dir> [timeframe_seconds...]\n\n";
  std::cerr << "Arguments:\n";
  std::cerr << "  input_dir   Directory containing binary log files (.bin)\n";
  std::cerr << "  output_dir  Directory to write bar files (will be created)\n";
  std::cerr << "  timeframes  List of timeframe intervals in seconds (default: 60 300 900 3600)\n";
  std::cerr << "\nExamples:\n";
  std::cerr << "  " << progName << " data/BTCUSDT bars/BTCUSDT 60 300 900\n";
  std::cerr << "  " << progName << " /path/to/trades /path/to/bars 60 300 900 1800 3600\n";
  std::cerr << "\nOutput:\n";
  std::cerr << "  Creates files like bars_60s.bin, bars_300s.bin, etc.\n";
  std::cerr << "  These files can be loaded with MmapBarStorage for backtesting.\n";
}

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    printUsage(argv[0]);
    return 1;
  }

  std::filesystem::path inputDir = argv[1];
  std::filesystem::path outputDir = argv[2];

  if (!std::filesystem::exists(inputDir))
  {
    std::cerr << "Error: Input directory not found: " << inputDir << "\n";
    return 1;
  }

  // Parse timeframes (default: 1m, 5m, 15m, 1h)
  std::vector<int> timeframeSecs;
  if (argc > 3)
  {
    for (int i = 3; i < argc; ++i)
    {
      try
      {
        timeframeSecs.push_back(std::stoi(argv[i]));
      }
      catch (const std::exception& e)
      {
        std::cerr << "Error: Invalid timeframe '" << argv[i] << "': " << e.what() << "\n";
        return 1;
      }
    }
  }
  else
  {
    // Default timeframes
    timeframeSecs = {60, 300, 900, 3600};
  }

  if (timeframeSecs.size() > 8)
  {
    std::cerr << "Error: Maximum 8 timeframes supported\n";
    return 1;
  }

  // Create output directory
  std::filesystem::create_directories(outputDir);

  std::cout << "=== Pre-aggregating Bars ===\n\n";
  std::cout << "Input:  " << inputDir << "\n";
  std::cout << "Output: " << outputDir << "\n";
  std::cout << "Timeframes: ";
  for (size_t i = 0; i < timeframeSecs.size(); ++i)
  {
    if (i > 0)
    {
      std::cout << ", ";
    }
    int tf = timeframeSecs[i];
    if (tf >= 3600)
    {
      std::cout << (tf / 3600) << "h";
    }
    else if (tf >= 60)
    {
      std::cout << (tf / 60) << "m";
    }
    else
    {
      std::cout << tf << "s";
    }
  }
  std::cout << "\n\n";

  // Create bar bus and aggregator
  BarBus bus;
  MultiTimeframeAggregator<8> aggregator(&bus);

  for (int tfSec : timeframeSecs)
  {
    aggregator.addTimeInterval(std::chrono::seconds(tfSec));
  }

  // Create writer and subscribe to bus
  MmapBarWriter writer(outputDir);
  bus.subscribe(&writer);

  // Create reader
  replay::ReaderFilter filter;
  auto reader = replay::createMultiSegmentReader(inputDir, filter);
  if (!reader)
  {
    std::cerr << "Error: Failed to create reader for: " << inputDir << "\n";
    std::cerr << "Make sure the directory contains valid .bin log files.\n";
    return 1;
  }

  auto segments = reader->segments();
  std::cout << "Found " << segments.size() << " segment(s), "
            << reader->totalEvents() << " total events\n\n";

  // Start aggregation
  aggregator.start();

  // Process trades
  std::cout << "Processing trades...\n";
  size_t tradeCount = 0;
  auto startTime = std::chrono::steady_clock::now();

  reader->forEach(
      [&](const replay::ReplayEvent& ev)
      {
        if (ev.type == replay::EventType::Trade)
        {
          TradeEvent trade;
          trade.trade.symbol = ev.trade.symbol_id;
          trade.trade.price = Price::fromRaw(ev.trade.price_raw);
          trade.trade.quantity = Quantity::fromRaw(ev.trade.qty_raw);
          trade.trade.exchangeTsNs = ev.trade.exchange_ts_ns;
          trade.trade.isBuy = (ev.trade.side == 1);
          trade.trade.instrument = static_cast<InstrumentType>(ev.trade.instrument);
          trade.exchangeMsgTsNs = ev.trade.exchange_ts_ns;

          aggregator.onTrade(trade);
          ++tradeCount;

          if (tradeCount % 500000 == 0)
          {
            std::cout << "  " << tradeCount << " trades processed...\n";
          }
        }
        return true;
      });

  // Stop aggregation (flushes partial bars)
  aggregator.stop();

  // Flush writer to disk
  writer.flush();

  auto endTime = std::chrono::steady_clock::now();
  auto durationMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

  std::cout << "\n=== Complete ===\n";
  std::cout << "Processed: " << tradeCount << " trades\n";
  std::cout << "Duration:  " << durationMs << " ms\n";
  std::cout << "Speed:     " << (tradeCount * 1000 / std::max(1L, durationMs)) << " trades/sec\n";
  std::cout << "\nOutput files:\n";

  for (int tfSec : timeframeSecs)
  {
    std::string filename = "bars_" + std::to_string(tfSec) + "s.bin";
    auto path = outputDir / filename;
    if (std::filesystem::exists(path))
    {
      auto size = std::filesystem::file_size(path);
      size_t barCount = (size > 8) ? ((size - 8) / sizeof(Bar)) : 0;
      std::cout << "  " << filename << ": " << barCount << " bars\n";
    }
  }

  std::cout << "\nBar files are ready for use with MmapBarStorage.\n";

  return 0;
}
