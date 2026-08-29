/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Market-data snapshot + recovery, end to end over real sockets: a late joiner
 * detects the gap, fetches a snapshot through MdRecoveryClient (the consumer
 * round-trip: request -> classified reply -> GapDetector reset) and converges
 * on the reference book; a publisher restart surfaces as an epoch change and
 * the consumer re-snapshots instead of stalling; the resend ring replays a
 * tail; sendto failures are counted, never blocking.
 */
#include "flox-venue/market_data.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/md_recovery.h"
#include "flox-venue/metrics.h"
#include "flox-venue/resend_buffer.h"
#include "flox-venue/udp_multicast.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
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

// Minimal MD consumer book: per-order state driven by the public feed, folded
// to (side, price) -> aggregate qty for comparisons.
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
        break;  // no direct depth impact (depth moves via Executed)
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

// All recovery round-trips below go through MdRecoveryClient -- the test IS
// the documentation of the consumer protocol the client packages.
MdRecoveryClient client(int port)
{
  return MdRecoveryClient("127.0.0.1", static_cast<uint16_t>(port));
}

// Probe whether same-host multicast works (CI containers often block it);
// fall back to loopback unicast exactly like the existing UDP MD test.
bool probeMulticast()
{
  const char* group = "239.7.7.9";
  const char* iface = "127.0.0.1";
  UdpMdSubscriber sub;
  if (!sub.join(group, 0, true, iface))
  {
    return false;
  }
  sub.setTimeout(200);
  UdpMdPublisher pub;
  if (!pub.open(group, static_cast<uint16_t>(sub.port()), true, iface))
  {
    return false;
  }
  pub.publish(MdMessage{MdType::Trade, 1, SYM, 1, Side::BUY, px(100), qty(1), 0, 1});
  MdMessage m;
  return sub.recv(m);
}

bool g_multicast = false;

bool setupUdp(UdpMdPublisher& pub, UdpMdSubscriber& sub, const char* group)
{
  const char* iface = "127.0.0.1";
  if (!sub.join(group, 0, g_multicast, iface))
  {
    return false;
  }
  sub.setTimeout(300);
  return pub.open(g_multicast ? group : "127.0.0.1", static_cast<uint16_t>(sub.port()),
                  g_multicast, iface);
}

// Late joiner: misses the start of the stream. RecoveringMdSubscriber (the
// packaged consumer protocol: gap accounting + TCP round-trip + sequencer
// fast-forward) detects the gap, fetches the snapshot (fromSeq older than the
// ring -> SnapshotRequired path), hands the body to the onSnapshot callback,
// resumes the incremental feed and converges on the reference consumer's
// book. The low-level protocol cases stay in test_resend_served -- this test
// covers the composed consumer.
void test_late_joiner_snapshot()
{
  std::printf("test_late_joiner_snapshot\n");
  MdCounters counters;
  ConsumerBook refBook;  // reference consumer: sees the stream from seq 1
  UdpMdPublisher udpPub;
  UdpMdSubscriber sub;
  CHECK(setupUdp(udpPub, sub, "239.7.8.1"));
  udpPub.setCounters(&counters);

  // Small ring so a from-genesis resend is impossible: the late joiner MUST be
  // routed through the snapshot path.
  MarketDataPublisher<> md([&](const MdMessage& m)
                           {
                             refBook.apply(m);
                             udpPub.publish(m); },
                           px(0.01), SYM, /*epoch*/ 0, /*resendCapacity*/ 8);
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { md.onEvent(e, eng.engineTimeNs()); });
  MdRecoveryServer rec(&counters);
  rec.addPublisher(md);
  CHECK(rec.start(0) > 0);

  // Phase A (missed by the late joiner): resting depth + a partial fill.
  for (int i = 1; i <= 5; ++i)
  {
    eng.submit(limit(static_cast<OrderId>(i), Side::SELL, 100 + i, i));
  }
  for (int i = 1; i <= 5; ++i)
  {
    eng.submit(limit(static_cast<OrderId>(5 + i), Side::BUY, 100 - i, i));
  }
  eng.submit(limit(11, Side::BUY, 101, 0.5, 2));  // trades 0.5 against id1
  CHECK(md.seq() > 8);                            // more than the ring retains

  // The late joiner was not listening: drop everything received so far.
  {
    MdMessage m;
    while (sub.recv(m))
    {
    }
  }

  // Attach the recovering consumer over the raw subscriber.
  RecoveringMdSubscriber::Config rc;
  rc.host = "127.0.0.1";
  rc.port = static_cast<uint16_t>(rec.port());
  RecoveringMdSubscriber rsub(sub, rc);
  ConsumerBook lateBook;
  uint64_t snapshotLastSeq = 0;
  rsub.onSnapshot([&](SymbolId sym, const MdSnapshotBegin& b, const std::vector<MdMessage>& orders)
                  {
                    CHECK(sym == SYM);
                    CHECK(b.epoch == md.epoch());
                    CHECK(b.orderCount == orders.size());
                    snapshotLastSeq = b.lastSeq;
                    lateBook.clear();
                    for (const auto& m : orders)
                    {
                      CHECK(m.type == MdType::AddOrder);
                      CHECK(m.epoch == b.epoch);
                      lateBook.apply(m);
                    } });

  // Phase B: live traffic the late joiner receives but cannot deliver until
  // the snapshot recovery has run (everything is ahead of the missing prefix).
  eng.submit(CancelOrder{2, SYM, 1});
  eng.submit(ModifyOrder{3, SYM, px(106), qty(2), 1});
  eng.submit(limit(12, Side::SELL, 107, 3));
  const uint64_t targetB = md.seq();

  // Position = the later of the snapshot resume point and the last delivered
  // increment (the snapshot may already cover everything published so far).
  uint64_t appliedSeq = 0;
  const auto position = [&]
  { return appliedSeq > snapshotLastSeq ? appliedSeq : snapshotLastSeq; };
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (position() < targetB && std::chrono::steady_clock::now() < deadline)
    {
      MdMessage m;
      if (!rsub.recv(m))
      {
        continue;
      }
      CHECK(m.seq == appliedSeq + 1 || appliedSeq == 0);  // strictly in order
      lateBook.apply(m);
      appliedSeq = m.seq;
    }
  }
  CHECK(rsub.gapsDetected() >= 1);        // the missing prefix surfaced as a gap
  CHECK(rsub.snapshotsRecovered() == 1);  // resolved through the snapshot path
  CHECK(snapshotLastSeq >= 8);            // the snapshot really covered the prefix
  CHECK(counters.snapshotsServed.load() == 1);
  CHECK(position() == targetB);
  CHECK(rsub.expected(SYM) == targetB + 1);
  CHECK(sameBook(lateBook, refBook));  // snapshot + resumed increments == reference

  // Phase C: the incremental feed keeps both books in step with no further
  // recovery round-trips.
  eng.submit(limit(13, Side::BUY, 94, 2));
  eng.submit(CancelOrder{6, SYM, 1});
  eng.submit(limit(14, Side::BUY, 103, 10, 2));  // sweeps the 101 remainder, rests at 103
  const uint64_t target = md.seq();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (position() < target && std::chrono::steady_clock::now() < deadline)
  {
    MdMessage m;
    if (!rsub.recv(m))
    {
      continue;
    }
    lateBook.apply(m);
    appliedSeq = m.seq;
  }
  CHECK(position() == target);
  CHECK(appliedSeq == target);            // phase C really flowed as increments
  CHECK(rsub.snapshotsRecovered() == 1);  // no extra recovery was needed
  CHECK(sameBook(lateBook, refBook));
  rec.stop();
}

// Resend ring: a gap inside the retained tail is replayed byte-for-byte; a
// fromSeq past the head is an empty replay, not a snapshot.
void test_resend_served()
{
  std::printf("test_resend_served\n");
  MdCounters counters;
  std::vector<MdMessage> sent;
  MarketDataPublisher<> md([&](const MdMessage& m)
                           { sent.push_back(m); }, px(0.01), SYM);
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { md.onEvent(e, eng.engineTimeNs()); });
  MdRecoveryServer rec(&counters);
  rec.addPublisher(md);
  CHECK(rec.start(0) > 0);

  for (int i = 1; i <= 10; ++i)
  {
    eng.submit(limit(static_cast<OrderId>(i), i % 2 ? Side::SELL : Side::BUY,
                     i % 2 ? 100 + i : 100 - i, i));
  }
  const uint64_t last = md.seq();
  CHECK(last == sent.size());

  MdRecoveryClient::Result mid;
  CHECK(client(rec.port()).recover(SYM, 5, mid) == MdRecoveryClient::Status::Ok);
  CHECK(!mid.snapshot);
  CHECK(mid.messages.size() == last - 4);
  for (size_t i = 0; i < mid.messages.size(); ++i)
  {
    CHECK(mid.messages[i].seq == 5 + i);
    CHECK(mid.messages[i].epoch == md.epoch());
    CHECK(mid.messages[i].id == sent[4 + i].id);
    CHECK(mid.messages[i].qty == sent[4 + i].qty);
  }
  CHECK(mid.lastSeq == last && mid.epoch == md.epoch());
  CHECK(counters.resendServed.load() == 1);

  MdRecoveryClient::Result all;
  CHECK(client(rec.port()).recover(SYM, 1, all) == MdRecoveryClient::Status::Ok);
  CHECK(!all.snapshot && all.messages.size() == last);

  MdRecoveryClient::Result ahead;  // nothing published past `last` yet
  CHECK(client(rec.port()).recover(SYM, last + 1, ahead) == MdRecoveryClient::Status::Ok);
  CHECK(!ahead.snapshot && ahead.messages.empty());
  CHECK(ahead.lastSeq == last);  // empty replay keeps the consumer's position
  CHECK(counters.snapshotsServed.load() == 0);

  // A trimmed-out fromSeq flips to the snapshot path (small ring).
  MdCounters counters2;
  MarketDataPublisher<> md2([](const MdMessage&) {}, px(0.01), SYM, 0, /*resendCapacity*/ 4);
  MatchingEngine<MatchingBook> eng2(cfg(), [&](const OutboundEvent& e)
                                    { md2.onEvent(e, eng2.engineTimeNs()); });
  MdRecoveryServer rec2(&counters2);
  rec2.addPublisher(md2);
  CHECK(rec2.start(0) > 0);
  for (int i = 1; i <= 10; ++i)
  {
    eng2.submit(limit(static_cast<OrderId>(i), Side::SELL, 100 + i, i));
  }
  MdRecoveryClient::Result old;
  CHECK(client(rec2.port()).recover(SYM, 2, old) == MdRecoveryClient::Status::Ok);
  CHECK(old.snapshot);
  CHECK(old.messages.size() == 10);  // all ten still resting
  CHECK(counters2.snapshotsServed.load() == 1 && counters2.resendServed.load() == 0);

  // A dead server is an honest ConnectError, not an exception or a hang.
  const int deadPort = rec2.port();
  rec2.stop();
  MdRecoveryClient::Result none;
  CHECK(client(deadPort).recover(SYM, 1, none) == MdRecoveryClient::Status::ConnectError);
  rec.stop();
}

// Publisher restart: the surviving RecoveringMdSubscriber sees the epoch
// change, drops its stale book through the onSnapshot callback (fromSeq=0
// snapshot recovery) and keeps consuming -- never a silent stall where every
// message of the restarted feed counts as a duplicate.
void test_publisher_restart_epoch()
{
  std::printf("test_publisher_restart_epoch\n");
  MdCounters counters;
  UdpMdPublisher udpPub;
  UdpMdSubscriber sub;
  CHECK(setupUdp(udpPub, sub, "239.7.8.2"));
  MdRecoveryServer rec(&counters);
  CHECK(rec.start(0) > 0);

  ConsumerBook consumer;
  ConsumerBook ref2;  // reference for the second publisher lifetime

  RecoveringMdSubscriber::Config rc;
  rc.host = "127.0.0.1";
  rc.port = static_cast<uint16_t>(rec.port());
  RecoveringMdSubscriber rsub(sub, rc);
  uint64_t snapshotEpoch = 0;
  rsub.onSnapshot([&](SymbolId sym, const MdSnapshotBegin& b, const std::vector<MdMessage>& orders)
                  {
                    CHECK(sym == SYM);
                    snapshotEpoch = b.epoch;
                    consumer.clear();  // the pre-restart book is void
                    for (const auto& m : orders)
                    {
                      consumer.apply(m);
                    } });

  auto md1 = std::make_unique<MarketDataPublisher<>>([&](const MdMessage& m)
                                                     { udpPub.publish(m); }, px(0.01), SYM);
  rec.addPublisher(*md1);
  const uint64_t epoch1 = md1->epoch();
  {
    MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                     { md1->onEvent(e, eng.engineTimeNs()); });
    eng.submit(limit(1, Side::SELL, 101, 1));
    eng.submit(limit(2, Side::BUY, 99, 2));
    const uint64_t t1 = md1->seq();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    uint64_t applied = 0;
    while (applied < t1 && std::chrono::steady_clock::now() < deadline)
    {
      MdMessage m;
      if (rsub.recv(m))
      {
        consumer.apply(m);
        applied = m.seq;
      }
    }
    CHECK(applied == t1);
    CHECK(rsub.epochChanges() == 0);  // first lifetime: nothing to recover
  }

  // Restart: a fresh publisher for the same symbol -- new epoch, seq back to 1.
  auto md2 = std::make_unique<MarketDataPublisher<>>([&](const MdMessage& m)
                                                     {
                                                       ref2.apply(m);
                                                       udpPub.publish(m); },
                                                     px(0.01), SYM);
  rec.addPublisher(*md2);  // replaces the source for SYM
  md1.reset();
  CHECK(md2->epoch() != epoch1);
  MatchingEngine<MatchingBook> eng2(cfg(), [&](const OutboundEvent& e)
                                    { md2->onEvent(e, eng2.engineTimeNs()); });
  eng2.submit(limit(1, Side::SELL, 105, 3));  // ids restart too: a different book
  eng2.submit(limit(2, Side::BUY, 95, 4));
  eng2.submit(limit(3, Side::SELL, 106, 1));
  const uint64_t t2 = md2->seq();

  // The wrapper handles the whole restart protocol inside recv(): epoch
  // change -> snapshot recovery -> fast-forward -> continue the new stream.
  // The snapshot may already cover everything published in the new lifetime,
  // so the position is the later of the resume point and delivered seqs.
  uint64_t applied = 0;
  uint64_t snapLastSeq = 0;
  rsub.onSnapshot([&](SymbolId sym, const MdSnapshotBegin& b, const std::vector<MdMessage>& orders)
                  {
                    CHECK(sym == SYM);
                    snapshotEpoch = b.epoch;
                    snapLastSeq = b.lastSeq;
                    consumer.clear();  // the pre-restart book is void
                    for (const auto& m : orders)
                    {
                      consumer.apply(m);
                    } });
  const auto position = [&]
  { return applied > snapLastSeq ? applied : snapLastSeq; };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (position() < t2 && std::chrono::steady_clock::now() < deadline)
  {
    MdMessage m;
    if (!rsub.recv(m))
    {
      continue;
    }
    CHECK(m.epoch == md2->epoch());  // stale-epoch datagrams never surface
    consumer.apply(m);
    applied = m.seq;
  }
  CHECK(rsub.epochChanges() == 1);
  CHECK(rsub.snapshotsRecovered() >= 1);
  CHECK(snapshotEpoch == md2->epoch());
  CHECK(position() == t2);  // the feed kept flowing after the restart
  CHECK(rsub.expected(SYM) == t2 + 1);
  CHECK(sameBook(consumer, ref2));
  CHECK(counters.snapshotsServed.load() >= 1);
  rec.stop();
}

// Recovery-channel failure: attempts are bounded with doubling backoff, the
// failure is counted, recv() degrades to a timeout instead of hanging -- and
// once the resend server is back, the SAME gap (re-raised by the detector)
// recovers through the ring replay path.
void test_recovery_backoff_bounded()
{
  std::printf("test_recovery_backoff_bounded\n");
  UdpMdPublisher pub;
  UdpMdSubscriber sub;
  CHECK(setupUdp(pub, sub, "239.7.8.4"));
  sub.setTimeout(100);

  MdCounters counters;
  // Lossy wire: seq 2 never reaches the subscriber (it stays on the resend
  // ring) -- the manufactured gap this test recovers.
  MarketDataPublisher<> md([&](const MdMessage& m)
                           {
                             if (m.seq != 2)
                             {
                               pub.publish(m);
                             } },
                           px(0.01), SYM, /*epoch*/ 7);
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { md.onEvent(e, eng.engineTimeNs()); });
  MdRecoveryServer rec(&counters);
  rec.addPublisher(md);
  const int port = rec.start(0);
  CHECK(port > 0);
  rec.stop();  // server down: every recovery round-trip is a ConnectError

  RecoveringMdSubscriber::Config rc;
  rc.host = "127.0.0.1";
  rc.port = static_cast<uint16_t>(port);
  rc.maxAttempts = 2;
  rc.initialBackoff = std::chrono::milliseconds(5);
  // reraiseAfter=2: the still-open gap re-raises quickly once traffic flows.
  RecoveringMdSubscriber rsub(sub, rc, GapDetector::Config{2, 1024});

  eng.submit(limit(1, Side::SELL, 101, 1));  // seq 1: delivered normally
  {
    MdMessage m;
    CHECK(rsub.recv(m));
    CHECK(m.seq == 1);
  }
  eng.submit(limit(2, Side::BUY, 99, 1));    // seq 2: dropped by the lossy wire
  eng.submit(limit(3, Side::SELL, 102, 1));  // seq 3: opens the gap at 2
  // recv: gap at 2 -> recovery attempts (server down) -> bounded failure ->
  // timeout, not a hang.
  {
    MdMessage m;
    const bool got = rsub.recv(m);
    CHECK(!got);  // nothing deliverable, recovery failed, clean timeout
  }
  CHECK(rsub.gapsDetected() >= 1);
  CHECK(rsub.recoveriesFailed() == 1);
  CHECK(rsub.resendsRecovered() == 0);

  // Server returns; further traffic re-raises the gap; the ring replays 2..
  CHECK(rec.start(static_cast<uint16_t>(port)) == port);
  eng.submit(limit(4, Side::BUY, 98, 1));    // seq 4
  eng.submit(limit(5, Side::SELL, 103, 1));  // seq 5 (re-raise threshold)
  std::vector<uint64_t> seqs;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (seqs.size() < 4 && std::chrono::steady_clock::now() < deadline)
  {
    MdMessage m;
    if (rsub.recv(m))
    {
      seqs.push_back(m.seq);
    }
  }
  CHECK(seqs.size() == 4);  // 2, 3, 4, 5 -- the hole filled, strictly in order
  for (size_t i = 0; i < seqs.size(); ++i)
  {
    CHECK(seqs[i] == 2 + i);
  }
  CHECK(rsub.resendsRecovered() >= 1);
  rec.stop();
}

// MdRecoveryServer beyond loopback: an explicit bind address ("0.0.0.0")
// exposes the channel on all interfaces (hardening contract documented in
// market-data.md: TLS termination / firewall in front); the loopback client
// still reaches it, and the default remains loopback-only.
void test_recovery_server_bind_address()
{
  std::printf("test_recovery_server_bind_address\n");
  MdCounters counters;
  MarketDataPublisher<> md([](const MdMessage&) {}, px(0.01), SYM);
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { md.onEvent(e, eng.engineTimeNs()); });
  for (int i = 1; i <= 3; ++i)
  {
    eng.submit(limit(static_cast<OrderId>(i), Side::SELL, 100 + i, i));
  }

  MdRecoveryServer rec(&counters);
  rec.addPublisher(md);
  const int port = rec.start(0, "0.0.0.0");  // all interfaces (safe in CI: connect via loopback)
  CHECK(port > 0);
  MdRecoveryClient::Result all;
  CHECK(client(port).recover(SYM, 1, all) == MdRecoveryClient::Status::Ok);
  CHECK(!all.snapshot && all.messages.size() == 3);
  rec.stop();

  // A garbage bind address fails loudly instead of silently binding loopback.
  MdRecoveryServer bad(&counters);
  bad.addPublisher(md);
  CHECK(bad.start(0, "not-an-address") == -1);
}

// sendto failures are counted as drops and never block or throw.
void test_send_drop_counted()
{
  std::printf("test_send_drop_counted\n");
  MdCounters counters;
  UdpMdPublisher pub;
  pub.setCounters(&counters);
  // Closed socket: the datagram cannot leave -- a counted drop, not a wait.
  const MdMessage m{MdType::Trade, 1, SYM, 1, Side::BUY, px(100), qty(1), 0, 1};
  CHECK(!pub.publish(m));
  CHECK(counters.sendDrops.load() == 1);

  // A healthy socket publishes without touching the counter.
  UdpMdSubscriber sub;
  CHECK(sub.join("239.7.8.3", 0, false, "127.0.0.1"));
  sub.setTimeout(300);
  CHECK(pub.open("127.0.0.1", static_cast<uint16_t>(sub.port()), false));
  CHECK(pub.publish(m));
  CHECK(counters.sendDrops.load() == 1);
  MdMessage got;
  CHECK(sub.recv(got));
  CHECK(got.seq == 1 && got.epoch == 1);
}

// The new message types are ordinary feed messages: they cross the datagram
// transport intact, and the recovery channel's snapshot carries the CURRENT
// status and derivatives so a consumer that reconnects into a halted
// instrument is not left reading an idle book.
void test_status_and_derivatives_over_multicast_and_recovery()
{
  std::printf("test_status_and_derivatives_over_multicast_and_recovery\n");
  MdCounters counters;
  UdpMdPublisher udpPub;
  UdpMdSubscriber sub;
  CHECK(setupUdp(udpPub, sub, "239.7.8.4"));

  MarketDataPublisher<> md([&](const MdMessage& m)
                           { udpPub.publish(m); }, px(0.01), SYM);
  SymbolConfig c = cfg();
  c.fundingIntervalNs = DurationNs{8'000'000'000LL};
  MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                   { md.onEvent(e, eng.engineTimeNs()); });
  MdRecoveryServer rec(&counters);
  rec.addPublisher(md);
  CHECK(rec.start(0) > 0);

  eng.submit(limit(1, Side::SELL, 100, 5), 1'000);
  eng.submit(InboundCommand{SetMark{SYM, px(100)}}, 2'000);
  eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Halt}}, 3'000);

  // Multicast: both new types arrive decoded, with both timestamps.
  bool sawStatus = false;
  bool sawDerivatives = false;
  MdMessage m;
  while (sub.recv(m))
  {
    CHECK(m.engineTsNs != 0 && m.sendTsNs != 0);
    if (m.type == MdType::TradingStatus)
    {
      sawStatus = m.status == TradingStatus::Halted && m.engineTsNs == 3'000;
    }
    if (m.type == MdType::DerivativesUpdate)
    {
      sawDerivatives = m.price == px(100) && m.nextFundingNs == 8'000'000'000LL;
    }
  }
  CHECK(sawStatus);
  CHECK(sawDerivatives);

  // Recovery channel: a late joiner (fromSeq = 0) gets the state alongside the
  // book, kept out of the order body.
  MdRecoveryClient::Result res;
  CHECK(client(rec.port()).recover(SYM, 0, res) == MdRecoveryClient::Status::Ok);
  CHECK(res.snapshot);
  CHECK(res.hasStatus && res.status.status == TradingStatus::Halted);
  CHECK(res.hasDerivatives && res.derivatives.price == px(100));
  CHECK(res.messages.size() == res.begin.orderCount);  // body is orders only

  // A RESEND is a different case: there they are sequenced increments and must
  // stay in the stream, or the replay would have a hole in it.
  MdRecoveryClient::Result replay;
  CHECK(client(rec.port()).recover(SYM, 1, replay) == MdRecoveryClient::Status::Ok);
  CHECK(!replay.snapshot);
  CHECK(replay.messages.size() == md.seq());
  CHECK(replay.lastSeq == md.seq());
  bool statusInStream = false;
  for (const MdMessage& r : replay.messages)
  {
    statusInStream = statusInStream || r.type == MdType::TradingStatus;
  }
  CHECK(statusInStream);
}

}  // namespace

TEST(MdRecovery, EngineSuite)
{
  g_multicast = probeMulticast();
  std::printf("  UDP path: %s\n", g_multicast ? "multicast" : "unicast fallback");
  test_late_joiner_snapshot();
  test_resend_served();
  test_publisher_restart_epoch();
  test_recovery_backoff_bounded();
  test_recovery_server_bind_address();
  test_send_drop_counted();
  test_status_and_derivatives_over_multicast_and_recovery();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
