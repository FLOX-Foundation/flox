/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Hostile input into the client side of a FIX session, on the model of
 * venue/tests/test_venue_parser_fuzz.cpp. An initiator connects OUT, which
 * means the bytes it parses come from a machine it does not run, so the parser
 * is an attack surface whether or not the counterparty is friendly.
 *
 * Two layers are fuzzed. ClientCodec::decode is the message parser: it must
 * return a value or nullopt for any byte string and must never read out of
 * bounds. FixInitiator::onFrame is the session layer above it: it must reach a
 * verdict for any byte string, must not corrupt its sequence state on garbage,
 * and must not be talked past the sequence gate.
 *
 * The value comes from running this under AddressSanitizer and
 * UndefinedBehaviorSanitizer, where an out-of-bounds read is a failure rather
 * than a wrong answer. A clean run without them proves much less.
 */
#include "flox/connector/fix/fix_client_codec.h"
#include "flox/connector/fix/fix_initiator.h"
#include "flox/connector/fix/fix_wire.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace flox;

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

std::string execReport()
{
  std::string b = "35=8";
  b += fix::kSoh;
  fix::appendHeader(b, 2, "VENUE", "CLIENT", fix::sendingTime(0));
  auto add = [&b](int t, const std::string& v)
  { b += std::to_string(t) + "=" + v + fix::kSoh; };
  add(37, "5");
  add(11, "5");
  add(55, "1");
  add(54, "1");
  add(150, "U");
  add(39, "0");
  add(32, "2.5");
  add(31, "100.25");
  add(151, "7.5");
  add(20001, "99");
  add(20002, "12");
  add(58, "held");
  return fix::frame(b);
}

void test_client_codec_fuzz()
{
  std::printf("test_fix_client_codec_fuzz\n");

  const std::vector<std::string> seeds = {
      execReport(),
      fix::encodeAdmin("9", 3, "VENUE", "CLIENT", fix::sendingTime(0),
                       {{37, "5"}, {41, "5"}, {39, "8"}, {434, "1"}, {102, "1"}, {58, "no"}}),
      fix::encodeAdmin("3", 4, "VENUE", "CLIENT", fix::sendingTime(0),
                       {{45, "7"}, {371, "44"}, {372, "D"}, {373, "5"}}),
      fix::encodeAdmin("j", 5, "VENUE", "CLIENT", fix::sendingTime(0),
                       {{45, "7"}, {372, "G"}, {380, "3"}}),
      std::string("8=FIX.4.4\x01"
                  "9=0\x01"
                  "35=8\x01"
                  "10=000\x01"),
      std::string("garbage-not-fix"),
      std::string("35=8\x01\x01\x01\x01="),
      std::string("="),
      std::string(),
  };

  // Positive control: the seeds this codec is meant to read, do read. A fuzz
  // run over inputs the parser rejects at the first byte proves nothing.
  auto ok = fix::ClientCodec::decode(seeds[0]);
  CHECK(ok.has_value());
  if (ok)
  {
    const auto* e = std::get_if<fix::ExecutionReport>(&*ok);
    CHECK(e != nullptr && e->execType == fix::ExecType::FillHeld && e->heldId == 99);
  }
  CHECK(fix::ClientCodec::decode(seeds[1]).has_value());
  CHECK(fix::ClientCodec::decode(seeds[2]).has_value());
  CHECK(fix::ClientCodec::decode(seeds[3]).has_value());

  Rng rng{0x1234ABCDULL};
  int decoded = 0;
  for (const std::string& base : seeds)
  {
    // Every prefix, including the empty one.
    for (size_t len = 0; len <= base.size(); ++len)
    {
      auto d = fix::ClientCodec::decode(base.substr(0, len));
      (void)d;
    }
    // Byte corruption of a valid frame.
    for (int i = 0; i < 60000; ++i)
    {
      std::string c = base;
      if (!c.empty())
      {
        const size_t flips = 1 + (rng.next() % 5);
        for (size_t f = 0; f < flips; ++f)
        {
          c[rng.next() % c.size()] =
              static_cast<char>(static_cast<unsigned char>(c[rng.next() % c.size()]) ^
                                static_cast<unsigned char>(rng.next() & 0xFF));
        }
      }
      if (fix::ClientCodec::decode(c))
      {
        ++decoded;
      }
    }
  }

  // Pure random buffers, including embedded NULs and SOH runs.
  for (int i = 0; i < 300000; ++i)
  {
    const size_t n = rng.next() % 96;
    std::string buf;
    buf.reserve(n);
    for (size_t k = 0; k < n; ++k)
    {
      const uint64_t r = rng.next();
      // Weighted towards the bytes that mean something in tag=value framing,
      // so the fuzzer spends its time inside the parser rather than bouncing
      // off the first character.
      switch (r % 8)
      {
        case 0:
          buf.push_back(fix::kSoh);
          break;
        case 1:
          buf.push_back('=');
          break;
        case 2:
          buf.push_back(static_cast<char>('0' + (r >> 8) % 10));
          break;
        default:
          buf.push_back(static_cast<char>((r >> 8) & 0xFF));
          break;
      }
    }
    if (fix::ClientCodec::decode(buf))
    {
      ++decoded;
    }
  }

  // Numeric fields that are not numbers, are too long, or overflow: the
  // decimal parser rejects rather than coercing, and nothing here may trap.
  const char* nasty[] = {"",
                         "-1",
                         "+1",
                         "1e300",
                         "0x10",
                         ".",
                         "..",
                         "1.",
                         ".5",
                         "999999999999999999999999999999",
                         "1.000000000000000000000001",
                         "99999999999999999999.99999999",
                         "nan",
                         "inf",
                         "1 2"};
  for (const char* v : nasty)
  {
    std::string b = "35=8";
    b += fix::kSoh;
    fix::appendHeader(b, 2, "VENUE", "CLIENT", fix::sendingTime(0));
    b += std::string("44=") + v + fix::kSoh;
    b += std::string("38=") + v + fix::kSoh;
    b += std::string("37=") + v + fix::kSoh;
    b += std::string("150=") + v + fix::kSoh;
    auto d = fix::ClientCodec::decode(fix::frame(b));
    CHECK(d.has_value());  // an exec report with an unreadable optional field
    if (d)                 // is still a report; it is the value that is dropped
    {
      const auto* e = std::get_if<fix::ExecutionReport>(&*d);
      CHECK(e != nullptr);
    }
  }

  std::printf("  client codec survived prefixes + corruption + 300k random (%d parsed)\n", decoded);
}

// A well-formed message of the given type at the given seq, the way a correct
// acceptor would write it. Mutating one of these reaches far deeper into the
// session layer than a random buffer, which dies at the sequence gate.
std::string peerFrame(const std::string& type, uint64_t seq)
{
  if (type == "8")
  {
    std::string b = "35=8";
    b += fix::kSoh;
    fix::appendHeader(b, seq, "VENUE", "CLIENT", fix::sendingTime(0));
    auto add = [&b](int t, const std::string& v)
    { b += std::to_string(t) + "=" + v + fix::kSoh; };
    add(37, "5");
    add(11, "5");
    add(55, "1");
    add(54, "1");
    add(150, "F");
    add(39, "1");
    add(32, "1.5");
    add(31, "100.25");
    add(151, "3.5");
    return fix::frame(b);
  }
  if (type == "1")
  {
    return fix::encodeAdmin("1", seq, "VENUE", "CLIENT", fix::sendingTime(0), {{112, "probe"}});
  }
  if (type == "2")
  {
    return fix::encodeAdmin("2", seq, "VENUE", "CLIENT", fix::sendingTime(0),
                            {{7, "1"}, {16, "0"}});
  }
  if (type == "4")
  {
    return fix::encodeAdmin("4", seq, "VENUE", "CLIENT", fix::sendingTime(0),
                            {{123, "Y"}, {36, std::to_string(seq + 1)}});
  }
  if (type == "9")
  {
    return fix::encodeAdmin("9", seq, "VENUE", "CLIENT", fix::sendingTime(0),
                            {{37, "5"}, {41, "5"}, {39, "8"}, {434, "1"}, {102, "1"}});
  }
  return fix::encodeAdmin(type, seq, "VENUE", "CLIENT", fix::sendingTime(0));
}

void test_initiator_frame_fuzz()
{
  std::printf("test_fix_initiator_frame_fuzz\n");

  Rng rng{0x99AA55FFULL};
  int disconnects = 0;
  int reports = 0;
  int64_t consumed = 0;

  const char* types[] = {"8", "0", "1", "2", "4", "9", "3", "j", "A", "5", "AE", "ZZ"};

  fix::FixInitiatorConfig cfg;
  cfg.senderCompId = "CLIENT";
  cfg.targetCompId = "VENUE";
  cfg.heartBtIntSec = 10;

  auto fresh = [&](fix::FixInitiator& into)
  {
    into.setSend([](const std::string&)
                 { return true; });
    into.setReportHandler([&reports](const fix::InboundReport&)
                          { ++reports; });
    into.connect(0);
    into.onFrame(fix::encodeAdmin("A", 1, "VENUE", "CLIENT", fix::sendingTime(0), {{108, "10"}}),
                 0);
  };

  for (int round = 0; round < 3000; ++round)
  {
    auto initiator = std::make_unique<fix::FixInitiator>(cfg);
    fresh(*initiator);
    uint64_t peerSeq = 2;

    for (int i = 0; i < 40; ++i)
    {
      std::string buf;
      const uint64_t roll = rng.next() % 10;
      if (roll < 7)
      {
        // A valid frame at the seq the session is waiting for, with a handful
        // of bytes corrupted. This is the input that gets past the gate and
        // into the field parsing, which is where the memory bugs live.
        buf = peerFrame(types[rng.next() % (sizeof(types) / sizeof(types[0]))], peerSeq);
        if (roll < 5 && !buf.empty())
        {
          const size_t flips = 1 + (rng.next() % 3);
          for (size_t f = 0; f < flips; ++f)
          {
            const size_t at = rng.next() % buf.size();
            buf[at] = static_cast<char>(static_cast<unsigned char>(buf[at]) ^
                                        static_cast<unsigned char>(rng.next() & 0xFF));
          }
        }
      }
      else
      {
        const size_t n = rng.next() % 128;
        buf.reserve(n);
        for (size_t k = 0; k < n; ++k)
        {
          const uint64_t r = rng.next();
          switch (r % 6)
          {
            case 0:
              buf.push_back(fix::kSoh);
              break;
            case 1:
              buf.push_back('=');
              break;
            case 2:
              buf.push_back(static_cast<char>('0' + (r >> 8) % 10));
              break;
            default:
              buf.push_back(static_cast<char>((r >> 8) & 0xFF));
              break;
          }
        }
      }

      const int64_t nowNs = static_cast<int64_t>(i) * 1'000'000'000LL;
      if (initiator->onFrame(buf, nowNs) == fix::FixInitiator::Verdict::Disconnect)
      {
        ++disconnects;
        // Start over rather than stopping: a session that dies on frame one
        // every round never exercises the code past frame one.
        initiator = std::make_unique<fix::FixInitiator>(cfg);
        fresh(*initiator);
        peerSeq = 2;
        continue;
      }
      ++consumed;
      peerSeq = initiator->seqState().expectedIn;
      initiator->onTick(nowNs);

      // Whatever the garbage did, the sequence state stays a valid FIX 4.4
      // sequence space: a buffer that could drive the counters to zero would
      // be a buffer that talks its way past the gate.
      const fix::FixSeqState s = initiator->seqState();
      CHECK(s.nextOut >= 1 && s.expectedIn >= 1);
    }
  }

  // The fuzz has to reach the application path, or it is only testing the
  // sequence gate. Reports arriving is the evidence that it did.
  CHECK(reports > 1000);
  CHECK(consumed > 10000);

  // Structurally valid frames whose FIELDS are hostile: the header parses, so
  // these reach further in than random bytes ever do.
  fix::FixInitiator initiator;
  initiator.setSend([](const std::string&)
                    { return true; });
  initiator.connect(0);
  initiator.onFrame(fix::encodeAdmin("A", 1, "VENUE", "CLIENT", fix::sendingTime(0), {{108, "10"}}),
                    0);
  const char* seqValues[] = {"0",
                             "1",
                             "18446744073709551615",
                             "99999999999999999999999999",
                             "-1",
                             "abc",
                             ""};
  for (const char* v : seqValues)
  {
    std::string b = "35=0";
    b += fix::kSoh;
    b += std::string("34=") + v + fix::kSoh;
    b += "49=VENUE";
    b += fix::kSoh;
    b += "56=CLIENT";
    b += fix::kSoh;
    b += "52=";
    b += fix::sendingTime(0);
    b += fix::kSoh;
    initiator.onFrame(fix::frame(b), 0);
    const fix::FixSeqState s = initiator.seqState();
    CHECK(s.expectedIn >= 1);
  }

  // A GapFill naming an absurd NewSeqNo must not be able to fabricate a
  // sequence space the counterparty will never reach.
  fix::FixInitiator gapVictim;
  gapVictim.setSend([](const std::string&)
                    { return true; });
  gapVictim.connect(0);
  gapVictim.onFrame(fix::encodeAdmin("A", 1, "VENUE", "CLIENT", fix::sendingTime(0), {{108, "10"}}),
                    0);
  for (const char* v : {"0", "18446744073709551615", "-5", "abc"})
  {
    gapVictim.onFrame(fix::encodeAdmin("4", 2, "VENUE", "CLIENT", fix::sendingTime(0),
                                       {{123, "Y"}, {36, v}}),
                      0);
    CHECK(gapVictim.seqState().expectedIn >= 1);
  }

  std::printf(
      "  initiator survived 3000 x 40 hostile frames: %lld consumed, %d disconnects, "
      "%d reports reached the application path\n",
      static_cast<long long>(consumed), disconnects, reports);
}

}  // namespace

TEST(FixClientParser, HostileInput)
{
  test_client_codec_fuzz();
  test_initiator_frame_fuzz();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
