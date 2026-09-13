/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// A frame that arrives in pieces is still one frame.
//
// The read loop wakes on a receive timeout to service session timers. If the
// bytes already taken out of the kernel buffer are dropped on the way out, the
// stream is offset by exactly those bytes: the next length prefix is read from
// the middle of a message, and from then on the decoder is handed frames the
// peer never sent. TCP fragmentation with a pause longer than the timer tick is
// enough -- a loaded network, a retransmit, a slow sender.
//
// The TLS and WebSocket gateways both keep their read state across a timeout.
// This pins the same property for the plain framed gateway.

#include "flox-venue/fix_codec.h"
#include "flox-venue/fix_session.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/session_registry.h"
#include "flox-venue/tcp_gateway.h"
#include "flox/util/transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 1;
using Fields = std::unordered_map<int, std::string>;

Price px(double v) { return Price::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}

int64_t nowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// One FIX gateway in front of one engine -- the smallest arrangement that
// exercises the gateway's own read loop.
struct FixVenue
{
  GatewayCounters counters;
  SessionRegistry registry;
  std::mutex m;
  MatchingEngine<MatchingBook> eng;
  FixSessionHost fixHost;

  FixVenue()
      : eng(cfg(), [this](const OutboundEvent& e)
            { registry.route(e); }),
        fixHost(fixCfg())
  {
  }

  static FixSessionConfig fixCfg()
  {
    FixSessionConfig c;
    c.senderCompId = "VENUE";
    c.targetCompId = "CLIENT";
    return c;
  }

  static SessionRegistry::Encoder encoder(const FixSessionConfig& fc)
  {
    return [fc](const OutboundEvent& e, uint64_t seq, int64_t tsNs, std::vector<uint8_t>& out)
    {
      const std::string msg = FixCodec::encode(e, seq, fc.senderCompId, fc.targetCompId,
                                               FixSession::sendingTime(tsNs));
      if (msg.empty())
      {
        return false;
      }
      out.assign(msg.begin(), msg.end());
      return true;
    };
  }

  std::unique_ptr<TcpGateway> gateway(uint64_t account)
  {
    auto gw = std::make_unique<TcpGateway>(
        [](const uint8_t* p, size_t n)
        { return FixCodec::decode(std::string(reinterpret_cast<const char*>(p), n)); },
        account);
    gw->setDelivery(&registry, encoder(fixHost.config()));
    gw->setFixSession(&fixHost);
    gw->setCounters(&counters);
    return gw;
  }

  TcpGateway::Handler handler()
  {
    return [this](const InboundCommand& c, const TcpGateway::Responder&, int64_t)
    {
      std::lock_guard<std::mutex> lk(m);
      eng.submit(c);
    };
  }
};

struct Client
{
  int fd{-1};
  uint64_t seq{1};

  bool connectTo(int port, int rcvTimeoutSec = 4)
  {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
      return false;
    }
    suppressSigpipe(fd);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
    {
      return false;
    }
    timeval tv{rcvTimeoutSec, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return true;
  }

  void send(const std::string& m)
  {
    net::writeFrame(fd, reinterpret_cast<const uint8_t*>(m.data()), m.size());
  }

  // The same bytes, handed over in two pieces with a pause in between. `head`
  // counts from the start of the length prefix, so head < 4 splits the prefix
  // itself and head > 4 splits the body.
  void sendSplit(const std::string& m, size_t head, std::chrono::milliseconds pause)
  {
    std::vector<uint8_t> framed;
    const uint32_t n = static_cast<uint32_t>(m.size());
    framed.push_back(static_cast<uint8_t>(n >> 24));
    framed.push_back(static_cast<uint8_t>(n >> 16));
    framed.push_back(static_cast<uint8_t>(n >> 8));
    framed.push_back(static_cast<uint8_t>(n));
    framed.insert(framed.end(), m.begin(), m.end());

    net::writeAll(fd, framed.data(), head);
    std::this_thread::sleep_for(pause);
    net::writeAll(fd, framed.data() + head, framed.size() - head);
  }

  void admin(const std::string& type, const std::vector<std::pair<int, std::string>>& fields = {})
  {
    send(FixCodec::encodeAdmin(type, seq++, "CLIENT", "VENUE", FixSession::sendingTime(nowNs()),
                               fields, false));
  }

  std::string orderMessage(uint64_t id, const char* side, const char* price, const char* quantity)
  {
    std::string b;
    auto add = [&b](int t, const std::string& v)
    { b += std::to_string(t) + "=" + v + std::string(1, FixCodec::SOH); };
    add(35, "D");
    add(34, std::to_string(seq++));
    add(49, "CLIENT");
    add(56, "VENUE");
    add(52, FixSession::sendingTime(nowNs()));
    add(11, std::to_string(id));
    add(55, "1");
    add(54, side);
    add(38, quantity);
    add(44, price);
    add(40, "2");
    return FixCodec::frame(b);
  }

  bool read(Fields& f)
  {
    std::vector<uint8_t> frame;
    if (!net::readFrame(fd, frame))
    {
      return false;
    }
    f = FixCodec::parseFields(std::string(frame.begin(), frame.end()));
    return true;
  }

  bool readType(const std::string& type, Fields& f)
  {
    while (read(f))
    {
      if (f.count(35) != 0 && f[35] == type)
      {
        return true;
      }
    }
    return false;
  }

  ~Client()
  {
    if (fd >= 0)
    {
      ::close(fd);
    }
  }
};

}  // namespace

TEST(VenueFrameReassembly, LengthPrefixSplitAcrossATimerTickStillDecodes)
{
  FixVenue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  ASSERT_GT(port, 0);

  Client c;
  ASSERT_TRUE(c.connectTo(port));
  c.admin("A", {{108, "1"}, {141, "Y"}});
  Fields f;
  ASSERT_TRUE(c.readType("A", f));

  // Three of the four prefix bytes, then a pause longer than the FIX timer
  // tick (250 ms), then the rest.
  c.sendSplit(c.orderMessage(1, "1", "100.00", "5"), /*head*/ 3, std::chrono::milliseconds(400));

  ASSERT_TRUE(c.readType("8", f)) << "the order never reached the engine: the session either "
                                     "dropped or resynchronized onto the wrong byte";
  EXPECT_EQ(f[37], "1");

  gw->stop();
}

TEST(VenueFrameReassembly, BodySplitAcrossATimerTickIsNotReinterpreted)
{
  FixVenue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  ASSERT_GT(port, 0);

  Client c;
  ASSERT_TRUE(c.connectTo(port));
  c.admin("A", {{108, "1"}, {141, "Y"}});
  Fields f;
  ASSERT_TRUE(c.readType("A", f));

  // The split lands inside the message body this time. The bytes after it must
  // be read as the rest of that message, never as a fresh length prefix.
  const std::string msg = c.orderMessage(2, "1", "101.00", "7");
  c.sendSplit(msg, /*head*/ 4 + 9, std::chrono::milliseconds(400));

  ASSERT_TRUE(c.readType("8", f)) << "the body's tail was reinterpreted as a new frame";
  EXPECT_EQ(f[37], "2");

  // And the session is still usable afterwards.
  c.send(c.orderMessage(3, "1", "102.00", "3"));
  ASSERT_TRUE(c.readType("8", f));
  EXPECT_EQ(f[37], "3");

  gw->stop();
}
