/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/fix_codec.h"
#include "flox-venue/itch_codec.h"
#include "flox-venue/market_data.h"
#include "flox-venue/ouch_codec.h"
#include "flox-venue/rest_json.h"
#include "flox/util/websocket.h"

#include <gtest/gtest.h>
#include <cstdint>

#include <cstdio>
#include <cstdlib>
#include <string>
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

struct Rng
{
  uint64_t s;
  uint64_t next()
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

NewOrder sampleOrder()
{
  NewOrder o;
  o.id = 42;
  o.symbol = 1;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = px(100);
  o.quantity = qty(5);
  o.accountId = 7;
  return o;
}

void test_ouch_fuzz()
{
  std::printf("test_ouch_parser_fuzz\n");
  std::vector<uint8_t> wire;
  OuchCodec::encode(InboundCommand{sampleOrder()}, wire);
  CHECK(!wire.empty());

  // Positive control: a valid frame round-trips.
  auto ok = OuchCodec::decode(wire.data(), wire.size());
  CHECK(ok.has_value());

  Rng rng{0xF0F0F0F0ULL};

  // Truncation: decode every prefix length (including 0). Must not crash.
  for (size_t len = 0; len <= wire.size(); ++len)
  {
    auto d = OuchCodec::decode(wire.data(), len);
    (void)d;
  }

  // Bit-flip corruption of the valid frame.
  for (int i = 0; i < 200000; ++i)
  {
    std::vector<uint8_t> c = wire;
    const size_t flips = 1 + (rng.next() % 4);
    for (size_t f = 0; f < flips; ++f)
    {
      c[rng.next() % c.size()] ^= static_cast<uint8_t>(rng.next() & 0xFF);
    }
    auto d = OuchCodec::decode(c.data(), c.size());
    (void)d;
  }

  // Pure random buffers of random length.
  int decoded = 0;
  for (int i = 0; i < 300000; ++i)
  {
    uint8_t buf[64];
    const size_t n = rng.next() % 64;
    for (size_t k = 0; k < n; ++k)
    {
      buf[k] = static_cast<uint8_t>(rng.next() & 0xFF);
    }
    auto d = OuchCodec::decode(buf, n);
    if (d)
    {
      ++decoded;
    }
  }
  CHECK(true);  // reached here without crashing
  std::printf("  ouch survived truncation + 200k corruptions + 300k random (%d parsed)\n", decoded);
}

void test_fix_fuzz()
{
  std::printf("test_fix_parser_fuzz\n");
  Rng rng{0xABCDEF01ULL};
  const char* seeds[] = {
      "8=FIX.4.4\x01"
      "35=D\x01"
      "11=1\x01"
      "55=1\x01"
      "54=1\x01"
      "38=5\x01"
      "44=100\x01"
      "10=000\x01",
      "8=FIX.4.4\x01"
      "35=F\x01"
      "11=2\x01"
      "41=1\x01"
      "10=000\x01",
      "garbage-not-fix",
      "",
  };
  for (const char* seed : seeds)
  {
    const std::string base(seed);
    // Truncations.
    for (size_t len = 0; len <= base.size(); ++len)
    {
      auto d = FixCodec::decode(base.substr(0, len));
      (void)d;
    }
    // Corruptions + random junk.
    for (int i = 0; i < 100000; ++i)
    {
      std::string c = base;
      if (!c.empty())
      {
        const size_t flips = 1 + (rng.next() % 5);
        for (size_t f = 0; f < flips; ++f)
        {
          c[rng.next() % c.size()] ^= static_cast<char>(rng.next() & 0x7F);
        }
      }
      const size_t extra = rng.next() % 8;
      for (size_t e = 0; e < extra; ++e)
      {
        c.push_back(static_cast<char>(rng.next() & 0xFF));
      }
      auto d = FixCodec::decode(c);
      (void)d;
    }
  }
  // Random ASCII with SOH separators.
  for (int i = 0; i < 100000; ++i)
  {
    std::string s;
    const size_t n = rng.next() % 40;
    for (size_t k = 0; k < n; ++k)
    {
      const uint32_t c = rng.next() % 40;
      s.push_back(c == 0 ? '\x01' : static_cast<char>('0' + (c % 74)));
    }
    auto d = FixCodec::decode(s);
    (void)d;
  }
  CHECK(true);
  std::printf("  fix survived truncation + corruption + random\n");
}

void test_rest_fuzz()
{
  std::printf("test_rest_parser_fuzz\n");
  Rng rng{0x12345678ULL};
  const char* seeds[] = {
      R"({"action":"new","id":1,"symbol":1,"side":"buy","ordType":"limit","qty":5,"price":100})",
      R"({"action":"cancel","id":1,"symbol":1})",
      R"({"action":"new","id":1)",  // truncated
      R"(not json at all)",
      R"({})",
      "",
  };
  for (const char* seed : seeds)
  {
    const std::string base(seed);
    for (size_t len = 0; len <= base.size(); ++len)
    {
      auto d = RestJson::decode(base.substr(0, len));
      (void)d;
    }
    for (int i = 0; i < 100000; ++i)
    {
      std::string c = base;
      if (!c.empty())
      {
        const size_t flips = 1 + (rng.next() % 5);
        for (size_t f = 0; f < flips; ++f)
        {
          c[rng.next() % c.size()] ^= static_cast<char>(rng.next() & 0x7F);
        }
      }
      auto d = RestJson::decode(c);
      (void)d;
    }
  }
  // Random braces/quotes/digits soup.
  const char* alphabet = "{}[]\":,0123456789abcstunew ";
  for (int i = 0; i < 200000; ++i)
  {
    std::string s;
    const size_t n = rng.next() % 60;
    for (size_t k = 0; k < n; ++k)
    {
      s.push_back(alphabet[rng.next() % 27]);
    }
    auto d = RestJson::decode(s);
    (void)d;
  }
  CHECK(true);
  std::printf("  rest survived truncation + corruption + random soup\n");
}

void test_ws_fuzz()
{
  std::printf("test_ws_parser_fuzz\n");
  // A valid text frame as the corruption/truncation seed.
  const uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
  const std::vector<uint8_t> frame = ws::buildFrame(ws::Opcode::Text, payload, sizeof payload);

  ws::Opcode op{};
  std::vector<uint8_t> out;
  for (size_t len = 0; len <= frame.size(); ++len)
  {
    ws::parseFrame(frame.data(), len, op, out);  // truncation: no OOB on short length fields
  }

  Rng rng{0x5151A1A1ULL};
  for (int i = 0; i < 200000; ++i)
  {
    std::vector<uint8_t> c = frame;
    const size_t flips = 1 + (rng.next() % 6);
    for (size_t f = 0; f < flips; ++f)
    {
      c[rng.next() % c.size()] ^= static_cast<uint8_t>(rng.next() & 0xFF);
    }
    ws::parseFrame(c.data(), c.size(), op, out);
  }
  for (int i = 0; i < 300000; ++i)
  {
    uint8_t buf[80];
    const size_t n = rng.next() % 80;
    for (size_t k = 0; k < n; ++k)
    {
      buf[k] = static_cast<uint8_t>(rng.next() & 0xFF);
    }
    ws::parseFrame(buf, n, op, out);  // arbitrary lengths incl. 64-bit length headers
  }

  // Handshake HTTP header parsing.
  const std::string hs =
      "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
  for (size_t len = 0; len <= hs.size(); ++len)
  {
    auto r = ws::handshakeResponse(hs.substr(0, len));
    (void)r;
  }
  for (int i = 0; i < 50000; ++i)
  {
    std::string c = hs;
    const size_t flips = 1 + (rng.next() % 8);
    for (size_t f = 0; f < flips; ++f)
    {
      c[rng.next() % c.size()] ^= static_cast<char>(rng.next() & 0x7F);
    }
    auto r = ws::handshakeResponse(c);
    (void)r;
  }
  CHECK(true);
  std::printf("  ws survived truncation + corruption + random frames + handshake fuzz\n");
}

void test_itch_fuzz()
{
  std::printf("test_itch_parser_fuzz\n");
  Rng rng{0x1DC4C0DEULL};
  MdMessage m{};
  for (int i = 0; i < 300000; ++i)
  {
    uint8_t buf[48];
    const size_t n = rng.next() % 48;
    for (size_t k = 0; k < n; ++k)
    {
      buf[k] = static_cast<uint8_t>(rng.next() & 0xFF);
    }
    ItchCodec::decode(buf, n, m);  // big-endian field reads on short/garbage buffers
  }
  CHECK(true);
  std::printf("  itch survived random buffers\n");
}

}  // namespace

TEST(ParserFuzz, EngineSuite)
{
  test_ouch_fuzz();
  test_fix_fuzz();
  test_rest_fuzz();
  test_ws_fuzz();
  test_itch_fuzz();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
