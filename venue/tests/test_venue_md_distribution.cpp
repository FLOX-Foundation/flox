/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Unicast market-data distribution over real loopback sockets: a subscriber
 * that joins mid-stream converges on the book of a consumer that watched from
 * the first message; a subscriber that stops reading is dropped by the bounded
 * queue while a healthy one alongside it keeps up and the publisher never
 * waits; a gap is filled from the resend ring, or answered with the explicit
 * "you need a snapshot" when it has been trimmed; a publisher restart surfaces
 * as an epoch change; the FIX encoding runs the request/snapshot/incremental/
 * reject round trip; and both encodings run on one server at the same time.
 */
#include "flox-venue/fix_md_codec.h"
#include "flox-venue/market_data.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/md_distribution.h"
#include "flox-venue/metrics.h"
#include "flox-venue/sbe_md_codec.h"

#include "flox/util/transport.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr SymbolId SYM = 1;
constexpr SymbolId OTHER = 2;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}

NewOrder limit(OrderId id, Side s, double p, double q, uint64_t acct = 1)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = acct;
  return o;
}

// Per-order consumer book driven by the public feed, folded to
// (side, price) -> aggregate for comparisons. Same shape as the multicast
// consumer in test_venue_md_recovery.
struct ConsumerBook
{
  struct O
  {
    Side side{};
    Price price{};
    Quantity qty{};
  };
  std::unordered_map<OrderId, O> orders;

  void clear() { orders.clear(); }

  void apply(const MdMessage& m)
  {
    switch (m.type)
    {
      case MdType::AddOrder:
        orders[m.id] = O{m.side, m.price, m.qty};
        break;
      case MdType::Executed:
      {
        auto it = orders.find(m.id);
        if (it != orders.end())
        {
          it->second.qty = m.qty;
          if (m.qty.raw() == 0)
          {
            orders.erase(it);
          }
        }
        break;
      }
      case MdType::Cancel:
        orders.erase(m.id);
        break;
      case MdType::Replace:
        orders[m.id] = O{m.side, m.price, m.qty};
        break;
      case MdType::Trade:
      case MdType::Triggered:
      case MdType::TradingStatus:
      case MdType::DerivativesUpdate:
        break;
    }
  }

  std::map<std::pair<int, int64_t>, int64_t> levels() const
  {
    std::map<std::pair<int, int64_t>, int64_t> lv;
    for (const auto& [id, o] : orders)
    {
      if (o.qty.raw() > 0)
      {
        lv[{static_cast<int>(o.side), o.price.raw()}] += o.qty.raw();
      }
    }
    return lv;
  }
};

bool sameBook(const ConsumerBook& a, const ConsumerBook& b) { return a.levels() == b.levels(); }

int dialLoopback(int port, int recvTimeoutMs, int recvBufBytes = 0)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
  {
    return -1;
  }
  if (recvBufBytes > 0)
  {
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof recvBufBytes);
  }
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0)
  {
    ::close(fd);
    return -1;
  }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  timeval tv{recvTimeoutMs / 1000, (recvTimeoutMs % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return fd;
}

// Binary-encoding subscriber: the same SBE templates the multicast consumer
// decodes, carried in the same length-prefixed frames.
struct SbeClient
{
  int fd{-1};

  bool open(int port, int recvTimeoutMs = 700, int recvBufBytes = 0)
  {
    fd = dialLoopback(port, recvTimeoutMs, recvBufBytes);
    return fd >= 0;
  }

  void close()
  {
    if (fd >= 0)
    {
      ::close(fd);
      fd = -1;
    }
  }
  ~SbeClient() { close(); }

  bool subscribe(SymbolId s, uint64_t fromSeq = 0, bool snapshotOnly = false)
  {
    std::vector<uint8_t> b;
    SbeMdCodec::encode(MdSubscribeRequest{s, fromSeq, snapshotOnly}, b);
    return net::writeFrame(fd, b.data(), b.size());
  }

  bool unsubscribe(SymbolId s)
  {
    std::vector<uint8_t> b;
    SbeMdCodec::encode(MdUnsubscribeRequest{s}, b);
    return net::writeFrame(fd, b.data(), b.size());
  }

  bool resend(SymbolId s, uint64_t fromSeq)
  {
    std::vector<uint8_t> b;
    SbeMdCodec::encode(MdResendRequest{s, fromSeq}, b);
    return net::writeFrame(fd, b.data(), b.size());
  }

  // Next non-empty frame (an empty frame is the server's heartbeat).
  bool next(std::vector<uint8_t>& f)
  {
    while (net::readFrame(fd, f))
    {
      if (!f.empty())
      {
        return true;
      }
    }
    return false;
  }

  SbeMdCodec::Tmpl tmpl(const std::vector<uint8_t>& f) const
  {
    return static_cast<SbeMdCodec::Tmpl>(SbeMdCodec::templateId(f.data(), f.size()));
  }
};

// Read one whole SBE snapshot (Begin, body, End) into `book`; returns the
// SnapshotBegin. `expectRequired` demands the explicit "you need a snapshot"
// message in front of it.
// Instrument state a snapshot carried alongside the book (absent when the
// publisher has published none).
struct SnapshotState
{
  bool hasStatus{false};
  MdMessage status{};
  bool hasDerivatives{false};
  MdMessage derivatives{};
  size_t beforeFirstOrder{0};  // state frames seen ahead of the book body
};

bool readSnapshot(SbeClient& c, ConsumerBook& book, MdSnapshotBegin& begin,
                  bool expectRequired = false, SnapshotState* state = nullptr)
{
  std::vector<uint8_t> f;
  if (expectRequired)
  {
    if (!c.next(f) || c.tmpl(f) != SbeMdCodec::Tmpl::SnapshotRequired)
    {
      return false;
    }
  }
  if (!c.next(f) || c.tmpl(f) != SbeMdCodec::Tmpl::SnapshotBegin ||
      !SbeMdCodec::decode(f.data(), f.size(), begin))
  {
    return false;
  }
  book.clear();
  size_t got = 0;
  while (c.next(f))
  {
    if (c.tmpl(f) == SbeMdCodec::Tmpl::SnapshotEnd)
    {
      MdSnapshotEnd end{};
      return SbeMdCodec::decode(f.data(), f.size(), end) && end.lastSeq == begin.lastSeq &&
             got == begin.orderCount;
    }
    MdMessage m;
    if (!SbeMdCodec::decode(f.data(), f.size(), m))
    {
      return false;
    }
    // The instrument's current state travels in the body too, ahead of the
    // orders; it is not part of orderCount.
    if (m.type == MdType::TradingStatus || m.type == MdType::DerivativesUpdate)
    {
      if (state != nullptr)
      {
        if (m.type == MdType::TradingStatus)
        {
          state->hasStatus = true;
          state->status = m;
        }
        else
        {
          state->hasDerivatives = true;
          state->derivatives = m;
        }
        state->beforeFirstOrder += got == 0 ? 1 : 0;
      }
      continue;
    }
    book.apply(m);
    ++got;
  }
  return false;
}

// ---- FIX subscriber ----

struct FixClient
{
  int fd{-1};
  uint64_t outSeq{1};
  std::vector<uint8_t> in;

  bool open(int port, int recvTimeoutMs = 700)
  {
    fd = dialLoopback(port, recvTimeoutMs);
    return fd >= 0;
  }

  void close()
  {
    if (fd >= 0)
    {
      ::close(fd);
      fd = -1;
    }
  }
  ~FixClient() { close(); }

  bool send(const std::string& body)
  {
    const std::string msg = FixCodec::frame(body);
    return net::writeAll(fd, reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
  }

  std::string header(const char* msgType)
  {
    std::string b;
    FixMdCodec::add(b, 35, msgType);
    FixMdCodec::add(b, 34, std::to_string(outSeq++));
    FixMdCodec::add(b, 49, "CLIENT");
    FixMdCodec::add(b, 56, "VENUE");
    FixMdCodec::add(b, 52, FixMdCodec::nowStamp());
    return b;
  }

  bool logon()
  {
    std::string b = header("A");
    FixMdCodec::add(b, 98, "0");
    FixMdCodec::add(b, 108, "30");
    return send(b);
  }

  // MarketDataRequest. subType: "0" snapshot, "1" snapshot+updates, "2" unsubscribe.
  bool request(const std::string& reqId, const char* subType, const std::vector<SymbolId>& syms,
               const std::vector<const char*>& entryTypes = {"0", "1", "2"},
               const char* depth = "0")
  {
    std::string b = header("V");
    FixMdCodec::add(b, 262, reqId);
    FixMdCodec::add(b, 263, subType);
    if (depth != nullptr)
    {
      FixMdCodec::add(b, 264, depth);
    }
    FixMdCodec::add(b, 267, std::to_string(entryTypes.size()));
    for (const char* e : entryTypes)
    {
      FixMdCodec::add(b, 269, e);
    }
    FixMdCodec::add(b, 146, std::to_string(syms.size()));
    for (SymbolId s : syms)
    {
      FixMdCodec::add(b, 55, std::to_string(s));
    }
    return send(b);
  }

  // Next complete FIX message, or empty on timeout.
  std::string next()
  {
    while (true)
    {
      size_t len = 0;
      if (!in.empty())
      {
        const int st = FixMdCodec::messageLength(in.data(), in.size(), len);
        if (st < 0)
        {
          return {};
        }
        if (st == 1)
        {
          std::string msg(reinterpret_cast<const char*>(in.data()), len);
          in.erase(in.begin(), in.begin() + static_cast<long>(len));
          return msg;
        }
      }
      uint8_t chunk[4096];
      const ssize_t r = ::recv(fd, chunk, sizeof chunk, 0);
      if (r <= 0)
      {
        return {};
      }
      in.insert(in.end(), chunk, chunk + r);
    }
  }

  // Next message whose MsgType is `type`, skipping heartbeats. Empty on timeout.
  std::string nextOf(const char* type)
  {
    for (int i = 0; i < 64; ++i)
    {
      const std::string m = next();
      if (m.empty())
      {
        return {};
      }
      if (FixMdCodec::first(FixMdCodec::parseOrdered(m), 35) == type)
      {
        return m;
      }
    }
    return {};
  }
};

bool hasField(const std::string& msg, const std::string& field)
{
  const std::string needle = std::string(1, FixCodec::SOH) + field + FixCodec::SOH;
  return msg.find(needle) != std::string::npos;
}

MdDistributionConfig testConfig()
{
  MdDistributionConfig c;
  c.readTimeoutMs = 40;
  c.idleTimeoutMs = 60000;  // the liveness policy has its own case
  c.heartbeatMs = 60000;
  return c;
}

// A subscriber that joins after the market has been trading sees an atomic
// snapshot and then the increments that follow it, and converges on the book
// of a consumer that watched from the very first message. The subscribe is
// deliberately raced against live traffic: orders are submitted while the
// snapshot is being taken.
void test_subscribe_midstream()
{
  std::printf("test_subscribe_midstream\n");
  MdCounters counters;
  ConsumerBook refBook;
  MdDistributionServer dist(&counters, testConfig());

  MarketDataPublisher<> md([&](const MdMessage& m)
                           {
                             refBook.apply(m);
                             dist.publish(m); },
                           px(0.01), SYM);
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { md.onEvent(e, eng.engineTimeNs()); });
  dist.addPublisher(md);
  CHECK(dist.start(0) > 0);

  // Phase A: nobody is listening yet.
  for (int i = 1; i <= 5; ++i)
  {
    eng.submit(limit(static_cast<OrderId>(i), Side::SELL, 100 + i, i));
    eng.submit(limit(static_cast<OrderId>(5 + i), Side::BUY, 100 - i, i));
  }
  eng.submit(limit(11, Side::BUY, 101, 0.5, 2));  // partial fill against id 1
  const uint64_t beforeJoin = md.seq();
  CHECK(beforeJoin >= 11);

  SbeClient c;
  CHECK(c.open(dist.port()));
  CHECK(c.subscribe(SYM));

  // Phase B: the market keeps moving while the snapshot is being served.
  eng.submit(CancelOrder{2, SYM, 1});
  eng.submit(ModifyOrder{3, SYM, px(106), qty(2), 1});
  eng.submit(limit(12, Side::SELL, 107, 3));
  eng.submit(limit(13, Side::BUY, 94, 2));
  eng.submit(limit(14, Side::BUY, 103, 10, 2));  // sweeps the 101 remainder, rests at 103
  const uint64_t target = md.seq();
  CHECK(target > beforeJoin);

  ConsumerBook joined;
  MdSnapshotBegin begin{};
  CHECK(readSnapshot(c, joined, begin));
  CHECK(begin.epoch == md.epoch());
  CHECK(begin.lastSeq >= beforeJoin);

  uint64_t applied = begin.lastSeq;
  std::vector<uint8_t> f;
  while (applied < target && c.next(f))
  {
    MdMessage m;
    CHECK(SbeMdCodec::decode(f.data(), f.size(), m));
    CHECK(m.seq == applied + 1);  // strictly in order, no hole across the handover
    CHECK(m.epoch == md.epoch());
    joined.apply(m);
    applied = m.seq;
  }
  CHECK(applied == target);
  CHECK(sameBook(joined, refBook));
  CHECK(counters.snapshotsServed.load() == 1);
  CHECK(counters.subscribers.load() == 1);

  c.close();
  dist.stop();
}

// A subscriber that stops reading fills its bounded queue and is dropped; a
// healthy subscriber alongside it keeps receiving, and the publishing thread
// never waits on either socket.
void test_slow_consumer_disconnected()
{
  std::printf("test_slow_consumer_disconnected\n");
  MdCounters counters;
  ConsumerBook refBook;
  MdDistributionConfig dcfg = testConfig();
  dcfg.queueCapacity = 512;
  dcfg.pendingCapacity = 512;
  dcfg.sendBufferBytes = 4096;  // do not let the kernel hide the backlog
  MdDistributionServer dist(&counters, dcfg);

  MarketDataPublisher<> md([&](const MdMessage& m)
                           {
                             refBook.apply(m);
                             dist.publish(m); },
                           px(0.01), SYM, 0, /*resendCapacity*/ 1u << 16);
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { md.onEvent(e, eng.engineTimeNs()); });
  dist.addPublisher(md);
  CHECK(dist.start(0) > 0);

  SbeClient slow;
  SbeClient healthy;
  CHECK(slow.open(dist.port(), 700, /*recvBuf*/ 2048));
  CHECK(healthy.open(dist.port(), 700));
  CHECK(slow.subscribe(SYM));
  CHECK(healthy.subscribe(SYM));

  // Let both subscriptions arm before the flood.
  ConsumerBook healthyBook;
  MdSnapshotBegin begin{};
  CHECK(readSnapshot(healthy, healthyBook, begin));

  // The healthy subscriber drains continuously, exactly as a real consumer
  // would; the slow one never reads a byte.
  std::atomic<uint64_t> applied{begin.lastSeq};
  std::atomic<bool> stopReader{false};
  std::thread reader(
      [&]
      {
        std::vector<uint8_t> f;
        while (!stopReader.load(std::memory_order_relaxed))
        {
          if (!healthy.next(f))
          {
            continue;  // read timeout tick
          }
          MdMessage m;
          if (!SbeMdCodec::decode(f.data(), f.size(), m))
          {
            continue;
          }
          healthyBook.apply(m);
          applied.store(m.seq, std::memory_order_relaxed);
        }
      });

  // Publish until the queue verdict lands, timing the worst single publish:
  // the matching thread must never be parked on the wedged socket.
  uint64_t worstNs = 0;
  OrderId id = 1;
  for (int i = 0; i < 20000 && counters.slowConsumerDisconnects.load() == 0; ++i)
  {
    const double price = 90.0 + static_cast<double>(i % 20) * 0.01;
    const auto t0 = std::chrono::steady_clock::now();
    eng.submit(limit(id++, Side::BUY, price, 1));
    const uint64_t ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    worstNs = ns > worstNs ? ns : worstNs;
    if (i % 8 == 7)
    {
      // Pace the flood so the test measures the POLICY, not this machine's
      // ability to outrun a single-threaded reader: the slow subscriber is
      // slow because it never reads, not because the feed is faster than a
      // socket. The healthy reader keeps up at any rate a real one would.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  CHECK(counters.slowConsumerDisconnects.load() >= 1);
  CHECK(worstNs < 500'000'000);  // no submit ever waited on the wedged socket

  // The healthy subscriber is untouched by its neighbour's fate and stays in
  // step with the reference consumer.
  const uint64_t target = md.seq();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (applied.load() < target && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  stopReader.store(true);
  reader.join();
  CHECK(applied.load() == target);
  CHECK(sameBook(healthyBook, refBook));

  // The dropped one is the slow one: whatever the kernel had already buffered
  // for it still reads out, and then the connection is simply gone.
  std::vector<uint8_t> f;
  bool slowClosed = false;
  for (int i = 0; i < 100000; ++i)
  {
    if (!slow.next(f))
    {
      slowClosed = true;
      break;
    }
  }
  CHECK(slowClosed);
  const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (dist.subscriberCount() > 1 && std::chrono::steady_clock::now() < settle)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(dist.subscriberCount() == 1);

  slow.close();
  healthy.close();
  dist.stop();
}

// A gap inside the retained ring is replayed on the same connection; a gap
// older than the ring is answered with the explicit "you need a snapshot"
// followed by the book itself.
void test_resend_and_snapshot_required()
{
  std::printf("test_resend_and_snapshot_required\n");
  MdCounters counters;
  ConsumerBook refBook;
  MdDistributionServer dist(&counters, testConfig());

  MarketDataPublisher<> md([&](const MdMessage& m)
                           {
                             refBook.apply(m);
                             dist.publish(m); },
                           px(0.01), SYM, /*epoch*/ 0, /*resendCapacity*/ 16);
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { md.onEvent(e, eng.engineTimeNs()); });
  dist.addPublisher(md);
  CHECK(dist.start(0) > 0);

  for (int i = 1; i <= 6; ++i)
  {
    eng.submit(limit(static_cast<OrderId>(i), Side::SELL, 100 + i, i));
  }

  SbeClient c;
  CHECK(c.open(dist.port()));
  CHECK(c.subscribe(SYM));
  ConsumerBook book;
  MdSnapshotBegin begin{};
  CHECK(readSnapshot(c, book, begin));
  const uint64_t joinedAt = begin.lastSeq;

  // A few more increments, all comfortably inside the 16-deep ring.
  for (int i = 7; i <= 10; ++i)
  {
    eng.submit(limit(static_cast<OrderId>(i), Side::SELL, 100 + i % 5 + 1, i));
  }
  const uint64_t head = md.seq();
  std::vector<uint8_t> f;
  uint64_t applied = joinedAt;
  while (applied < head && c.next(f))
  {
    MdMessage m;
    CHECK(SbeMdCodec::decode(f.data(), f.size(), m));
    applied = m.seq;
  }
  CHECK(applied == head);

  // Ask for a tail we already have: the ring serves it, byte for byte.
  const uint64_t from = head - 2;
  CHECK(c.resend(SYM, from));
  uint64_t seen = from - 1;
  for (uint64_t k = from; k <= head; ++k)
  {
    CHECK(c.next(f));
    MdMessage m;
    CHECK(SbeMdCodec::decode(f.data(), f.size(), m));
    CHECK(m.seq == seen + 1);
    seen = m.seq;
  }
  CHECK(seen == head);
  CHECK(counters.resendServed.load() >= 1);

  // Push the ring past seq 1, then ask for something that has been trimmed.
  for (int i = 11; i <= 40; ++i)
  {
    eng.submit(limit(static_cast<OrderId>(i), Side::BUY, 90 + i % 5, 1));
  }
  while (c.next(f))
  {
    MdMessage m;
    if (!SbeMdCodec::decode(f.data(), f.size(), m) || m.seq >= md.seq())
    {
      break;
    }
  }
  const uint64_t snapshotsBefore = counters.snapshotsServed.load();
  CHECK(c.resend(SYM, 1));
  ConsumerBook rebuilt;
  MdSnapshotBegin again{};
  CHECK(readSnapshot(c, rebuilt, again, /*expectRequired*/ true));
  CHECK(again.epoch == md.epoch());
  CHECK(counters.snapshotsServed.load() == snapshotsBefore + 1);

  // The rebuilt book plus whatever follows equals the reference.
  applied = again.lastSeq;
  const uint64_t target = md.seq();
  while (applied < target && c.next(f))
  {
    MdMessage m;
    if (!SbeMdCodec::decode(f.data(), f.size(), m))
    {
      continue;
    }
    if (m.seq <= applied)
    {
      continue;  // an in-flight duplicate from before the re-snapshot
    }
    rebuilt.apply(m);
    applied = m.seq;
  }
  CHECK(sameBook(rebuilt, refBook));

  // An unknown symbol is refused outright, with no subscription left behind.
  CHECK(c.subscribe(OTHER));
  CHECK(c.next(f));
  CHECK(c.tmpl(f) == SbeMdCodec::Tmpl::SubscribeReject);
  MdSubscribeReject rej{};
  CHECK(SbeMdCodec::decode(f.data(), f.size(), rej));
  CHECK(rej.symbol == OTHER);
  CHECK(rej.reason == MdSubscribeRejectReason::UnknownSymbol);
  CHECK(counters.subscribeRejects.load() == 1);

  c.close();
  dist.stop();
}

// A restarted publisher mints a new epoch and starts over at seq=1. The
// subscriber must SEE that -- not have the restarted stream filtered out as
// stale by a delivery position left over from the previous lifetime -- and
// re-snapshot.
void test_publisher_restart_epoch()
{
  std::printf("test_publisher_restart_epoch\n");
  MdCounters counters;
  MdDistributionServer dist(&counters, testConfig());
  CHECK(dist.start(0) > 0);

  ConsumerBook refBook;
  auto sink = [&](const MdMessage& m)
  {
    refBook.apply(m);
    dist.publish(m);
  };

  MarketDataPublisher<> first(sink, px(0.01), SYM, /*epoch*/ 1111);
  MatchingEngine<MatchingBook> eng1(cfg(), [&](const OutboundEvent& e)
                                    { first.onEvent(e, eng1.engineTimeNs()); });
  dist.addPublisher(first);
  for (int i = 1; i <= 8; ++i)
  {
    eng1.submit(limit(static_cast<OrderId>(i), Side::SELL, 100 + i, i));
  }

  SbeClient c;
  CHECK(c.open(dist.port()));
  CHECK(c.subscribe(SYM));
  ConsumerBook book;
  MdSnapshotBegin begin{};
  CHECK(readSnapshot(c, book, begin));
  CHECK(begin.epoch == 1111);

  std::vector<uint8_t> f;
  uint64_t applied = begin.lastSeq;
  const uint64_t head = first.seq();
  while (applied < head && c.next(f))
  {
    MdMessage m;
    CHECK(SbeMdCodec::decode(f.data(), f.size(), m));
    applied = m.seq;
  }

  // Restart: a fresh publisher for the same symbol, new epoch, seq back to 1.
  refBook.clear();
  MarketDataPublisher<> second(sink, px(0.01), SYM, /*epoch*/ 2222);
  MatchingEngine<MatchingBook> eng2(cfg(), [&](const OutboundEvent& e)
                                    { second.onEvent(e, eng2.engineTimeNs()); });
  dist.addPublisher(second);
  eng2.submit(limit(101, Side::BUY, 95, 4));

  CHECK(c.next(f));
  MdMessage m;
  CHECK(SbeMdCodec::decode(f.data(), f.size(), m));
  CHECK(m.epoch == 2222);  // the restarted stream is delivered, not swallowed
  CHECK(m.seq == 1);       // even though the old position was well past it

  // Seeing the epoch change, the consumer re-snapshots.
  CHECK(c.subscribe(SYM));
  ConsumerBook rebuilt;
  MdSnapshotBegin after{};
  CHECK(readSnapshot(c, rebuilt, after));
  CHECK(after.epoch == 2222);
  CHECK(after.lastSeq >= 1);

  eng2.submit(limit(102, Side::SELL, 120, 2));
  applied = after.lastSeq;
  const uint64_t target = second.seq();
  while (applied < target && c.next(f))
  {
    MdMessage inc;
    if (!SbeMdCodec::decode(f.data(), f.size(), inc) || inc.seq <= applied)
    {
      continue;
    }
    rebuilt.apply(inc);
    applied = inc.seq;
  }
  CHECK(applied == target);
  CHECK(sameBook(rebuilt, refBook));

  c.close();
  dist.stop();
}

// FIX round trip: Logon, MarketDataRequest, full refresh, incrementals,
// unsubscribe, and a reject for a symbol the venue does not publish. Prices
// and sizes print exactly, as they do on the order-entry side.
void test_fix_market_data()
{
  std::printf("test_fix_market_data\n");
  MdCounters counters;
  MdDistributionServer dist(&counters, testConfig());

  MarketDataPublisher<> md([&](const MdMessage& m)
                           { dist.publish(m); }, px(0.01), SYM);
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { md.onEvent(e, eng.engineTimeNs()); });
  dist.addPublisher(md);
  CHECK(dist.start(0) > 0);

  eng.submit(limit(1, Side::SELL, 100.25, 1.5));
  eng.submit(limit(2, Side::BUY, 99.75, 2));

  FixClient c;
  CHECK(c.open(dist.port()));
  CHECK(c.logon());
  const std::string logon = c.nextOf("A");
  CHECK(!logon.empty());
  CHECK(hasField(logon, "49=VENUE"));
  CHECK(hasField(logon, "56=CLIENT"));

  // 263=1: snapshot plus updates.
  CHECK(c.request("REQ-1", "1", {SYM}));
  const std::string refresh = c.nextOf("W");
  CHECK(!refresh.empty());
  CHECK(hasField(refresh, "262=REQ-1"));
  CHECK(hasField(refresh, "55=1"));
  CHECK(hasField(refresh, "268=2"));
  CHECK(hasField(refresh, "269=1"));       // the resting offer
  CHECK(hasField(refresh, "270=100.25"));  // exact decimal, never 100.250000
  CHECK(hasField(refresh, "271=1.5"));
  CHECK(hasField(refresh, "269=0"));  // the resting bid
  CHECK(hasField(refresh, "270=99.75"));
  CHECK(hasField(refresh, "278=1"));  // order-level entry ids

  // A new resting order arrives as an incremental with MDUpdateAction New.
  eng.submit(limit(3, Side::SELL, 101.5, 4.25));
  const std::string add = c.nextOf("X");
  CHECK(!add.empty());
  CHECK(hasField(add, "262=REQ-1"));
  CHECK(hasField(add, "268=1"));
  CHECK(hasField(add, "279=0"));  // New
  CHECK(hasField(add, "269=1"));  // Offer
  CHECK(hasField(add, "55=1"));
  CHECK(hasField(add, "270=101.5"));
  CHECK(hasField(add, "271=4.25"));
  CHECK(hasField(add, "278=3"));

  // A cancel arrives as MDUpdateAction Delete.
  eng.submit(CancelOrder{3, SYM, 1});
  const std::string del = c.nextOf("X");
  CHECK(!del.empty());
  CHECK(hasField(del, "279=2"));  // Delete
  CHECK(hasField(del, "278=3"));

  // A trade prints as an MDEntryType Trade entry.
  eng.submit(limit(4, Side::BUY, 100.25, 0.5, 2));
  bool sawTrade = false;
  for (int i = 0; i < 8 && !sawTrade; ++i)
  {
    const std::string x = c.nextOf("X");
    if (x.empty())
    {
      break;
    }
    sawTrade = hasField(x, "269=2") && hasField(x, "270=100.25");
  }
  CHECK(sawTrade);

  // 263=2 unsubscribes: the stream stops.
  CHECK(c.request("REQ-1", "2", {SYM}));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  while (!c.next().empty())
  {
  }
  eng.submit(limit(5, Side::BUY, 98, 1));
  eng.submit(limit(6, Side::BUY, 97, 1));
  CHECK(c.nextOf("X").empty());  // nothing further arrives

  // A symbol the venue does not publish is rejected, with a reason.
  CHECK(c.request("REQ-2", "1", {OTHER}));
  const std::string reject = c.nextOf("Y");
  CHECK(!reject.empty());
  CHECK(hasField(reject, "262=REQ-2"));
  CHECK(hasField(reject, "281=0"));  // unknown symbol

  // An unsupported request type is rejected before it reaches the feed.
  CHECK(c.request("REQ-3", "9", {SYM}));
  const std::string bad = c.nextOf("Y");
  CHECK(!bad.empty());
  CHECK(hasField(bad, "281=4"));  // unsupported SubscriptionRequestType

  // Partial depth is not something the feed can honestly serve.
  CHECK(c.request("REQ-4", "1", {SYM}, {"0", "1"}, "5"));
  const std::string depth = c.nextOf("Y");
  CHECK(!depth.empty());
  CHECK(hasField(depth, "281=5"));

  c.close();
  dist.stop();
}

// One server, one port, two encodings at the same time: the first byte of the
// connection picks the encoder and the distribution logic is the same for both.
void test_both_encodings_one_server()
{
  std::printf("test_both_encodings_one_server\n");
  MdCounters counters;
  MdDistributionServer dist(&counters, testConfig());

  MarketDataPublisher<> md([&](const MdMessage& m)
                           { dist.publish(m); }, px(0.01), SYM);
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { md.onEvent(e, eng.engineTimeNs()); });
  dist.addPublisher(md);
  CHECK(dist.start(0) > 0);

  eng.submit(limit(1, Side::SELL, 105, 3));

  SbeClient binary;
  FixClient text;
  CHECK(binary.open(dist.port()));
  CHECK(text.open(dist.port()));
  CHECK(binary.subscribe(SYM));
  CHECK(text.logon());
  CHECK(!text.nextOf("A").empty());
  CHECK(text.request("BOTH", "1", {SYM}));

  ConsumerBook book;
  MdSnapshotBegin begin{};
  CHECK(readSnapshot(binary, book, begin));
  CHECK(begin.orderCount == 1);
  const std::string refresh = text.nextOf("W");
  CHECK(!refresh.empty());
  CHECK(hasField(refresh, "268=1"));
  CHECK(hasField(refresh, "270=105"));

  CHECK(counters.subscribers.load() == 2);

  // The same increment reaches both, each in its own encoding.
  eng.submit(limit(2, Side::BUY, 95.5, 7));
  std::vector<uint8_t> f;
  CHECK(binary.next(f));
  MdMessage m;
  CHECK(SbeMdCodec::decode(f.data(), f.size(), m));
  CHECK(m.type == MdType::AddOrder);
  CHECK(m.id == 2);
  CHECK(m.price.raw() == px(95.5).raw());

  const std::string inc = text.nextOf("X");
  CHECK(!inc.empty());
  CHECK(hasField(inc, "278=2"));
  CHECK(hasField(inc, "270=95.5"));
  CHECK(hasField(inc, "271=7"));

  binary.close();
  text.close();
  dist.stop();
}

// A subscriber that goes silent is not kept forever: the liveness policy is
// the gateways' -- no inbound traffic within the idle window, connection gone.
void test_idle_subscriber_dropped()
{
  std::printf("test_idle_subscriber_dropped\n");
  MdCounters counters;
  MdDistributionConfig dcfg = testConfig();
  dcfg.idleTimeoutMs = 200;
  dcfg.heartbeatMs = 60000;
  MdDistributionServer dist(&counters, dcfg);

  MarketDataPublisher<> md([&](const MdMessage& m)
                           { dist.publish(m); }, px(0.01), SYM);
  dist.addPublisher(md);
  CHECK(dist.start(0) > 0);

  SbeClient c;
  CHECK(c.open(dist.port(), 1500));
  CHECK(c.subscribe(SYM));
  ConsumerBook book;
  MdSnapshotBegin begin{};
  CHECK(readSnapshot(c, book, begin));

  std::vector<uint8_t> f;
  CHECK(!c.next(f));  // the server closed: read returns EOF, not a frame
  CHECK(counters.idleDisconnects.load() >= 1);
  CHECK(dist.subscriberCount() == 0);

  c.close();
  dist.stop();
}

// Trading status and the derivatives layer travel over the unicast channel
// like any other message -- live, and inside the snapshot a late joiner gets.
// A subscriber that connects into a halted instrument must be told it is
// halted; one holding a perp must get a mark to value it with.
void test_status_and_derivatives_over_unicast()
{
  std::printf("test_status_and_derivatives_over_unicast\n");
  MdDistributionServer dist(nullptr, testConfig());

  MarketDataPublisher<> md([&](const MdMessage& m)
                           { dist.publish(m); }, px(0.01), SYM);
  SymbolConfig c = cfg();
  c.fundingIntervalNs = DurationNs{8'000'000'000LL};
  MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                   { md.onEvent(e, eng.engineTimeNs()); });
  dist.addPublisher(md);
  CHECK(dist.start(0) > 0);

  eng.submit(limit(1, Side::SELL, 100, 5), 1'000);
  eng.submit(InboundCommand{SetMark{SYM, px(100)}}, 2'000);
  eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Halt}}, 3'000);

  // Late joiner: the snapshot leads with the state, then the book.
  SbeClient joiner;
  CHECK(joiner.open(dist.port()));
  CHECK(joiner.subscribe(SYM));
  ConsumerBook book;
  MdSnapshotBegin begin{};
  SnapshotState state;
  CHECK(readSnapshot(joiner, book, begin, /*expectRequired*/ false, &state));
  CHECK(state.hasStatus);
  CHECK(state.status.status == TradingStatus::Halted);
  CHECK(state.status.engineTsNs == 3'000);
  CHECK(state.hasDerivatives);
  CHECK(state.derivatives.price == px(100));
  CHECK(state.derivatives.nextFundingNs == 8'000'000'000LL);
  CHECK(state.beforeFirstOrder == 2);  // both ahead of the book body
  CHECK(begin.orderCount == 1);

  // Live: the resume and a new mark reach the same subscriber as increments.
  eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Resume}}, 4'000);
  eng.submit(InboundCommand{SetMark{SYM, px(101)}}, 5'000);

  bool sawResume = false;
  bool sawMark = false;
  std::vector<uint8_t> f;
  while ((!sawResume || !sawMark) && joiner.next(f))
  {
    MdMessage m;
    CHECK(SbeMdCodec::decode(f.data(), f.size(), m));
    CHECK(m.engineTsNs != 0 && m.sendTsNs != 0);
    if (m.type == MdType::TradingStatus && m.status == TradingStatus::Trading)
    {
      sawResume = true;
    }
    if (m.type == MdType::DerivativesUpdate && m.price == px(101))
    {
      sawMark = true;
    }
  }
  CHECK(sawResume);
  CHECK(sawMark);

  joiner.close();
  dist.stop();
}

}  // namespace

TEST(MdDistribution, EngineSuite)
{
  test_status_and_derivatives_over_unicast();
  test_subscribe_midstream();
  test_slow_consumer_disconnected();
  test_resend_and_snapshot_required();
  test_publisher_restart_epoch();
  test_fix_market_data();
  test_both_encodings_one_server();
  test_idle_subscriber_dropped();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
