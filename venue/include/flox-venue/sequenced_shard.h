/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/journal.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/messages.h"
#include "flox/book/matching_book.h"

#include "flox/util/eventing/event_bus.h"

#include <cstdint>
#include <string>
#include <utility>

namespace flox::venue
{

// ---- ingress: normalized commands from every gateway ----
struct InboundCommandEvent;
struct ICommandListener
{
  virtual ~ICommandListener() = default;
  virtual void onCommand(const InboundCommandEvent& ev) = 0;
};
struct InboundCommandEvent
{
  using Listener = ICommandListener;
  InboundCommand cmd{};
  uint64_t tickSequence = 0;  // gateway sequence number, stamped by the bus
};

// ---- outbound: engine events fanned out to exec-report / market-data / ... ----
struct EngineEventMsg;
struct IEngineEventListener
{
  virtual ~IEngineEventListener() = default;
  virtual void onEngineEvent(const EngineEventMsg& ev) = 0;
};
struct EngineEventMsg
{
  using Listener = IEngineEventListener;
  OutboundEvent event{};
  uint64_t tickSequence = 0;
};

}  // namespace flox::venue

namespace flox
{
template <>
struct EventDispatcher<flox::venue::InboundCommandEvent>
{
  static void dispatch(const flox::venue::InboundCommandEvent& ev, flox::venue::ICommandListener& l)
  {
    l.onCommand(ev);
  }
};
template <>
struct EventDispatcher<flox::venue::EngineEventMsg>
{
  static void dispatch(const flox::venue::EngineEventMsg& ev, flox::venue::IEngineEventListener& l)
  {
    l.onEngineEvent(ev);
  }
};
}  // namespace flox

namespace flox::venue
{

template <class Book = MatchingBook, size_t IngressCap = 1 << 16, size_t OutboundCap = 1 << 16>
class SequencedShard
{
 public:
  using IngressBus = flox::EventBus<InboundCommandEvent, IngressCap, 4>;
  using OutboundBus = flox::EventBus<EngineEventMsg, OutboundCap, 8>;

  SequencedShard(SymbolConfig cfg, const std::string& journalPath, Book book = Book{})
      : journal_(journalPath),
        consumer_(cfg, journal_, outbound_, std::move(book))
  {
    ingress_.enableDrainOnStop();
    outbound_.enableDrainOnStop();
  }

  // Subscribe an outbound consumer (exec-report, market-data, ...). Must be
  // called before start().
  bool subscribeOutbound(IEngineEventListener* l, bool required = true)
  {
    return outbound_.subscribe(l, required);
  }

  void start()
  {
    outbound_.start();  // must be live before the matching thread publishes
    ingress_.subscribe(&consumer_, true);
    ingress_.start();
  }

  // Producer side (gateway). Returns the ingress sequence number.
  int64_t submit(const InboundCommand& cmd) { return ingress_.publish(InboundCommandEvent{cmd}); }

  void flush()
  {
    ingress_.flush();
    outbound_.flush();
  }

  void stop()
  {
    ingress_.flush();
    ingress_.stop();
    outbound_.flush();
    outbound_.stop();
    journal_.flush();
  }

  uint64_t journaled() const noexcept { return journal_.count(); }

 private:
  class MatchingConsumer : public ICommandListener
  {
   public:
    MatchingConsumer(SymbolConfig cfg, Journal& journal, OutboundBus& out, Book book)
        : journal_(journal),
          out_(out),
          engine_(cfg, [this](const OutboundEvent& ev)
                  { out_.publish(EngineEventMsg{ev}); }, std::move(book))
    {
    }

    void onCommand(const InboundCommandEvent& ev) override
    {
      journal_.append(ev.cmd);  // write-ahead, before applying
      engine_.submit(ev.cmd);
    }

   private:
    Journal& journal_;
    OutboundBus& out_;
    MatchingEngine<Book> engine_;
  };

  Journal journal_;
  OutboundBus outbound_;
  MatchingConsumer consumer_;
  IngressBus ingress_;
};

}  // namespace flox::venue
