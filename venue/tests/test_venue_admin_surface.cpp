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

// What a session did with a command: the command it let through, or the
// reason it refused.
struct Verdict
{
  std::optional<InboundCommand> cmd;
  SessionReject reject{SessionReject::None};
};

// A session whose decoder always yields `cmd`, authenticated as kSessionAccount.
Verdict throughSession(const InboundCommand& cmd)
{
  GatewaySession s(kSessionAccount, [cmd](const uint8_t*, size_t)
                   { return std::optional<InboundCommand>{cmd}; });
  s.authenticate(true);
  SessionReject rej{};
  const uint8_t frame[1] = {0};
  auto out = s.handle(frame, sizeof frame, 0, rej);
  return Verdict{std::move(out), rej};
}

// Read and write the account field by the same walk over the variant the
// session uses, so a command added later is covered here too.
uint64_t accountNamedBy(const InboundCommand& c)
{
  return std::visit(
      [](const auto& m) -> uint64_t
      {
        using Cmd = std::remove_cvref_t<decltype(m)>;
        if constexpr (HasAccountId<Cmd>)
        {
          return m.accountId;
        }
        else if constexpr (HasAccount<Cmd>)
        {
          return m.account;
        }
        else
        {
          return 0;
        }
      },
      c);
}

void nameAccount(InboundCommand& c, uint64_t a)
{
  std::visit(
      [a](auto& m)
      {
        using Cmd = std::remove_reference_t<decltype(m)>;
        if constexpr (HasAccountId<Cmd>)
        {
          m.accountId = a;
        }
        else if constexpr (HasAccount<Cmd>)
        {
          m.account = a;
        }
      },
      c);
}

// Every command that carries an account: the order flow, the money moves, the
// position close, and the per-account configuration. The last three were the
// ones a hand-written list used to miss.
std::vector<InboundCommand> accountBearingCommands()
{
  Withdraw w;
  w.amountRaw = 750000;
  SetStpGroup g;
  g.group = 77;
  SetAdmissionProfile prof;
  prof.profile.deny = AdmissionDeny::DenyCancel | AdmissionDeny::DenyAmend;
  return {InboundCommand{w}, InboundCommand{Deposit{}},
          InboundCommand{ForceClosePosition{}},
          InboundCommand{g}, InboundCommand{prof},
          InboundCommand{NewOrder{}}, InboundCommand{CancelOrder{}},
          InboundCommand{ModifyOrder{}},
          InboundCommand{MassCancel{}}, InboundCommand{Quote{}},
          InboundCommand{LastLookDecision{}}};
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

// A command that names an account the session does not speak for is REFUSED.
//
// It used to be stamped instead: the session's own account was written over
// whatever the payload carried. That kept the victim safe, but it was only
// ever possible because there was exactly one account to write -- and it told
// the client nothing, so an order aimed at the wrong account was quietly
// placed on a different one.
//
// The refusal also has to cover every command that carries an account, not a
// remembered subset. A hand-written list covered the six order-flow commands
// and missed the ones that move money, close positions and set another
// account's entitlements.
TEST(VenueAdminSurface, SessionRefusesACommandNamingAnAccountItDoesNotSpeakFor)
{
  for (auto cmd : accountBearingCommands())
  {
    nameAccount(cmd, kVictimAccount);
    const auto v = throughSession(cmd);
    EXPECT_FALSE(v.cmd.has_value()) << "command index " << cmd.index() << " was let through";
    EXPECT_EQ(v.reject, SessionReject::Unauthenticated) << "command index " << cmd.index();
  }
}

// A command that names NO account is the client saying "me": there is exactly
// one answer, so it is stamped rather than refused.
TEST(VenueAdminSurface, SessionStampsItsOwnAccountOntoACommandThatNamesNone)
{
  for (auto cmd : accountBearingCommands())
  {
    nameAccount(cmd, 0);
    const auto v = throughSession(cmd);
    ASSERT_TRUE(v.cmd.has_value())
        << "command index " << cmd.index() << " refused: " << toString(v.reject);
    EXPECT_EQ(accountNamedBy(*v.cmd), kSessionAccount) << "command index " << cmd.index();
  }
}

// A session that speaks for several accounts keeps the one the client named,
// as long as it is one of them.
TEST(VenueAdminSurface, SessionKeepsAnAccountItSpeaksFor)
{
  for (auto cmd : accountBearingCommands())
  {
    nameAccount(cmd, kVictimAccount);
    GatewaySession s(std::vector<uint64_t>{kSessionAccount, kVictimAccount},
                     [cmd](const uint8_t*, size_t)
                     { return std::optional<InboundCommand>{cmd}; });
    s.authenticate(true);
    SessionReject rej{};
    const uint8_t frame[1] = {0};
    auto out = s.handle(frame, sizeof frame, 0, rej);
    ASSERT_TRUE(out.has_value()) << "command index " << cmd.index() << ": " << toString(rej);
    EXPECT_EQ(accountNamedBy(*out), kVictimAccount) << "command index " << cmd.index();
  }
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
