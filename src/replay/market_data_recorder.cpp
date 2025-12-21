/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/market_data_recorder.h"

#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"

namespace flox
{

MarketDataRecorder::MarketDataRecorder(MarketDataRecorderConfig config)
    : _config(std::move(config))
{
  // Generate unique subscriber ID
  static std::atomic<SubscriberId> next_id{1000};
  _id = next_id.fetch_add(1, std::memory_order_relaxed);
}

MarketDataRecorder::~MarketDataRecorder() { stop(); }

void MarketDataRecorder::start()
{
  if (_recording.exchange(true))
  {
    return;  // Already recording
  }

  replay::WriterConfig writer_config{
      .output_dir = _config.output_dir,
      .max_segment_bytes = _config.max_segment_bytes,
      .exchange_id = _config.exchange_id,
  };

  _writer = std::make_unique<replay::BinaryLogWriter>(std::move(writer_config));
}

void MarketDataRecorder::stop()
{
  if (!_recording.exchange(false))
  {
    return;  // Not recording
  }

  if (_writer)
  {
    _writer->close();
    _writer.reset();
  }
}

void MarketDataRecorder::onBookUpdate(const BookUpdateEvent& event)
{
  if (!_recording.load(std::memory_order_relaxed) || !_writer)
  {
    return;
  }

  const auto& update = event.update;

  replay::BookRecordHeader header{};
  header.exchange_ts_ns = static_cast<int64_t>(update.exchangeTsNs);
  header.recv_ts_ns = static_cast<int64_t>(event.recvNs);
  header.seq = event.seq;
  header.symbol_id = update.symbol;
  header.bid_count = static_cast<uint16_t>(std::min(update.bids.size(), size_t{UINT16_MAX}));
  header.ask_count = static_cast<uint16_t>(std::min(update.asks.size(), size_t{UINT16_MAX}));
  header.type = (update.type == BookUpdateType::SNAPSHOT) ? 0 : 1;
  header.instrument = static_cast<uint8_t>(update.instrument);

  // Convert BookLevel to replay::BookLevel
  std::vector<replay::BookLevel> bids;
  std::vector<replay::BookLevel> asks;

  bids.reserve(header.bid_count);
  for (size_t i = 0; i < header.bid_count; ++i)
  {
    bids.push_back({update.bids[i].price.raw(), update.bids[i].quantity.raw()});
  }

  asks.reserve(header.ask_count);
  for (size_t i = 0; i < header.ask_count; ++i)
  {
    asks.push_back({update.asks[i].price.raw(), update.asks[i].quantity.raw()});
  }

  if (_writer->writeBook(header, bids, asks))
  {
    ++_stats.book_updates_written;
  }
  else
  {
    ++_stats.errors;
  }
}

void MarketDataRecorder::onTrade(const TradeEvent& event)
{
  if (!_recording.load(std::memory_order_relaxed) || !_writer)
  {
    return;
  }

  const auto& trade = event.trade;

  replay::TradeRecord record{};
  record.exchange_ts_ns = static_cast<int64_t>(trade.exchangeTsNs);
  record.recv_ts_ns = static_cast<int64_t>(event.recvNs);
  record.price_raw = trade.price.raw();
  record.qty_raw = trade.quantity.raw();
  record.trade_id = event.trade_id;
  record.symbol_id = trade.symbol;
  record.side = trade.isBuy ? 1 : 0;
  record.instrument = static_cast<uint8_t>(trade.instrument);
  record.exchange_id = event.sourceExchange;

  if (_writer->writeTrade(record))
  {
    ++_stats.trades_written;
  }
  else
  {
    ++_stats.errors;
  }
}

void MarketDataRecorder::onBar(const BarEvent& /*event*/)
{
  // Bars are not recorded - they can be reconstructed from trades
}

void MarketDataRecorder::setOutputDir(const std::filesystem::path& dir)
{
  _config.output_dir = dir;
  if (_writer)
  {
    // Restart writer with new directory
    stop();
    start();
  }
}

void MarketDataRecorder::flush()
{
  if (_writer)
  {
    _writer->flush();
  }
}

RecorderStats MarketDataRecorder::stats() const
{
  if (_writer)
  {
    auto ws = _writer->stats();
    _stats.bytes_written = ws.bytes_written;
    _stats.files_created = ws.segments_created;
  }
  return _stats;
}

}  // namespace flox
