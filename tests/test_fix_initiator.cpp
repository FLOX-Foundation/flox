/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The FIX initiator's session rules, driven from a scripted counterparty in
 * memory: no sockets, no venue module, so the rules are pinned where they are
 * decided rather than where they happen to show through. The paired test
 * against the venue's real acceptor lives in
 * venue/tests/test_venue_fix_initiator.cpp.
 */
#include "flox/connector/fix/fix_client_codec.h"
#include "flox/connector/fix/fix_executor.h"
#include "flox/connector/fix/fix_initiator.h"
#include "flox/connector/fix/fix_wire.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <variant>
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

constexpr int64_t kSec = 1'000'000'000LL;

// A counterparty that exists only as a sequence number and an encoder. It is
// deliberately dumber than FixConnection: it lets a test send exactly the bytes
// it wants to send, including the ones a correct acceptor never would.
struct Peer
{
  uint64_t seq{1};
  std::string sender{"VENUE"};
  std::string target{"CLIENT"};

  std::string admin(const std::string& type,
                    const std::vector<std::pair<int, std::string>>& fields = {},
                    uint64_t seqOverride = 0, bool possDup = false)
  {
    const uint64_t s = seqOverride != 0 ? seqOverride : seq++;
    return fix::encodeAdmin(type, s, sender, target, fix::sendingTime(0), fields, possDup);
  }

  // A raw message of an arbitrary type, so a test can send a MsgType no
  // acceptor should be sending.
  std::string raw(const std::string& type, uint64_t seqOverride = 0)
  {
    const uint64_t s = seqOverride != 0 ? seqOverride : seq++;
    return fix::encodeAdmin(type, s, sender, target, fix::sendingTime(0));
  }

  std::string execReport(uint64_t orderId, const char* execType, const char* price,
                         uint64_t seqOverride = 0, bool possDup = false)
  {
    const uint64_t s = seqOverride != 0 ? seqOverride : seq++;
    std::string b = "35=8";
    b += fix::kSoh;
    fix::appendHeader(b, s, sender, target, fix::sendingTime(0), possDup,
                      possDup ? fix::sendingTime(0) : std::string{});
    auto add = [&b](int t, const std::string& v)
    { b += std::to_string(t) + "=" + v + fix::kSoh; };
    add(37, std::to_string(orderId));
    add(11, std::to_string(orderId));
    add(55, "1");
    add(54, "1");
    add(150, execType);
    add(39, "0");
    add(44, price);
    add(151, "5");
    return fix::frame(b);
  }
};

// The initiator under test plus a record of everything it wrote.
struct Harness
{
  fix::FixInitiator initiator;
  std::vector<std::string> sent;
  std::vector<fix::InboundReport> reports;

  explicit Harness(fix::FixInitiatorConfig cfg = defaults()) : initiator(cfg)
  {
    initiator.setSend(
        [this](const std::string& m)
        {
          sent.push_back(m);
          return true;
        });
    initiator.setReportHandler([this](const fix::InboundReport& r)
                               { reports.push_back(r); });
  }

  static fix::FixInitiatorConfig defaults()
  {
    fix::FixInitiatorConfig c;
    c.senderCompId = "CLIENT";
    c.targetCompId = "VENUE";
    c.heartBtIntSec = 10;
    return c;
  }

  // The last message of `type` the initiator wrote, as parsed fields.
  bool last(const std::string& type, fix::Fields& out) const
  {
    for (auto it = sent.rbegin(); it != sent.rend(); ++it)
    {
      fix::Fields f = fix::parseFields(*it);
      if (f[35] == type)
      {
        out = f;
        return true;
      }
    }
    return false;
  }

  size_t count(const std::string& type) const
  {
    size_t n = 0;
    for (const std::string& m : sent)
    {
      if (fix::parseFields(m)[35] == type)
      {
        ++n;
      }
    }
    return n;
  }

  // Log on against a compliant peer, so each test starts from a live session.
  void logon(Peer& p, int64_t nowNs = 0)
  {
    initiator.connect(nowNs);
    initiator.onFrame(p.admin("A", {{108, "10"}, {141, "Y"}}), nowNs);
  }
};

fix::NewOrderRequest order(uint64_t id, const char* price, const char* quantity)
{
  fix::NewOrderRequest o;
  o.clOrdId = id;
  o.symbol = 1;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  int64_t raw = 0;
  CHECK(decwire::parse(price, raw));
  o.price = Price::fromRaw(raw);
  CHECK(decwire::parse(quantity, raw));
  o.quantity = Quantity::fromRaw(raw);
  return o;
}

// (1) The outbound Logon carries what FIX 4.4 requires of one, and the reply's
// HeartBtInt wins over what we proposed.
void test_logon_fields_and_heartbeat_adoption()
{
  std::printf("test_logon_fields_and_heartbeat_adoption\n");
  Harness h;
  Peer p;
  CHECK(h.initiator.connect(0));

  fix::Fields f;
  CHECK(h.last("A", f));
  CHECK(f[8] == "FIX.4.4");
  CHECK(f[34] == "1");
  CHECK(f[49] == "CLIENT" && f[56] == "VENUE");
  CHECK(f.count(52) != 0);
  CHECK(f[98] == "0");
  CHECK(f[108] == "10");
  CHECK(f[141] == "Y");  // a fresh space by default
  CHECK(fix::checksumValid(h.sent.back()));
  CHECK(!h.initiator.loggedOn());

  // The acceptor answers with 1, not the 10 we asked for. Its number governs.
  h.initiator.onFrame(p.admin("A", {{108, "1"}, {141, "Y"}}), 0);
  CHECK(h.initiator.loggedOn());
  CHECK(h.initiator.negotiatedHeartBtIntSec() == 1);
  CHECK(h.initiator.seqState().expectedIn == 2);
  CHECK(h.initiator.seqState().nextOut == 2);

  // The cancel-on-disconnect tag rides on the Logon when the session asks for
  // it, and stays off the wire when it does not.
  fix::FixInitiatorConfig codCfg = Harness::defaults();
  codCfg.cancelOnDisconnect = true;
  Harness cod{codCfg};
  cod.initiator.connect(0);
  CHECK(cod.last("A", f) && f[20003] == "Y");
}

// (2) Liveness in both directions.
void test_heartbeat_and_testrequest()
{
  std::printf("test_heartbeat_and_testrequest\n");
  fix::FixInitiatorConfig cfg = Harness::defaults();
  cfg.heartBtIntSec = 1;
  Harness h{cfg};
  Peer p;
  h.logon(p);
  CHECK(h.initiator.negotiatedHeartBtIntSec() == 10);  // the peer's 108 in logon()

  // Their TestRequest comes back as a Heartbeat echoing 112, which is the only
  // answer that proves WHICH probe we answered.
  h.initiator.onFrame(p.admin("1", {{112, "probe-7"}}), 0);
  fix::Fields f;
  CHECK(h.last("0", f));
  CHECK(f[112] == "probe-7");

  // Outbound silence past HeartBtInt emits our own Heartbeat.
  const size_t before = h.count("0");
  CHECK(h.initiator.onTick(11 * kSec));
  CHECK(h.count("0") == before + 1);

  // Inbound silence past 1.2 intervals probes, and an unanswered probe ends
  // the session rather than leaving a half-open peer holding orders.
  CHECK(h.initiator.onTick(30 * kSec));
  CHECK(h.count("1") == 1);
  CHECK(!h.initiator.onTick(60 * kSec));
}

// (3) An in-sequence message we do not speak is answered, and the answer
// distinguishes "the session layer could not process this" from "the
// application declines this".
void test_reject_unknown_type()
{
  std::printf("test_reject_unknown_type\n");
  Harness h;
  Peer p;
  h.logon(p);

  h.initiator.onFrame(p.raw("ZZ"), 0);  // not a FIX 4.4 type at all
  fix::Fields f;
  CHECK(h.last("3", f));
  CHECK(f[45] == "2");  // RefSeqNum: the message we are rejecting
  CHECK(f[372] == "ZZ");
  CHECK(f[373] == "11");  // SessionRejectReason = Invalid MsgType
  CHECK(f.count(58) != 0);
  CHECK(h.count("j") == 0);

  // A real FIX 4.4 application type a client session does not implement. The
  // session is fine; we decline the message, and 35=j says exactly that.
  h.initiator.onFrame(p.raw("AE"), 0);  // TradeCaptureReport
  CHECK(h.last("j", f));
  CHECK(f[372] == "AE");
  CHECK(f[380] == "3");  // BusinessRejectReason = Unsupported Message Type
  CHECK(h.count("3") == 1);

  // Both were consumed in sequence: the expectation advanced past them rather
  // than opening a phantom gap.
  CHECK(h.initiator.seqState().expectedIn == 4);
  CHECK(h.count("2") == 0);
}

// (4) Our inbound gap: request a resend, refuse everything above the hole, and
// apply the replay exactly once.
void test_inbound_gap_holds_back_until_closed()
{
  std::printf("test_inbound_gap_holds_back_until_closed\n");
  Harness h;
  Peer p;
  h.logon(p);
  CHECK(h.initiator.seqState().expectedIn == 2);

  // Seq 2 never arrives. Seq 3 does.
  h.initiator.onFrame(p.execReport(7, "0", "100.25", /*seq*/ 3), 0);
  CHECK(h.reports.empty());  // NOT applied: it sits above the hole
  fix::Fields f;
  CHECK(h.last("2", f));
  CHECK(f[7] == "2" && f[16] == "0");
  CHECK(h.initiator.seqState().expectedIn == 2);

  // A second message above the hole does not earn a second ResendRequest
  // inside one HeartBtInt, and is still not applied.
  h.initiator.onFrame(p.execReport(8, "0", "101", /*seq*/ 4), 0);
  CHECK(h.count("2") == 1);
  CHECK(h.reports.empty());

  // The replay closes it, and the held-back messages follow in order.
  h.initiator.onFrame(p.execReport(6, "0", "99.5", /*seq*/ 2, /*possDup*/ true), 0);
  CHECK(h.reports.size() == 1);
  h.initiator.onFrame(p.execReport(7, "0", "100.25", /*seq*/ 3, /*possDup*/ true), 0);
  h.initiator.onFrame(p.execReport(8, "0", "101", /*seq*/ 4, /*possDup*/ true), 0);
  CHECK(h.reports.size() == 3);
  CHECK(h.initiator.seqState().expectedIn == 5);

  const auto* first = std::get_if<fix::ExecutionReport>(&h.reports[0]);
  CHECK(first != nullptr && first->orderId == 6);
  std::string s;
  decwire::append(s, first->price.raw());
  CHECK(s == "99.5");

  // A PossDup whose 34 we already consumed is dropped, not applied twice.
  h.initiator.onFrame(p.execReport(6, "0", "99.5", /*seq*/ 2, /*possDup*/ true), 0);
  CHECK(h.reports.size() == 3);

  // An inbound GapFill advances the expectation over seqs that carry nothing.
  h.initiator.onFrame(p.admin("4", {{123, "Y"}, {36, "9"}}, /*seq*/ 5, /*possDup*/ true), 0);
  CHECK(h.initiator.seqState().expectedIn == 9);
}

// (5) Their ResendRequest: our application messages replay with 43=Y and 122 at
// their original 34, and the seqs the log does not hold gap-fill.
void test_serve_their_resend()
{
  std::printf("test_serve_their_resend\n");
  Harness h;
  Peer p;
  h.logon(p);

  CHECK(h.initiator.submit(order(1, "100.25", "5"), 1 * kSec));  // seq 2
  // An idle Heartbeat at seq 3. The tick lands inside the liveness grace
  // window on purpose, so this is the only admin message in the range and the
  // hole the GapFill covers is exactly one seq wide.
  CHECK(h.initiator.onTick(11 * kSec));
  CHECK(h.count("0") == 1);
  CHECK(h.count("1") == 0);
  CHECK(h.initiator.submit(order(2, "101.5", "5"), 11 * kSec));  // seq 4
  const size_t beforeResend = h.sent.size();

  h.initiator.onFrame(p.admin("2", {{7, "2"}, {16, "0"}}), 11 * kSec);

  // Strict replay order: the order at 2, a GapFill over the Heartbeat at 3,
  // the order at 4.
  CHECK(h.sent.size() == beforeResend + 3);
  fix::Fields f1 = fix::parseFields(h.sent[beforeResend]);
  CHECK(f1[35] == "D" && f1[34] == "2" && f1[43] == "Y");
  CHECK(f1.count(122) != 0);
  CHECK(f1[11] == "1");
  CHECK(f1[44] == "100.25");  // re-encoded and still exact
  CHECK(fix::checksumValid(h.sent[beforeResend]));

  fix::Fields f2 = fix::parseFields(h.sent[beforeResend + 1]);
  CHECK(f2[35] == "4" && f2[34] == "3" && f2[123] == "Y" && f2[36] == "4" && f2[43] == "Y");

  fix::Fields f3 = fix::parseFields(h.sent[beforeResend + 2]);
  CHECK(f3[35] == "D" && f3[34] == "4" && f3[43] == "Y" && f3[11] == "2");

  // A replay consumes no new outbound sequence number: after it, the next
  // message we originate still carries 5.
  CHECK(h.initiator.seqState().nextOut == 5);

  // A range past anything we ever sent replays nothing rather than inventing
  // messages to fill it.
  const size_t afterFirst = h.sent.size();
  h.initiator.onFrame(p.admin("2", {{7, "99"}, {16, "0"}}), 11 * kSec);
  CHECK(h.sent.size() == afterFirst);
}

// (6) Logout, both directions, with the reason where a human can read it.
void test_logout_both_directions()
{
  std::printf("test_logout_both_directions\n");
  {
    Harness h;
    Peer p;
    h.logon(p);
    CHECK(h.initiator.logout("shutting down", 0));
    fix::Fields f;
    CHECK(h.last("5", f));
    CHECK(f[58] == "shutting down");
    CHECK(h.initiator.logout("again", 0));  // idempotent: one Logout per session
    CHECK(h.count("5") == 1);
  }
  {
    Harness h;
    Peer p;
    h.logon(p);
    CHECK(h.initiator.onFrame(p.admin("5", {{58, "venue closing"}}), 0) ==
          fix::FixInitiator::Verdict::Disconnect);
    CHECK(h.count("5") == 1);  // confirmed before hanging up
  }
  {
    // A Logon that is answered with a Logout instead: the session is over and
    // we do not sit waiting for a reply that is not coming.
    Harness h;
    Peer p;
    h.initiator.connect(0);
    CHECK(h.initiator.onFrame(p.admin("5", {{58, "not authorised"}}), 0) ==
          fix::FixInitiator::Verdict::Disconnect);
    CHECK(!h.initiator.loggedOn());
  }
  {
    // No Logon reply at all inside the timeout.
    Harness h;
    h.initiator.connect(0);
    CHECK(h.initiator.onTick(5 * kSec));
    CHECK(!h.initiator.onTick(11 * kSec));
  }
}

// (6b) loggedOn() must go false the moment the session is lost, for every
// way it can be lost -- not just the connect()-time reset. A consumer that
// judges reachability by loggedOn() otherwise keeps sending into a session
// that setSessionDown() already knows is gone. One scenario per path; all
// four end up inside setSessionDown(), which is where the flag is cleared.
void test_logged_on_false_after_session_loss()
{
  std::printf("test_logged_on_false_after_session_loss\n");
  {
    // EOF: the transport saw the peer close the TCP connection with no FIX
    // Logout of its own. FixInitiator has no socket to watch, so whatever
    // detects the close reports it the only way it can -- the same logout()
    // a write failure or an operator stop would call.
    Harness h;
    Peer p;
    h.logon(p);
    CHECK(h.initiator.loggedOn());
    CHECK(h.initiator.logout("read EOF", 0));
    CHECK(!h.initiator.loggedOn());
    CHECK(h.initiator.sessionDown().reason == fix::SessionDownReason::Lost);
    CHECK(h.initiator.sessionDown().text == "read EOF");
  }
  {
    // Write error: a failed send() on the transport, reported the same way.
    Harness h;
    Peer p;
    h.logon(p);
    CHECK(h.initiator.loggedOn());
    CHECK(h.initiator.logout("write failed", 0));
    CHECK(!h.initiator.loggedOn());
    CHECK(h.initiator.sessionDown().reason == fix::SessionDownReason::Lost);
    CHECK(h.initiator.sessionDown().text == "write failed");
  }
  {
    // HeartbeatMissed: our own TestRequest goes unanswered past the second
    // 1.2x grace window. Same nowNs values as test_heartbeat_and_testrequest.
    Harness h;
    Peer p;
    h.logon(p);
    CHECK(h.initiator.loggedOn());
    CHECK(h.initiator.onTick(30 * kSec));   // inbound silence: TestRequest goes out
    CHECK(!h.initiator.onTick(60 * kSec));  // TestRequest unanswered: session ends
    CHECK(!h.initiator.loggedOn());
    CHECK(h.initiator.sessionDown().reason == fix::SessionDownReason::HeartbeatMissed);
  }
  {
    // Logout: the counterparty ends the session on its own initiative.
    Harness h;
    Peer p;
    h.logon(p);
    CHECK(h.initiator.loggedOn());
    CHECK(h.initiator.onFrame(p.admin("5", {{58, "venue closing"}}), 0) ==
          fix::FixInitiator::Verdict::Disconnect);
    CHECK(!h.initiator.loggedOn());
    CHECK(h.initiator.sessionDown().reason == fix::SessionDownReason::Lost);
    CHECK(h.initiator.sessionDown().text == "venue closing");
  }
}

// (7) A sequence number below what we expect is a corrupted session, and a
// message with no 34 at all is not a FIX 4.4 message.
void test_sequence_violations_end_the_session()
{
  std::printf("test_sequence_violations_end_the_session\n");
  {
    Harness h;
    Peer p;
    h.logon(p);
    CHECK(h.initiator.onFrame(p.execReport(1, "0", "100", /*seq*/ 1), 0) ==
          fix::FixInitiator::Verdict::Disconnect);
    fix::Fields f;
    CHECK(h.last("5", f));
    CHECK(f[58].find("too low") != std::string::npos);
    CHECK(h.reports.empty());
  }
  {
    Harness h;
    Peer p;
    h.logon(p);
    std::string b = "35=0";
    b += fix::kSoh;
    b += "49=VENUE";
    b += fix::kSoh;
    b += "56=CLIENT";
    b += fix::kSoh;
    CHECK(h.initiator.onFrame(fix::frame(b), 0) == fix::FixInitiator::Verdict::Disconnect);
    fix::Fields f;
    CHECK(h.last("5", f));
    CHECK(f[58].find("34") != std::string::npos);
  }
  {
    // A bad checksum is dropped without an answer: its 34 cannot be trusted
    // either, so replying would be replying to a number we made up.
    Harness h;
    Peer p;
    h.logon(p);
    std::string msg = p.execReport(1, "0", "100", /*seq*/ 2);
    msg[msg.size() - 2] = msg[msg.size() - 2] == '0' ? '1' : '0';
    const size_t before = h.sent.size();
    CHECK(h.initiator.onFrame(msg, 0) == fix::FixInitiator::Verdict::Handled);
    CHECK(h.sent.size() == before);
    CHECK(h.reports.empty());
    CHECK(h.initiator.seqState().expectedIn == 2);  // the hole stays open
  }
}

// (8) The four inbound message types decode into the right alternative, with
// the fields a client acts on.
void test_inbound_report_types()
{
  std::printf("test_inbound_report_types\n");
  Harness h;
  Peer p;
  h.logon(p);

  h.initiator.onFrame(p.execReport(3, "F", "100.25"), 0);
  CHECK(h.reports.size() == 1);
  const auto* e = std::get_if<fix::ExecutionReport>(&h.reports.back());
  CHECK(e != nullptr && e->execType == fix::ExecType::Trade);
  CHECK(e != nullptr && e->price.raw() == Price::fromDouble(100.25).raw());

  h.initiator.onFrame(p.admin("9", {{37, "3"}, {11, "3"}, {41, "3"}, {39, "8"}, {434, "1"}, {102, "1"}, {58, "UnknownOrder"}}),
                      0);
  const auto* c = std::get_if<fix::CancelReject>(&h.reports.back());
  CHECK(c != nullptr && c->origClOrdId == 3 && c->responseTo == 1 && c->reason == 1);
  CHECK(c != nullptr && c->text == "UnknownOrder");

  h.initiator.onFrame(p.admin("3", {{45, "9"}, {371, "44"}, {372, "D"}, {373, "5"}, {58, "value is incorrect"}}),
                      0);
  const auto* s = std::get_if<fix::SessionReject>(&h.reports.back());
  CHECK(s != nullptr && s->refSeqNum == 9 && s->refTagId == 44 && s->refMsgType == "D");
  CHECK(s != nullptr && s->reason == 5 && s->text == "value is incorrect");

  h.initiator.onFrame(p.admin("j", {{45, "10"}, {372, "G"}, {380, "3"}, {58, "no replace here"}}),
                      0);
  const auto* b = std::get_if<fix::BusinessReject>(&h.reports.back());
  CHECK(b != nullptr && b->refSeqNum == 10 && b->refMsgType == "G" && b->reason == 3);
  CHECK(h.reports.size() == 4);
}

// (9) The sequence sidecar: it round-trips, and every way of damaging it reads
// as absent rather than as a wrong sequence space.
void test_sidecar_round_trip_and_damage()
{
  std::printf("test_sidecar_round_trip_and_damage\n");
  const std::string path = "/tmp/flox_test_fix_initiator_seq_" + std::to_string(::getpid());
  ::unlink(path.c_str());

  fix::FixSeqState out;
  CHECK(!fix::FixInitiatorSidecar::load(path, out));  // absent
  CHECK(out.nextOut == 1 && out.expectedIn == 1);     // and nothing was applied

  const fix::FixSeqState s{4242, 777};
  CHECK(fix::FixInitiatorSidecar::write(path, s));
  CHECK(fix::FixInitiatorSidecar::load(path, out));
  CHECK(out.nextOut == 4242 && out.expectedIn == 777);

  auto readAll = [&path]
  {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  auto writeAll = [&path](const std::string& body)
  {
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    o.write(body.data(), static_cast<std::streamsize>(body.size()));
  };
  const std::string good = readAll();

  // Truncated at every length: a crash mid-write cannot produce a file that
  // loads as a plausible-looking sequence space.
  for (size_t n = 0; n < good.size(); ++n)
  {
    writeAll(good.substr(0, n));
    fix::FixSeqState torn{9, 9};
    CHECK(!fix::FixInitiatorSidecar::load(path, torn));
    CHECK(torn.nextOut == 9 && torn.expectedIn == 9);
  }

  // A single flipped byte anywhere fails the CRC.
  for (size_t i = 0; i < good.size(); ++i)
  {
    std::string bad = good;
    bad[i] = static_cast<char>(bad[i] ^ 0x5A);
    writeAll(bad);
    fix::FixSeqState corrupt{9, 9};
    CHECK(!fix::FixInitiatorSidecar::load(path, corrupt));
  }

  // Zero counters are not a valid FIX 4.4 sequence space even with a good CRC.
  CHECK(fix::FixInitiatorSidecar::write(path, fix::FixSeqState{0, 1}));
  fix::FixSeqState zero{9, 9};
  CHECK(!fix::FixInitiatorSidecar::load(path, zero));

  // Restoring turns the ResetSeqNumFlag off: the point of carrying the
  // counters is to continue the space, not to restart it.
  writeAll(good);
  Harness h;
  fix::FixSeqState restored;
  CHECK(fix::FixInitiatorSidecar::load(path, restored));
  h.initiator.restore(restored);
  h.initiator.connect(0);
  fix::Fields f;
  CHECK(h.last("A", f));
  CHECK(f[34] == "4242");
  CHECK(f.count(141) == 0);
  CHECK(h.initiator.seqState().expectedIn == 777);

  ::unlink(path.c_str());
}

// (10) The router's order sink over a session: submit, cancel and replace go
// out as D / F / G, and a cancel for something that is not working names no
// order rather than inventing one.
void test_routable_executor()
{
  std::printf("test_routable_executor\n");
  Harness h;
  Peer p;
  h.logon(p);

  fix::FixRoutableExecutor exec{h.initiator, /*accountId*/ 42};
  exec.setClock([]
                { return int64_t{0}; });
  h.initiator.setReportHandler(
      [&h, &exec](const fix::InboundReport& r)
      {
        h.reports.push_back(r);
        exec.onReport(r);
      });

  OrderRouter<4> router;
  router.registerExecutor(0, &exec);
  CHECK(router.route(1, Side::BUY, Price::fromDouble(100.25).raw(),
                     Quantity::fromDouble(5).raw(), 77) == RoutingError::Success);

  fix::Fields f;
  CHECK(h.last("D", f));
  CHECK(f[11] == "77");
  CHECK(f[1] == "42");
  CHECK(f[55] == "1");
  CHECK(f[54] == "1");
  CHECK(f[44] == "100.25");
  CHECK(f[38] == "5");
  CHECK(f[40] == "2");
  CHECK(exec.liveOrderCount() == 1);

  exec.replace(77, Price::fromDouble(99.5).raw(), Quantity::fromDouble(3).raw());
  CHECK(h.last("G", f));
  CHECK(f[41] == "77" && f[44] == "99.5" && f[38] == "3");

  exec.cancel(77);
  CHECK(h.last("F", f));
  CHECK(f[41] == "77" && f[55] == "1");

  // A terminal report retires the order, and a later cancel for it writes
  // nothing: a 35=F naming an order the venue closed earns a 35=9 and
  // nothing else.
  h.initiator.onFrame(p.admin("8", {{37, "77"}, {11, "77"}, {55, "1"}, {150, "4"}, {39, "4"}}), 0);
  CHECK(exec.liveOrderCount() == 0);
  const size_t before = h.count("F");
  exec.cancel(77);
  CHECK(h.count("F") == before);

  // A market order carries no 44 -- a limit price of zero would be a different
  // order, not a missing field.
  CHECK(router.route(1, Side::SELL, 0, Quantity::fromDouble(2).raw(), 78) ==
        RoutingError::Success);
  CHECK(h.last("D", f));
  CHECK(f[40] == "1");
  CHECK(f.count(44) == 0);
}

// (11) The decimal contract, in both directions and at the edges.
void test_decimal_exactness()
{
  std::printf("test_decimal_exactness\n");
  const char* values[] = {"100.25", "0.00000001", "1", "99.5", "0.1",
                          "12345678.87654321", "50000", "0.5"};
  for (const char* v : values)
  {
    int64_t raw = 0;
    CHECK(decwire::parse(v, raw));
    fix::NewOrderRequest o;
    o.clOrdId = 1;
    o.symbol = 1;
    o.side = Side::BUY;
    o.price = Price::fromRaw(raw);
    o.quantity = Quantity::fromRaw(raw);
    fix::Fields f = fix::parseFields(fix::ClientCodec::encode(o));
    CHECK(f[44] == v);
    CHECK(f[38] == v);

    // And back through the inbound path at the same value.
    Peer p;
    Harness h;
    h.logon(p);
    h.initiator.onFrame(p.execReport(1, "0", v), 0);
    const auto* e = std::get_if<fix::ExecutionReport>(&h.reports.back());
    CHECK(e != nullptr && e->price.raw() == raw);
    std::string back;
    decwire::append(back, e->price.raw());
    CHECK(back == v);
  }
}

}  // namespace

TEST(FixInitiator, SessionRules)
{
  test_logon_fields_and_heartbeat_adoption();
  test_heartbeat_and_testrequest();
  test_reject_unknown_type();
  test_inbound_gap_holds_back_until_closed();
  test_serve_their_resend();
  test_logout_both_directions();
  test_logged_on_false_after_session_loss();
  test_sequence_violations_end_the_session();
  test_inbound_report_types();
  test_sidecar_round_trip_and_damage();
  test_routable_executor();
  test_decimal_exactness();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
