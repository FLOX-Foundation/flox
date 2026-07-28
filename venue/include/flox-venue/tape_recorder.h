/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/messages.h"

#include "flox/replay/binary_format_v1.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace flox::venue
{

class TapeRecorder
{
 public:
  TapeRecorder(const std::string& dir, const std::string& filename)
  {
    flox::replay::WriterConfig cfg;
    cfg.output_dir = dir;
    cfg.output_filename = filename;
    writer_ = std::make_unique<flox::replay::BinaryLogWriter>(std::move(cfg));
    writer_->setHasTrades(true);
  }

  // Ingests the engine's outbound stream; persists every trade print.
  // `tsNs` should be the sequencer-stamped time; here a monotonic counter keeps
  // the tape sorted deterministically without a wall-clock dependency.
  void onEvent(const OutboundEvent& e)
  {
    const auto* t = std::get_if<Trade>(&e);
    if (t == nullptr)
    {
      return;
    }
    flox::replay::TradeRecord rec{};
    rec.exchange_ts_ns = ++ts_;
    rec.recv_ts_ns = ts_;
    rec.price_raw = t->price.raw();
    rec.qty_raw = t->quantity.raw();
    rec.trade_id = t->tradeId;
    rec.symbol_id = t->symbol;
    rec.side = static_cast<uint8_t>(t->takerSide);
    writer_->writeTrade(rec);
  }

  void close()
  {
    if (writer_)
    {
      writer_->close();
    }
  }

  uint64_t tradesWritten() const
  {
    return writer_ ? writer_->stats().trades_written : 0;
  }

 private:
  std::unique_ptr<flox::replay::BinaryLogWriter> writer_;
  int64_t ts_{0};
};

}  // namespace flox::venue
