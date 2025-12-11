/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/ops/segment_ops.h"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <map>
#include <sstream>

namespace flox::replay
{

static std::vector<std::filesystem::path> collectSegmentPaths(const std::filesystem::path& dir)
{
  std::vector<std::filesystem::path> paths;

  if (!std::filesystem::exists(dir))
  {
    return paths;
  }

  for (const auto& entry : std::filesystem::directory_iterator(dir))
  {
    if (entry.is_regular_file() && entry.path().extension() == ".floxlog")
    {
      paths.push_back(entry.path());
    }
  }

  std::sort(paths.begin(), paths.end());
  return paths;
}

static uint64_t estimateTotalEvents(const std::vector<std::filesystem::path>& paths)
{
  uint64_t total = 0;
  for (const auto& path : paths)
  {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f)
    {
      continue;
    }

    SegmentHeader header;
    if (std::fread(&header, sizeof(header), 1, f) == 1 && header.isValid())
    {
      total += header.event_count;
    }
    std::fclose(f);
  }
  return total;
}

MergeResult SegmentOps::merge(const std::vector<std::filesystem::path>& input_paths,
                              const MergeConfig& config)
{
  ProgressCallback no_progress = nullptr;
  return merge(input_paths, config, no_progress);
}

MergeResult SegmentOps::mergeDirectory(const std::filesystem::path& input_dir,
                                       const MergeConfig& config)
{
  auto paths = collectSegmentPaths(input_dir);
  return merge(paths, config);
}

MergeResult SegmentOps::merge(const std::vector<std::filesystem::path>& input_paths,
                              const MergeConfig& config, ProgressCallback progress)
{
  MergeResult result;

  if (input_paths.empty())
  {
    result.errors.push_back("No input files specified");
    return result;
  }

  // Create output directory
  std::filesystem::create_directories(config.output_dir);

  // Generate output filename if not specified
  std::string output_name = config.output_name;
  if (output_name.empty())
  {
    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    output_name = std::to_string(now_ns) + "_merged.floxlog";
  }

  result.output_path = config.output_dir / output_name;

  // Setup writer
  WriterConfig writer_config{.output_dir = config.output_dir,
                             .create_index = config.create_index,
                             .index_interval = config.index_interval,
                             .compression = config.compression};
  BinaryLogWriter writer(writer_config);

  uint64_t total_events = estimateTotalEvents(input_paths);
  uint64_t events_processed = 0;

  // If sorting by timestamp, we need to collect all events first
  if (config.sort_by_timestamp && input_paths.size() > 1)
  {
    // Collect all events
    std::vector<ReplayEvent> all_events;
    all_events.reserve(total_events);

    for (const auto& path : input_paths)
    {
      BinaryLogIterator iter(path);
      if (!iter.isValid())
      {
        result.errors.push_back("Cannot open: " + path.string());
        continue;
      }

      ReplayEvent event;
      while (iter.next(event))
      {
        all_events.push_back(event);
        ++events_processed;

        if (progress && events_processed % 10000 == 0)
        {
          progress(events_processed, total_events);
        }
      }
      ++result.segments_merged;
    }

    // Sort by timestamp
    std::sort(all_events.begin(), all_events.end(),
              [](const ReplayEvent& a, const ReplayEvent& b)
              {
                return a.timestamp_ns < b.timestamp_ns;
              });

    // Write sorted events
    for (const auto& event : all_events)
    {
      if (event.type == EventType::Trade)
      {
        writer.writeTrade(event.trade);
      }
      else
      {
        writer.writeBook(event.book_header, event.bids, event.asks);
      }
      ++result.events_written;
    }
  }
  else
  {
    // Simple sequential merge (no sorting)
    for (const auto& path : input_paths)
    {
      BinaryLogIterator iter(path);
      if (!iter.isValid())
      {
        result.errors.push_back("Cannot open: " + path.string());
        continue;
      }

      ReplayEvent event;
      while (iter.next(event))
      {
        if (event.type == EventType::Trade)
        {
          writer.writeTrade(event.trade);
        }
        else
        {
          writer.writeBook(event.book_header, event.bids, event.asks);
        }

        ++result.events_written;
        ++events_processed;

        if (progress && events_processed % 10000 == 0)
        {
          progress(events_processed, total_events);
        }
      }
      ++result.segments_merged;
    }
  }

  // Get output path before close (close clears the internal path)
  result.output_path = writer.currentSegmentPath();
  writer.close();

  auto stats = writer.stats();
  result.bytes_written = stats.bytes_written;
  result.success = result.errors.empty() && result.events_written > 0;

  return result;
}

SplitResult SegmentOps::split(const std::filesystem::path& input_path, const SplitConfig& config)
{
  ProgressCallback no_progress = nullptr;
  return split(input_path, config, no_progress);
}

SplitResult SegmentOps::splitDirectory(const std::filesystem::path& input_dir,
                                       const SplitConfig& config)
{
  auto paths = collectSegmentPaths(input_dir);

  SplitResult combined_result;
  combined_result.success = true;

  for (const auto& path : paths)
  {
    auto result = split(path, config);
    combined_result.segments_created += result.segments_created;
    combined_result.events_written += result.events_written;
    combined_result.output_paths.insert(combined_result.output_paths.end(),
                                        result.output_paths.begin(), result.output_paths.end());

    if (!result.success)
    {
      combined_result.success = false;
      combined_result.errors.insert(combined_result.errors.end(), result.errors.begin(),
                                    result.errors.end());
    }
  }

  return combined_result;
}

SplitResult SegmentOps::split(const std::filesystem::path& input_path, const SplitConfig& config,
                              ProgressCallback progress)
{
  SplitResult result;

  BinaryLogIterator iter(input_path);
  if (!iter.isValid())
  {
    result.errors.push_back("Cannot open input file: " + input_path.string());
    return result;
  }

  std::filesystem::create_directories(config.output_dir);

  uint64_t total_events = iter.header().event_count;
  uint64_t events_processed = 0;

  // For BySymbol mode, use a map of writers
  if (config.mode == SplitMode::BySymbol)
  {
    std::map<uint32_t, std::unique_ptr<BinaryLogWriter>> writers;

    auto getOrCreateWriter = [&](uint32_t symbol_id) -> BinaryLogWriter*
    {
      auto it = writers.find(symbol_id);
      if (it != writers.end())
      {
        return it->second.get();
      }

      auto path = generateSplitPath(config.output_dir, 0, config.mode, symbol_id);
      result.output_paths.push_back(path);

      WriterConfig wconfig{.output_dir = path.parent_path(),
                           .output_filename = path.filename().string(),
                           .create_index = config.create_index,
                           .index_interval = config.index_interval,
                           .compression = config.compression};
      auto w = std::make_unique<BinaryLogWriter>(wconfig);
      auto* ptr = w.get();
      writers[symbol_id] = std::move(w);
      return ptr;
    };

    ReplayEvent event;
    while (iter.next(event))
    {
      uint32_t symbol_id =
          (event.type == EventType::Trade) ? event.trade.symbol_id : event.book_header.symbol_id;
      auto* writer = getOrCreateWriter(symbol_id);

      if (event.type == EventType::Trade)
      {
        writer->writeTrade(event.trade);
      }
      else
      {
        writer->writeBook(event.book_header, event.bids, event.asks);
      }

      ++result.events_written;
      ++events_processed;

      if (progress && events_processed % 10000 == 0)
      {
        progress(events_processed, total_events);
      }
    }

    // Close all writers
    for (auto& [sid, w] : writers)
    {
      w->close();
      ++result.segments_created;
    }

    result.success = result.errors.empty();
    return result;
  }

  // Original logic for other modes
  std::unique_ptr<BinaryLogWriter> writer;
  uint32_t segment_index = 0;
  int64_t current_boundary = 0;
  uint64_t current_events = 0;
  uint64_t current_bytes = 0;

  auto startNewSegment = [&](int64_t boundary_value)
  {
    if (writer)
    {
      writer->close();
      ++result.segments_created;
    }

    auto path = generateSplitPath(config.output_dir, segment_index++, config.mode, boundary_value);
    result.output_paths.push_back(path);

    WriterConfig wconfig{.output_dir = path.parent_path(),
                         .create_index = config.create_index,
                         .index_interval = config.index_interval,
                         .compression = config.compression};
    writer = std::make_unique<BinaryLogWriter>(wconfig);
    current_events = 0;
    current_bytes = 0;
    current_boundary = boundary_value;
  };

  ReplayEvent event;
  while (iter.next(event))
  {
    bool need_new_segment = false;
    int64_t boundary_value = 0;

    switch (config.mode)
    {
      case SplitMode::ByTime:
        boundary_value = (event.timestamp_ns / config.time_interval_ns) * config.time_interval_ns;
        need_new_segment = !writer || boundary_value != current_boundary;
        break;

      case SplitMode::ByEventCount:
        need_new_segment = !writer || current_events >= config.events_per_file;
        boundary_value = segment_index;
        break;

      case SplitMode::BySize:
        need_new_segment = !writer || current_bytes >= config.bytes_per_file;
        boundary_value = segment_index;
        break;

      case SplitMode::BySymbol:
        // Handled above
        break;
    }

    if (need_new_segment)
    {
      startNewSegment(boundary_value);
    }

    // Write event
    if (event.type == EventType::Trade)
    {
      writer->writeTrade(event.trade);
      current_bytes += sizeof(FrameHeader) + sizeof(TradeRecord);
    }
    else
    {
      writer->writeBook(event.book_header, event.bids, event.asks);
      current_bytes += sizeof(FrameHeader) + sizeof(BookRecordHeader) +
                       event.bids.size() * sizeof(BookLevel) + event.asks.size() * sizeof(BookLevel);
    }

    ++current_events;
    ++result.events_written;
    ++events_processed;

    if (progress && events_processed % 10000 == 0)
    {
      progress(events_processed, total_events);
    }
  }

  // Close last segment
  if (writer)
  {
    writer->close();
    ++result.segments_created;
  }

  result.success = result.errors.empty();
  return result;
}

std::filesystem::path SegmentOps::generateSplitPath(const std::filesystem::path& output_dir,
                                                    uint32_t index, SplitMode mode,
                                                    int64_t boundary_value)
{
  std::ostringstream oss;

  switch (mode)
  {
    case SplitMode::ByTime:
    {
      // Format as timestamp
      auto tp = time_utils::fromNanos(boundary_value);
      auto time = std::chrono::system_clock::to_time_t(tp);
      oss << std::put_time(std::gmtime(&time), "%Y%m%d_%H%M%S");
      break;
    }
    case SplitMode::ByEventCount:
    case SplitMode::BySize:
      oss << "segment_" << std::setw(6) << std::setfill('0') << index;
      break;

    case SplitMode::BySymbol:
      oss << "symbol_" << boundary_value;
      break;
  }

  oss << ".floxlog";
  return output_dir / oss.str();
}

ExportResult SegmentOps::exportData(const std::filesystem::path& input_path,
                                    const ExportConfig& config)
{
  ProgressCallback no_progress = nullptr;
  return exportData(input_path, config, no_progress);
}

ExportResult SegmentOps::exportDirectory(const std::filesystem::path& input_dir,
                                         const ExportConfig& config)
{
  ExportResult result;
  result.output_path = config.output_path;

  auto paths = collectSegmentPaths(input_dir);
  if (paths.empty())
  {
    result.errors.push_back("No segment files found in directory");
    return result;
  }

  // For binary export, merge then export
  if (config.format == ExportFormat::Binary)
  {
    MergeConfig merge_config{.output_dir = config.output_path.parent_path(),
                             .output_name = config.output_path.filename().string(),
                             .create_index = config.create_index,
                             .index_interval = config.index_interval,
                             .compression = config.compression};
    auto merge_result = SegmentOps::mergeDirectory(input_dir, merge_config);
    result.success = merge_result.success;
    result.events_exported = merge_result.events_written;
    result.bytes_written = merge_result.bytes_written;
    result.errors = merge_result.errors;
    return result;
  }

  // For text formats, stream through all files
  std::FILE* out = std::fopen(config.output_path.string().c_str(), "w");
  if (!out)
  {
    result.errors.push_back("Cannot open output file: " + config.output_path.string());
    return result;
  }

  // Write header for CSV
  if (config.format == ExportFormat::CSV && config.include_header)
  {
    std::fprintf(out,
                 "type%ctimestamp_ns%csymbol_id%cprice%cqty%cside%ctrade_id%cbid_count%cask_count\n",
                 config.delimiter, config.delimiter, config.delimiter, config.delimiter,
                 config.delimiter, config.delimiter, config.delimiter, config.delimiter);
  }

  // JSON array start
  if (config.format == ExportFormat::JSON)
  {
    std::fprintf(out, "[\n");
  }

  bool first_json = true;

  for (const auto& path : paths)
  {
    BinaryLogIterator iter(path);
    if (!iter.isValid())
    {
      continue;
    }

    ReplayEvent event;
    while (iter.next(event))
    {
      // Apply filters
      if (config.from_ts && event.timestamp_ns < *config.from_ts)
      {
        continue;
      }
      if (config.to_ts && event.timestamp_ns > *config.to_ts)
      {
        continue;
      }

      if (!config.symbols.empty())
      {
        uint32_t sym =
            (event.type == EventType::Trade) ? event.trade.symbol_id : event.book_header.symbol_id;
        if (config.symbols.find(sym) == config.symbols.end())
        {
          continue;
        }
      }

      if (config.trades_only && event.type != EventType::Trade)
      {
        continue;
      }
      if (config.books_only && event.type == EventType::Trade)
      {
        continue;
      }

      // Format and write
      switch (config.format)
      {
        case ExportFormat::CSV:
          std::fprintf(out, "%s\n", formatCSVEvent(event, config.delimiter).c_str());
          break;

        case ExportFormat::JSON:
          if (!first_json)
          {
            std::fprintf(out, ",\n");
          }
          first_json = false;
          std::fprintf(out, "%s", formatJSONEvent(event, config.pretty_print, config.indent).c_str());
          break;

        case ExportFormat::JSONLines:
          std::fprintf(out, "%s\n", formatJSONEvent(event, false, 0).c_str());
          break;

        default:
          break;
      }

      ++result.events_exported;
    }
  }

  // JSON array end
  if (config.format == ExportFormat::JSON)
  {
    std::fprintf(out, "\n]\n");
  }

  result.bytes_written = static_cast<uint64_t>(std::ftell(out));
  std::fclose(out);

  result.success = result.errors.empty();
  return result;
}

ExportResult SegmentOps::exportData(const std::filesystem::path& input_path,
                                    const ExportConfig& config, ProgressCallback progress)
{
  ExportResult result;
  result.output_path = config.output_path;

  // For binary export, just copy/filter
  if (config.format == ExportFormat::Binary)
  {
    WriterConfig wconfig{.output_dir = config.output_path.parent_path(),
                         .create_index = config.create_index,
                         .index_interval = config.index_interval,
                         .compression = config.compression};

    auto predicate = [&config](const ReplayEvent& event)
    {
      if (config.from_ts && event.timestamp_ns < *config.from_ts)
      {
        return false;
      }
      if (config.to_ts && event.timestamp_ns > *config.to_ts)
      {
        return false;
      }

      if (!config.symbols.empty())
      {
        uint32_t sym =
            (event.type == EventType::Trade) ? event.trade.symbol_id : event.book_header.symbol_id;
        if (config.symbols.find(sym) == config.symbols.end())
        {
          return false;
        }
      }

      if (config.trades_only && event.type != EventType::Trade)
      {
        return false;
      }
      if (config.books_only && event.type == EventType::Trade)
      {
        return false;
      }

      return true;
    };

    result.events_exported = filter(input_path, config.output_path, predicate, wconfig);
    result.success = true;
    return result;
  }

  // Text format export
  BinaryLogIterator iter(input_path);
  if (!iter.isValid())
  {
    result.errors.push_back("Cannot open input file");
    return result;
  }

  std::FILE* out = std::fopen(config.output_path.string().c_str(), "w");
  if (!out)
  {
    result.errors.push_back("Cannot open output file");
    return result;
  }

  uint64_t total_events = iter.header().event_count;
  uint64_t events_processed = 0;

  // Write header for CSV
  if (config.format == ExportFormat::CSV && config.include_header)
  {
    std::fprintf(out,
                 "type%ctimestamp_ns%csymbol_id%cprice%cqty%cside%ctrade_id%cbid_count%cask_count\n",
                 config.delimiter, config.delimiter, config.delimiter, config.delimiter,
                 config.delimiter, config.delimiter, config.delimiter, config.delimiter);
  }

  // JSON array start
  if (config.format == ExportFormat::JSON)
  {
    std::fprintf(out, "[\n");
  }

  bool first_json = true;
  ReplayEvent event;

  while (iter.next(event))
  {
    // Apply filters
    if (config.from_ts && event.timestamp_ns < *config.from_ts)
    {
      continue;
    }
    if (config.to_ts && event.timestamp_ns > *config.to_ts)
    {
      continue;
    }

    if (!config.symbols.empty())
    {
      uint32_t sym =
          (event.type == EventType::Trade) ? event.trade.symbol_id : event.book_header.symbol_id;
      if (config.symbols.find(sym) == config.symbols.end())
      {
        continue;
      }
    }

    if (config.trades_only && event.type != EventType::Trade)
    {
      continue;
    }
    if (config.books_only && event.type == EventType::Trade)
    {
      continue;
    }

    // Format and write
    switch (config.format)
    {
      case ExportFormat::CSV:
        std::fprintf(out, "%s\n", formatCSVEvent(event, config.delimiter).c_str());
        break;

      case ExportFormat::JSON:
        if (!first_json)
        {
          std::fprintf(out, ",\n");
        }
        first_json = false;
        std::fprintf(out, "%s", formatJSONEvent(event, config.pretty_print, config.indent).c_str());
        break;

      case ExportFormat::JSONLines:
        std::fprintf(out, "%s\n", formatJSONEvent(event, false, 0).c_str());
        break;

      default:
        break;
    }

    ++result.events_exported;
    ++events_processed;

    if (progress && events_processed % 10000 == 0)
    {
      progress(events_processed, total_events);
    }
  }

  // JSON array end
  if (config.format == ExportFormat::JSON)
  {
    std::fprintf(out, "\n]\n");
  }

  result.bytes_written = static_cast<uint64_t>(std::ftell(out));
  std::fclose(out);

  result.success = true;
  return result;
}

std::string SegmentOps::formatCSVEvent(const ReplayEvent& event, char delimiter)
{
  std::ostringstream oss;

  if (event.type == EventType::Trade)
  {
    oss << "trade" << delimiter << event.trade.exchange_ts_ns << delimiter
        << event.trade.symbol_id << delimiter << event.trade.price_raw << delimiter
        << event.trade.qty_raw << delimiter << static_cast<int>(event.trade.side) << delimiter
        << event.trade.trade_id << delimiter << 0 << delimiter << 0;
  }
  else
  {
    const char* type_str =
        (event.type == EventType::BookSnapshot) ? "book_snapshot" : "book_delta";
    oss << type_str << delimiter << event.book_header.exchange_ts_ns << delimiter
        << event.book_header.symbol_id << delimiter << 0 << delimiter << 0 << delimiter << 0
        << delimiter << 0 << delimiter << event.book_header.bid_count << delimiter
        << event.book_header.ask_count;
  }

  return oss.str();
}

std::string SegmentOps::formatJSONEvent(const ReplayEvent& event, bool pretty, int indent)
{
  std::ostringstream oss;
  std::string ind = pretty ? std::string(indent, ' ') : "";
  std::string nl = pretty ? "\n" : "";
  std::string sp = pretty ? " " : "";

  if (event.type == EventType::Trade)
  {
    oss << "{" << nl << ind << "\"type\":" << sp << "\"trade\"," << nl << ind << "\"timestamp_ns\":"
        << sp << event.trade.exchange_ts_ns << "," << nl << ind << "\"symbol_id\":" << sp
        << event.trade.symbol_id << "," << nl << ind << "\"price_raw\":" << sp
        << event.trade.price_raw << "," << nl << ind << "\"qty_raw\":" << sp << event.trade.qty_raw
        << "," << nl << ind << "\"side\":" << sp << static_cast<int>(event.trade.side) << "," << nl
        << ind << "\"trade_id\":" << sp << event.trade.trade_id << nl << "}";
  }
  else
  {
    const char* type_str =
        (event.type == EventType::BookSnapshot) ? "book_snapshot" : "book_delta";
    oss << "{" << nl << ind << "\"type\":" << sp << "\"" << type_str << "\"," << nl << ind
        << "\"timestamp_ns\":" << sp << event.book_header.exchange_ts_ns << "," << nl << ind
        << "\"symbol_id\":" << sp << event.book_header.symbol_id << "," << nl << ind
        << "\"bid_count\":" << sp << event.book_header.bid_count << "," << nl << ind
        << "\"ask_count\":" << sp << event.book_header.ask_count << nl << "}";
  }

  return oss.str();
}

bool SegmentOps::recompress(const std::filesystem::path& input_path,
                            const std::filesystem::path& output_path,
                            CompressionType new_compression)
{
  WriterConfig config{.output_dir = output_path.parent_path(),
                      .output_filename = output_path.filename().string(),
                      .create_index = true,
                      .compression = new_compression};

  BinaryLogIterator iter(input_path);
  if (!iter.isValid())
  {
    return false;
  }

  BinaryLogWriter writer(config);

  ReplayEvent event;
  while (iter.next(event))
  {
    if (event.type == EventType::Trade)
    {
      writer.writeTrade(event.trade);
    }
    else
    {
      writer.writeBook(event.book_header, event.bids, event.asks);
    }
  }

  writer.close();
  return true;
}

uint64_t SegmentOps::filter(const std::filesystem::path& input_path,
                            const std::filesystem::path& output_path,
                            const std::function<bool(const ReplayEvent&)>& predicate,
                            const WriterConfig& output_config)
{
  BinaryLogIterator iter(input_path);
  if (!iter.isValid())
  {
    return 0;
  }

  BinaryLogWriter writer(output_config);
  uint64_t count = 0;

  ReplayEvent event;
  while (iter.next(event))
  {
    if (predicate(event))
    {
      if (event.type == EventType::Trade)
      {
        writer.writeTrade(event.trade);
      }
      else
      {
        writer.writeBook(event.book_header, event.bids, event.asks);
      }
      ++count;
    }
  }

  writer.close();
  return count;
}

uint64_t SegmentOps::extractSymbols(const std::filesystem::path& input_path,
                                    const std::filesystem::path& output_path,
                                    const std::set<uint32_t>& symbols, const WriterConfig& config)
{
  auto predicate = [&symbols](const ReplayEvent& event)
  {
    uint32_t sym =
        (event.type == EventType::Trade) ? event.trade.symbol_id : event.book_header.symbol_id;
    return symbols.find(sym) != symbols.end();
  };

  return filter(input_path, output_path, predicate, config);
}

uint64_t SegmentOps::extractTimeRange(const std::filesystem::path& input_path,
                                      const std::filesystem::path& output_path, int64_t from_ns,
                                      int64_t to_ns, const WriterConfig& config)
{
  auto predicate = [from_ns, to_ns](const ReplayEvent& event)
  {
    return event.timestamp_ns >= from_ns && event.timestamp_ns <= to_ns;
  };

  return filter(input_path, output_path, predicate, config);
}

}  // namespace flox::replay
