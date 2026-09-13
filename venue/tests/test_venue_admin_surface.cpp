/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// What the admin and session surfaces must refuse.
//
// Two perimeters meet here. A gateway session speaks for one authenticated
// account and may not be talked into acting for another. The control plane
// takes operator input and must never turn a request it did not understand
// into a number nobody typed, or let one connection spend the venue's memory.

#include "flox-venue/control_api.h"
#include "flox-venue/control_plane.h"
#include "flox-venue/control_server.h"
#include "flox-venue/session.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr uint64_t kSessionAccount = 1001;
constexpr uint64_t kVictimAccount = 2002;

Price px(double v) { return Price::fromDouble(v); }

// A session whose decoder always yields `cmd`, authenticated as kSessionAccount.
// The payload the client wrote names the victim's account throughout.
InboundCommand throughSession(const InboundCommand& cmd)
{
  GatewaySession s(kSessionAccount, [cmd](const uint8_t*, size_t)
                   { return std::optional<InboundCommand>{cmd}; });
  s.authenticate(true);
  SessionReject rej{};
  const uint8_t frame[1] = {0};
  auto out = s.handle(frame, sizeof frame, 0, rej);
  EXPECT_TRUE(out.has_value()) << "session rejected the frame: " << toString(rej);
  return out.value_or(cmd);
}

bool containsText(const std::string& s, const char* sub)
{
  return s.find(sub) != std::string::npos;
}

int connectLoopback(int port, int rcvTimeoutSec)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
  {
    return -1;
  }
  suppressSigpipe(fd);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
  {
    ::close(fd);
    return -1;
  }
  timeval tv{};
  tv.tv_sec = rcvTimeoutSec;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return fd;
}

}  // namespace

// Every command that names an account must be forced onto the session's own
// account. The six order-flow commands were; the funding, position and
// per-account configuration commands were not, so a session that could reach
// them spoke for whichever account the payload named.
TEST(VenueAdminSurface, SessionStampsEveryAccountBearingCommand)
{
  {
    Withdraw w;
    w.accountId = kVictimAccount;
    w.amountRaw = 750000;
    const auto out = throughSession(InboundCommand{w});
    EXPECT_EQ(std::get<Withdraw>(out).accountId, kSessionAccount);
  }
  {
    Deposit d;
    d.accountId = kVictimAccount;
    const auto out = throughSession(InboundCommand{d});
    EXPECT_EQ(std::get<Deposit>(out).accountId, kSessionAccount);
  }
  {
    ForceClosePosition f;
    f.accountId = kVictimAccount;
    const auto out = throughSession(InboundCommand{f});
    EXPECT_EQ(std::get<ForceClosePosition>(out).accountId, kSessionAccount);
  }
  {
    SetStpGroup g;
    g.account = kVictimAccount;
    g.group = 77;
    const auto out = throughSession(InboundCommand{g});
    EXPECT_EQ(std::get<SetStpGroup>(out).account, kSessionAccount)
        << "otherwise a session drags another account into its own firm group";
  }
  {
    SetAdmissionProfile p;
    p.account = kVictimAccount;
    p.profile.deny = AdmissionDeny::DenyCancel | AdmissionDeny::DenyAmend;
    const auto out = throughSession(InboundCommand{p});
    EXPECT_EQ(std::get<SetAdmissionProfile>(out).account, kSessionAccount)
        << "otherwise a session can deny a competitor the right to cancel";
  }
}

TEST(VenueAdminSurface, SessionStillStampsOrderFlow)
{
  NewOrder o;
  o.accountId = kVictimAccount;
  EXPECT_EQ(std::get<NewOrder>(throughSession(InboundCommand{o})).accountId, kSessionAccount);

  CancelOrder c;
  c.accountId = kVictimAccount;
  EXPECT_EQ(std::get<CancelOrder>(throughSession(InboundCommand{c})).accountId, kSessionAccount);

  ModifyOrder m;
  m.accountId = kVictimAccount;
  EXPECT_EQ(std::get<ModifyOrder>(throughSession(InboundCommand{m})).accountId, kSessionAccount);

  MassCancel mc;
  mc.accountId = kVictimAccount;
  EXPECT_EQ(std::get<MassCancel>(throughSession(InboundCommand{mc})).accountId, kSessionAccount);

  Quote q;
  q.accountId = kVictimAccount;
  EXPECT_EQ(std::get<Quote>(throughSession(InboundCommand{q})).accountId, kSessionAccount);

  LastLookDecision ll;
  ll.accountId = kVictimAccount;
  EXPECT_EQ(std::get<LastLookDecision>(throughSession(InboundCommand{ll})).accountId,
            kSessionAccount);
}

// An unbound session (account 0, the trusted-transport default) is left alone:
// there is no authenticated identity to force onto the command.
TEST(VenueAdminSurface, UnboundSessionLeavesTheAccountAlone)
{
  Withdraw w;
  w.accountId = kVictimAccount;
  GatewaySession s(0, [w](const uint8_t*, size_t)
                   { return std::optional<InboundCommand>{InboundCommand{w}}; });
  s.authenticate(true);
  SessionReject rej{};
  const uint8_t frame[1] = {0};
  auto out = s.handle(frame, sizeof frame, 0, rej);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(std::get<Withdraw>(*out).accountId, kVictimAccount);
}

namespace
{
SymbolConfig listed(SymbolId id)
{
  SymbolConfig c;
  c.id = id;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}
}  // namespace

// A price band is a collar. A request that omits one of its two bounds used to
// answer ok and write a zero, which silently removes the collar an operator
// believed was in place.
TEST(VenueAdminSurface, MissingBandBoundIsRefusedRatherThanGuessed)
{
  InstrumentRegistry reg;
  reg.listInstrument(listed(1));
  ControlApi api(reg);

  const std::string resp = api.handle(R"({"method":"setBand","symbol":1})");
  EXPECT_TRUE(containsText(resp, "\"ok\":false")) << resp;
  EXPECT_EQ(reg.get(1)->minPrice, px(50.0)) << "the band must survive a request it did not accept";
  EXPECT_EQ(reg.get(1)->maxPrice, px(150.0));

  const std::string half = api.handle(R"({"method":"setBand","symbol":1,"minPrice":90})");
  EXPECT_TRUE(containsText(half, "\"ok\":false")) << half;
  EXPECT_EQ(reg.get(1)->maxPrice, px(150.0));

  // The well-formed request still works.
  EXPECT_TRUE(containsText(
      api.handle(R"({"method":"setBand","symbol":1,"minPrice":90,"maxPrice":110})"),
      "\"ok\":true"));
  EXPECT_EQ(reg.get(1)->minPrice, px(90.0));
  EXPECT_EQ(reg.get(1)->maxPrice, px(110.0));
}

TEST(VenueAdminSurface, OutOfRangeNumberIsRefused)
{
  InstrumentRegistry reg;
  reg.listInstrument(listed(1));
  ControlApi api(reg);

  for (const char* req : {R"({"method":"setBand","symbol":1,"minPrice":90,"maxPrice":1e300})",
                          R"({"method":"setBand","symbol":1,"minPrice":90,"maxPrice":inf})",
                          R"({"method":"setBand","symbol":1,"minPrice":90,"maxPrice":nan})",
                          R"({"method":"setBand","symbol":1,"minPrice":90,"maxPrice":1e11})",
                          R"({"method":"setBand","symbol":1,"minPrice":90,"maxPrice":"abc"})"})
  {
    const std::string resp = api.handle(req);
    EXPECT_TRUE(containsText(resp, "\"ok\":false")) << req << " -> " << resp;
  }
  EXPECT_EQ(reg.get(1)->maxPrice, px(150.0)) << "none of them may reach the registry";
}

TEST(VenueAdminSurface, InvertedBandIsRefused)
{
  InstrumentRegistry reg;
  reg.listInstrument(listed(1));
  ControlApi api(reg);

  const std::string resp =
      api.handle(R"({"method":"setBand","symbol":1,"minPrice":110,"maxPrice":90})");
  EXPECT_TRUE(containsText(resp, "\"ok\":false")) << resp;
  EXPECT_EQ(reg.get(1)->minPrice, px(50.0));
}

TEST(VenueAdminSurface, PairedRiskLimitsMustBothBeNamed)
{
  InstrumentRegistry reg;
  reg.listInstrument(listed(1));
  std::vector<InboundCommand> forwarded;
  ControlApi api(reg, [&forwarded](const InboundCommand& c)
                 { forwarded.push_back(c); });

  EXPECT_TRUE(containsText(api.handle(R"({"method":"setRiskLimits","symbol":1,"maxOrderQty":5})"),
                           "\"ok\":false"))
      << "a fat-finger cap without its notional half zeroes the notional half";
  EXPECT_TRUE(forwarded.empty());

  EXPECT_TRUE(containsText(
      api.handle(R"({"method":"setRiskLimits","symbol":1,"maxOrderQty":5,"maxOrderNotional":500})"),
      "\"ok\":true"));
  EXPECT_EQ(forwarded.size(), 1u);

  // A single-valued limit still travels alone.
  EXPECT_TRUE(containsText(api.handle(R"({"method":"setRiskLimits","symbol":1,"maxOpenOrders":25})"),
                           "\"ok\":true"));
}

// One connection must not be able to spend the venue's memory. The framed
// transport caps a length prefix at 16 MiB; the line-delimited admin surface
// had no cap at all, so a client that never sent a newline grew a string until
// the process died.
TEST(VenueAdminSurface, AdminRequestLineIsBounded)
{
  InstrumentRegistry reg;
  reg.listInstrument(listed(1));
  ControlApi api(reg);
  TcpControlServer srv(api);
  const int port = srv.start(0);
  ASSERT_GT(port, 0);

  const int fd = connectLoopback(port, /*rcvTimeoutSec*/ 2);
  ASSERT_GE(fd, 0);

  const std::string chunk(64 * 1024, 'A');  // no newline, ever
  size_t sent = 0;
  bool writeFailed = false;
  for (int i = 0; i < 64 && !writeFailed; ++i)  // 4 MiB, four times the cap
  {
    const ssize_t w = ::send(fd, chunk.data(), chunk.size(), 0);
    if (w <= 0)
    {
      writeFailed = true;
      break;
    }
    sent += static_cast<size_t>(w);
  }

  // The server must have dropped this connection rather than buffered it.
  // Either end of stream, or a reset because it closed while this side was
  // still pushing -- anything but a live connection waiting for more.
  std::string answer;
  ssize_t r = 0;
  char scratch[256];
  errno = 0;
  while ((r = ::recv(fd, scratch, sizeof scratch, 0)) > 0)
  {
    answer.append(scratch, static_cast<size_t>(r));
  }
  const bool timedOut = r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
  EXPECT_FALSE(timedOut) << "the server was still reading after " << sent
                         << " bytes with no newline";
  if (!answer.empty())
  {
    EXPECT_TRUE(containsText(answer, "request_too_long")) << answer;
  }

  ::close(fd);
  srv.stop();
}

TEST(VenueAdminSurface, BoundedLineStillServesOrdinaryRequests)
{
  InstrumentRegistry reg;
  reg.listInstrument(listed(1));
  ControlApi api(reg);
  TcpControlServer srv(api);
  const int port = srv.start(0);
  ASSERT_GT(port, 0);

  const int fd = connectLoopback(port, /*rcvTimeoutSec*/ 5);
  ASSERT_GE(fd, 0);

  const std::string req = R"({"method":"get","symbol":1})"
                          "\n";
  ASSERT_EQ(::send(fd, req.data(), req.size(), 0), static_cast<ssize_t>(req.size()));
  char buf[512];
  const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
  ASSERT_GT(r, 0);
  EXPECT_TRUE(containsText(std::string(buf, static_cast<size_t>(r)), "\"ok\":true"));

  ::close(fd);
  srv.stop();
}
