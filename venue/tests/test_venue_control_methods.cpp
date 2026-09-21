/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Verbs a deployment adds to the control plane.
//
// The surface has to be extensible from outside: an operator verb the
// framework does not ship must reach the same accept loop and the same line
// framing, be validated by the same field accessors, and obey the same
// journaling rule -- a change to engine state travels the sequenced stream, a
// read does not. Anything less and the deployment copies TcpControlServer to
// put two verbs of its own next to the built-in ones.

#include "flox-venue/control_api.h"
#include "flox-venue/control_plane.h"
#include "flox-venue/control_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

Price px(double v) { return Price::fromDouble(v); }

SymbolConfig listed(SymbolId id)
{
  SymbolConfig c;
  c.id = id;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
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

// One request line in, one response line out, with the receive timeout above
// as the deadline: a server that never answers fails the test instead of
// hanging the suite.
std::string ask(int fd, const std::string& request)
{
  const std::string line = request + "\n";
  if (::send(fd, line.data(), line.size(), 0) != static_cast<ssize_t>(line.size()))
  {
    return {};
  }
  std::string answer;
  char buf[1024];
  while (answer.find('\n') == std::string::npos)
  {
    const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
    if (r <= 0)
    {
      break;
    }
    answer.append(buf, static_cast<size_t>(r));
  }
  return answer;
}

}  // namespace

// The point of the whole exercise: a verb registered from outside is served by
// the stock TcpControlServer. No subclass, no second accept loop, no copy of
// the line framing -- the server is constructed from the same ControlApi and
// knows nothing about the verb.
TEST(VenueControlMethods, RegisteredMethodIsServedOverTcp)
{
  InstrumentRegistry reg;
  ASSERT_TRUE(reg.listInstrument(listed(1)));

  std::vector<InboundCommand> forwarded;
  ControlApi api(reg, [&](const InboundCommand& c)
                 { forwarded.push_back(c); });

  // A mutation: it validates through the request's own accessors, refuses a
  // symbol the registry does not know, and forwards the record that reproduces
  // it on replay.
  ASSERT_TRUE(api.registerMethod("parkInstrument",
                                 [](const ControlRequest& req)
                                 {
                                   SymbolId sym{};
                                   if (!req.symbolField("symbol", sym))
                                   {
                                     return ControlApi::err("bad_field");
                                   }
                                   if (!req.registry().get(sym))
                                   {
                                     return ControlApi::err("unknown_symbol");
                                   }
                                   req.forward(InboundCommand{AdminCmd{sym, AdminAction::Halt}});
                                   return ControlApi::ok();
                                 }));

  // A read: same surface, and it must leave the journaled stream alone.
  ASSERT_TRUE(api.registerMethod("countInstruments",
                                 [](const ControlRequest& req)
                                 {
                                   return std::string("{\"ok\":true,\"count\":") +
                                          std::to_string(req.registry().size()) + "}";
                                 }));

  TcpControlServer srv(api);
  const int port = srv.start(0);
  ASSERT_GT(port, 0);

  const int fd = connectLoopback(port, /*rcvTimeoutSec*/ 5);
  ASSERT_GE(fd, 0);

  EXPECT_TRUE(containsText(ask(fd, R"({"method":"parkInstrument","symbol":1})"), "\"ok\":true"));
  EXPECT_EQ(forwarded.size(), 1u);
  const auto* cmd = std::get_if<AdminCmd>(&forwarded.front());
  EXPECT_TRUE(cmd != nullptr && cmd->symbol == 1 && cmd->action == AdminAction::Halt);

  // The field accessors the request carries are the built-ins' own: a symbol
  // nobody named is absent, not zero.
  EXPECT_TRUE(containsText(ask(fd, R"({"method":"parkInstrument"})"), "bad_field"));
  EXPECT_TRUE(containsText(ask(fd, R"({"method":"parkInstrument","symbol":42})"), "unknown_symbol"));
  EXPECT_EQ(forwarded.size(), 1u) << "a refused mutation must not journal";

  // A read answers over the same connection and adds nothing to the stream.
  EXPECT_TRUE(containsText(ask(fd, R"({"method":"countInstruments"})"), "\"count\":1"));
  EXPECT_EQ(forwarded.size(), 1u) << "a read must not journal";

  // The built-in verbs still answer on the same api.
  EXPECT_TRUE(containsText(ask(fd, R"({"method":"get","symbol":1})"), "\"ok\":true"));

  ::close(fd);
  srv.stop();
}

// A name nobody registered has to come back named. A bare "unknown_method"
// reads the same for a typo, a registration that never ran, and a request that
// reached the wrong process.
TEST(VenueControlMethods, UnknownMethodAnswersWithTheName)
{
  InstrumentRegistry reg;
  ControlApi api(reg);

  const std::string answer = api.handle(R"({"method":"parkInstrument","symbol":1})");
  EXPECT_TRUE(containsText(answer, "\"ok\":false")) << answer;
  EXPECT_TRUE(containsText(answer, "unknown_method")) << answer;
  EXPECT_TRUE(containsText(answer, "\"method\":\"parkInstrument\"")) << answer;

  // Over TCP the same answer arrives, rather than a dropped line.
  TcpControlServer srv(api);
  const int port = srv.start(0);
  ASSERT_GT(port, 0);
  const int fd = connectLoopback(port, /*rcvTimeoutSec*/ 5);
  ASSERT_GE(fd, 0);
  const std::string overWire = ask(fd, R"({"method":"parkInstrument","symbol":1})");
  EXPECT_TRUE(containsText(overWire, "\"method\":\"parkInstrument\"")) << overWire;
  ::close(fd);
  srv.stop();

  // The echoed name is caller input, so it comes back escaped and cut instead
  // of breaking the response it travels in.
  const std::string quoted = api.handle(R"({"method":"say \"hi\""})");
  EXPECT_TRUE(containsText(quoted, "\\\"hi\\\"")) << quoted;
  const std::string longName = api.handle(std::string(R"({"method":")") + std::string(400, 'x') +
                                          R"("})");
  EXPECT_LT(longName.size(), 200u) << longName;
}

// Registration refuses what would make the routing ambiguous, so a verb never
// silently answers in place of another.
TEST(VenueControlMethods, RegistrationRefusesCollisions)
{
  InstrumentRegistry reg;
  ControlApi api(reg);
  auto stub = [](const ControlRequest&)
  { return ControlApi::ok(); };

  EXPECT_FALSE(api.registerMethod("", stub));
  EXPECT_FALSE(api.registerMethod("parkInstrument", ControlApi::Method{}));
  EXPECT_TRUE(api.registerMethod("parkInstrument", stub));
  EXPECT_FALSE(api.registerMethod("parkInstrument", stub)) << "a name is taken once";

  // Every built-in name is refused, and every name on that list is one the
  // built-in chain really answers -- a stale entry would silently make a
  // registerable name unregisterable.
  for (const std::string_view name : ControlApi::kBuiltinMethods)
  {
    EXPECT_FALSE(api.registerMethod(std::string(name), stub)) << name;
    const std::string answer = api.handle(std::string(R"({"method":")") + std::string(name) + R"("})");
    EXPECT_FALSE(containsText(answer, "unknown_method")) << name << " -> " << answer;
  }
}
