/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/readers/parallel_reader.h"

#include <algorithm>
#include <chrono>

namespace flox::replay
{

ParallelReader::ParallelReader(ParallelReaderConfig config) : _config(std::move(config))
{
  // Determine thread count
  _num_threads = _config.num_threads;
  if (_num_threads == 0)
  {
    _num_threads = std::thread::hardware_concurrency();
    if (_num_threads == 0)
    {
      _num_threads = 4;  // Fallback
    }
  }

  // Scan segments
  ReaderConfig reader_config{
      .data_dir = _config.data_dir,
      .from_ns = _config.from_ns,
      .to_ns = _config.to_ns,
      .symbols = _config.symbols,
      .verify_crc = _config.verify_crc,
  };

  BinaryLogReader scanner(reader_config);
  // Force scanning by calling summary()
  scanner.summary();
  _segments = scanner.segments();

  // Sort segments by first timestamp for proper merge ordering
  std::sort(_segments.begin(), _segments.end(), [](const SegmentInfo& a, const SegmentInfo& b)
            { return a.first_event_ns < b.first_event_ns; });
}

ParallelReader::~ParallelReader()
{
  _shutdown.store(true);
  _queue_cv.notify_all();

  for (auto& worker : _workers)
  {
    if (worker.joinable())
    {
      worker.join();
    }
  }
}

void ParallelReader::workerThread(size_t /*thread_id*/)
{
  while (true)
  {
    size_t segment_idx;

    {
      std::unique_lock lock(_queue_mutex);
      _queue_cv.wait(lock, [this]()
                     { return _shutdown.load() || !_work_queue.empty(); });

      if (_shutdown.load() && _work_queue.empty())
      {
        return;
      }

      segment_idx = _work_queue.front();
      _work_queue.pop();
    }

    // Read segment
    auto buffer = readSegment(_segments[segment_idx]);

    if (buffer)
    {
      std::lock_guard lock(_results_mutex);
      _results_queue.push(std::move(buffer));
      _results_cv.notify_one();
    }
  }
}

std::unique_ptr<SegmentBuffer> ParallelReader::readSegment(const SegmentInfo& segment)
{
  auto buffer = std::make_unique<SegmentBuffer>();
  buffer->info = segment;
  buffer->events.reserve(segment.event_count);

  BinaryLogIterator iter(segment.path);
  if (!iter.isValid())
  {
    return nullptr;
  }

  // Apply time range filter at seek level if possible
  if (_config.from_ns.has_value() && iter.hasIndex())
  {
    iter.seekToTimestamp(*_config.from_ns);
  }

  ReplayEvent event;
  while (iter.next(event))
  {
    // Apply time range filter
    if (_config.from_ns.has_value() && event.timestamp_ns < *_config.from_ns)
    {
      continue;
    }
    if (_config.to_ns.has_value() && event.timestamp_ns > *_config.to_ns)
    {
      break;  // Events are ordered by time
    }

    // Apply symbol filter
    if (!_config.symbols.empty())
    {
      uint32_t symbol_id = (event.type == EventType::Trade) ? event.trade.symbol_id
                                                            : event.book_header.symbol_id;
      if (_config.symbols.find(symbol_id) == _config.symbols.end())
      {
        continue;
      }
    }

    buffer->events.push_back(std::move(event));
  }

  return buffer;
}

void ParallelReader::mergeBuffers(std::vector<std::unique_ptr<SegmentBuffer>>& buffers,
                                  const EventCallback& callback)
{
  // Min-heap comparator: smallest timestamp first
  auto cmp = [](const SegmentBuffer* a, const SegmentBuffer* b)
  {
    return a->front().timestamp_ns > b->front().timestamp_ns;
  };

  std::priority_queue<SegmentBuffer*, std::vector<SegmentBuffer*>,
                      decltype(cmp)>
      heap(cmp);

  // Initialize heap with non-empty buffers
  for (auto& buffer : buffers)
  {
    if (buffer && !buffer->empty())
    {
      heap.push(buffer.get());
    }
  }

  // Merge
  while (!heap.empty())
  {
    SegmentBuffer* top = heap.top();
    heap.pop();

    if (!callback(top->front()))
    {
      return;  // Early exit requested
    }

    top->pop();

    if (!top->empty())
    {
      heap.push(top);
    }
  }
}

bool ParallelReader::passesFilter(const ReplayEvent& event) const
{
  if (_config.from_ns.has_value() && event.timestamp_ns < *_config.from_ns)
  {
    return false;
  }
  if (_config.to_ns.has_value() && event.timestamp_ns > *_config.to_ns)
  {
    return false;
  }
  if (!_config.symbols.empty())
  {
    uint32_t symbol_id =
        (event.type == EventType::Trade) ? event.trade.symbol_id : event.book_header.symbol_id;
    if (_config.symbols.find(symbol_id) == _config.symbols.end())
    {
      return false;
    }
  }
  return true;
}

uint64_t ParallelReader::forEach(EventCallback callback)
{
  if (_segments.empty())
  {
    return 0;
  }

  _stats.start_time_ns = time_utils::nowNanos();

  // For small datasets, use single-threaded approach
  if (_segments.size() <= 2 || _num_threads == 1)
  {
    uint64_t count = 0;
    for (const auto& segment : _segments)
    {
      auto buffer = readSegment(segment);
      if (buffer)
      {
        _stats.segments_processed++;
        _stats.events_read += buffer->events.size();

        for (const auto& event : buffer->events)
        {
          if (event.type == EventType::Trade)
          {
            _stats.trades_read++;
          }
          else
          {
            _stats.book_updates_read++;
          }

          if (!callback(event))
          {
            _stats.end_time_ns = time_utils::nowNanos();
            return count;
          }
          ++count;
        }
      }
    }
    _stats.end_time_ns = time_utils::nowNanos();
    return count;
  }

  // Multi-threaded approach with merge sort
  if (_config.sort_output)
  {
    // Read all segments in parallel, then merge
    std::vector<std::future<std::unique_ptr<SegmentBuffer>>> futures;
    futures.reserve(_segments.size());

    for (const auto& segment : _segments)
    {
      futures.push_back(std::async(std::launch::async,
                                   [this, &segment]()
                                   { return readSegment(segment); }));
    }

    // Collect results
    std::vector<std::unique_ptr<SegmentBuffer>> buffers;
    buffers.reserve(futures.size());

    for (auto& future : futures)
    {
      auto buffer = future.get();
      if (buffer)
      {
        _stats.segments_processed++;
        _stats.events_read += buffer->events.size();
        for (const auto& event : buffer->events)
        {
          if (event.type == EventType::Trade)
          {
            _stats.trades_read++;
          }
          else
          {
            _stats.book_updates_read++;
          }
        }
      }
      buffers.push_back(std::move(buffer));
    }

    // Merge and deliver events
    uint64_t count = 0;
    EventCallback counting_callback = [&count, &callback](const ReplayEvent& event)
    {
      ++count;
      return callback(event);
    };

    mergeBuffers(buffers, counting_callback);
    _stats.end_time_ns = time_utils::nowNanos();
    return count;
  }
  else
  {
    // Deliver events as they become available (no sorting)
    std::atomic<uint64_t> count{0};
    std::atomic<bool> stop_requested{false};

    std::vector<std::future<void>> futures;
    futures.reserve(_segments.size());

    std::mutex callback_mutex;

    for (const auto& segment : _segments)
    {
      futures.push_back(std::async(std::launch::async,
                                   [this, &segment, &callback, &count, &stop_requested,
                                    &callback_mutex]()
                                   {
                                     if (stop_requested.load())
                                     {
                                       return;
                                     }

                                     auto buffer = readSegment(segment);
                                     if (!buffer)
                                     {
                                       return;
                                     }

                                     for (const auto& event : buffer->events)
                                     {
                                       if (stop_requested.load())
                                       {
                                         return;
                                       }

                                       std::lock_guard lock(callback_mutex);
                                       if (!callback(event))
                                       {
                                         stop_requested.store(true);
                                         return;
                                       }
                                       count.fetch_add(1, std::memory_order_relaxed);
                                     }
                                   }));
    }

    // Wait for all
    for (auto& future : futures)
    {
      future.wait();
    }

    _stats.end_time_ns = time_utils::nowNanos();
    return count.load();
  }
}

uint64_t ParallelReader::forEachBatch(BatchCallback callback)
{
  if (_segments.empty())
  {
    return 0;
  }

  _stats.start_time_ns = time_utils::nowNanos();

  std::atomic<uint64_t> total_events{0};
  std::atomic<bool> stop_requested{false};

  std::vector<std::future<void>> futures;
  futures.reserve(_segments.size());

  for (const auto& segment : _segments)
  {
    futures.push_back(std::async(std::launch::async,
                                 [this, &segment, &callback, &total_events, &stop_requested]()
                                 {
                                   if (stop_requested.load())
                                   {
                                     return;
                                   }

                                   auto buffer = readSegment(segment);
                                   if (!buffer || buffer->events.empty())
                                   {
                                     return;
                                   }

                                   total_events.fetch_add(buffer->events.size(),
                                                          std::memory_order_relaxed);

                                   if (!callback(buffer->events))
                                   {
                                     stop_requested.store(true);
                                   }
                                 }));
  }

  // Wait for all
  for (auto& future : futures)
  {
    future.wait();
  }

  _stats.end_time_ns = time_utils::nowNanos();
  _stats.events_read = total_events.load();
  return total_events.load();
}

ParallelReaderStats ParallelReader::stats() const
{
  return _stats;
}

}  // namespace flox::replay
