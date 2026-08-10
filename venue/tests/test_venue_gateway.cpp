/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/fix_codec.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/ouch_codec.h"
#include "flox-venue/rest_json.h"

#include <gtest/gtest.h>

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

std::string fixJoin(std::initializer_list<std::pair<int, std::string>> fields)
{
  std::string s;
  for (const auto& [t, v] : fields)
  {
    s += std::to_string(t) + "=" + v + FixCodec::SOH;
  }
  return s;
}

void test_ouch_roundtrip()
{
  std::printf("test_ouch_roundtrip\n");
  NewOrder o;
  o.id = 42;
  o.symbol = SYM;
  o.side = Side::SELL;
  o.type = OrderType::STOP_LIMIT;
  o.price = px(101.25);
  o.quantity = qty(9);
  o.tif = TimeInForce::IOC;
  o.postOnly = true;
  o.stp = STPMode::CancelBoth;
  o.visibleQuantity = qty(2);
  o.triggerPrice = px(105.5);
  o.trailingOffset = px(0.5);
  o.accountId = 77;
  o.clientOrderId = 12345;
  o.reduceOnly = true;
  o.lastLook = true;
  o.peg = PegRef::Mid;
  o.expiryNs = 1'700'000'000'000;
  o.ocoGroup = 99;
  o.pegOffsetRaw = -250;

  std::vector<uint8_t> buf;
  OuchCodec::encode(InboundCommand{o}, buf);
  auto back = OuchCodec::decode(buf.data(), buf.size());
  CHECK(back.has_value());
  const auto* n = std::get_if<NewOrder>(&*back);
  CHECK(n != nullptr);
  CHECK(n->id == 42 && n->symbol == SYM && n->side == Side::SELL);
  CHECK(n->type == OrderType::STOP_LIMIT && n->tif == TimeInForce::IOC && n->postOnly);
  CHECK(n->stp == STPMode::CancelBoth);
  CHECK(n->price == px(101.25) && n->quantity == qty(9) && n->visibleQuantity == qty(2));
  CHECK(n->triggerPrice == px(105.5) && n->trailingOffset == px(0.5));
  CHECK(n->accountId == 77 && n->clientOrderId == 12345);
  // Derivatives / MM / advanced-order fields survive the wire round-trip.
  CHECK(n->reduceOnly && n->lastLook && n->peg == PegRef::Mid);
  CHECK(n->expiryNs == 1'700'000'000'000 && n->ocoGroup == 99 && n->pegOffsetRaw == -250);

  OuchCodec::encode(InboundCommand{CancelOrder{42, SYM, 77}}, buf);
  auto bc = OuchCodec::decode(buf.data(), buf.size());
  CHECK(bc && std::get_if<CancelOrder>(&*bc) && std::get<CancelOrder>(*bc).id == 42);

  OuchCodec::encode(InboundCommand{ModifyOrder{42, SYM, px(102), qty(4), 77}}, buf);
  auto bm = OuchCodec::decode(buf.data(), buf.size());
  CHECK(bm && std::get_if<ModifyOrder>(&*bm));
  CHECK(std::get<ModifyOrder>(*bm).newPrice == px(102) && std::get<ModifyOrder>(*bm).newQty == qty(4));
}

void test_fix_parse()
{
  std::printf("test_fix_parse\n");
  const std::string d = fixJoin({{35, "D"}, {11, "7"}, {55, "1"}, {54, "1"}, {40, "2"}, {38, "5"}, {44, "100"}, {59, "3"}, {1, "3"}});
  auto cd = FixCodec::decode(d);
  CHECK(cd && std::get_if<NewOrder>(&*cd));
  const auto& n = std::get<NewOrder>(*cd);
  CHECK(n.id == 7 && n.symbol == 1 && n.side == Side::BUY);
  CHECK(n.type == OrderType::LIMIT && n.tif == TimeInForce::IOC);
  CHECK(n.quantity == qty(5) && n.price == px(100) && n.accountId == 3);

  const std::string fcancel = fixJoin({{35, "F"}, {41, "7"}, {55, "1"}, {1, "3"}});
  auto cc = FixCodec::decode(fcancel);
  CHECK(cc && std::get_if<CancelOrder>(&*cc) && std::get<CancelOrder>(*cc).id == 7);

  const std::string g = fixJoin({{35, "G"}, {41, "7"}, {55, "1"}, {44, "101"}, {38, "3"}, {1, "3"}});
  auto cg = FixCodec::decode(g);
  CHECK(cg && std::get_if<ModifyOrder>(&*cg));
  CHECK(std::get<ModifyOrder>(*cg).newPrice == px(101) && std::get<ModifyOrder>(*cg).newQty == qty(3));

  // CheckSum (tag 10) integrity: a correct checksum is accepted, a wrong one is
  // rejected. Checksum = sum of bytes through the SOH before "10=", mod 256.
  const std::string body = fixJoin({{35, "D"}, {11, "8"}, {55, "1"}, {54, "1"}, {38, "5"}, {44, "100"}});
  unsigned sum = 0;
  for (char ch : body)
  {
    sum += static_cast<unsigned char>(ch);
  }
  char cs[8];
  std::snprintf(cs, sizeof cs, "%03u", sum % 256);
  const std::string good = body + "10=" + cs + FixCodec::SOH;
  CHECK(FixCodec::decode(good).has_value());  // valid checksum -> accepted
  const std::string bad = body + "10=999" + FixCodec::SOH;
  CHECK(!FixCodec::decode(bad).has_value() || std::string("999") == cs);  // wrong checksum -> rejected

  // reduce-only via ExecInst=E (DoNotIncrease) must survive -- a derivatives
  // reduce-only order that lost the flag would open a position.
  const std::string ro = fixJoin({{35, "D"}, {11, "9"}, {55, "1"}, {54, "1"}, {38, "5"}, {44, "100"}, {18, "E"}});
  auto cro = FixCodec::decode(ro);
  CHECK(cro && std::get_if<NewOrder>(&*cro) && std::get<NewOrder>(*cro).reduceOnly);
  // Without ExecInst=E, reduceOnly stays false.
  auto cno = FixCodec::decode(fixJoin({{35, "D"}, {11, "9"}, {55, "1"}, {54, "1"}, {38, "5"}, {44, "100"}}));
  CHECK(cno && !std::get<NewOrder>(*cno).reduceOnly);
}

void test_ouch_endtoend()
{
  std::printf("test_ouch_endtoend\n");
  std::vector<uint8_t> reportTags;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   {
                                     std::vector<uint8_t> b;
                                     OuchCodec::encode(e, b);
                                     if (!b.empty()){ reportTags.push_back(b[0]);
} });

  auto submitWire = [&](const InboundCommand& c)
  {
    std::vector<uint8_t> b;
    OuchCodec::encode(c, b);
    auto decoded = OuchCodec::decode(b.data(), b.size());
    CHECK(decoded.has_value());
    eng.submit(*decoded);
  };

  NewOrder s;
  s.id = 1;
  s.symbol = SYM;
  s.side = Side::SELL;
  s.type = OrderType::LIMIT;
  s.price = px(100);
  s.quantity = qty(5);
  submitWire(InboundCommand{s});

  NewOrder b;
  b.id = 2;
  b.symbol = SYM;
  b.side = Side::BUY;
  b.type = OrderType::LIMIT;
  b.price = px(100);
  b.quantity = qty(3);
  b.accountId = 2;
  submitWire(InboundCommand{b});

  bool sawAccepted = false, sawTrade = false, sawExec = false;
  for (uint8_t t : reportTags)
  {
    sawAccepted |= (t == static_cast<uint8_t>(OuchOut::Accepted));
    sawTrade |= (t == static_cast<uint8_t>(OuchOut::Trade));
    sawExec |= (t == static_cast<uint8_t>(OuchOut::Executed));
  }
  CHECK(sawAccepted && sawTrade && sawExec);
}

void test_fix_execreport()
{
  std::printf("test_fix_execreport\n");
  OrderAccepted a{7, SYM, Side::BUY, px(100), qty(5), true};
  const std::string er = FixCodec::encode(OutboundEvent{a});
  CHECK(er.rfind("8=FIX.4.4", 0) == 0);  // starts with BeginString
  CHECK(er.find("\x01"
                "35=8\x01") != std::string::npos);  // ExecutionReport
  CHECK(er.find("\x01"
                "39=0\x01") != std::string::npos);  // OrdStatus New
  CHECK(er.find("\x01"
                "10=") != std::string::npos);  // CheckSum present

  // A fill report carries the fill price (LastPx 31 + AvgPx 6) -- a client must
  // be able to see at what price it traded, not just how much.
  OrderExecuted x{};
  x.id = 7;
  x.symbol = SYM;
  x.lastQty = qty(3);
  x.leavesQty = qty(2);
  x.lastPx = px(100.5);
  const std::string fr = FixCodec::encode(OutboundEvent{x});
  CHECK(fr.find("\x01"
                "150=F\x01") != std::string::npos);  // ExecType Trade
  CHECK(fr.find("\x01"
                "31=") != std::string::npos);  // LastPx present
  CHECK(fr.find("\x01"
                "6=") != std::string::npos);  // AvgPx present
}

void test_rest_json()
{
  std::printf("test_rest_json\n");
  auto cd = RestJson::decode(
      R"({"action":"new","id":7,"symbol":1,"side":"buy","ordType":"limit","qty":5,"price":100.25,"tif":"ioc","postOnly":true})");
  CHECK(cd && std::get_if<NewOrder>(&*cd));
  const auto& n = std::get<NewOrder>(*cd);
  CHECK(n.id == 7 && n.symbol == 1 && n.side == Side::BUY && n.type == OrderType::LIMIT);
  CHECK(n.quantity == qty(5) && n.price == px(100.25) && n.tif == TimeInForce::IOC && n.postOnly);
  CHECK(!n.reduceOnly);  // not requested -> false

  // reduceOnly must be honored for derivatives (else the order would open).
  auto crd = RestJson::decode(
      R"({"action":"new","id":8,"symbol":1,"side":"sell","ordType":"limit","qty":5,"price":100,"reduceOnly":true})");
  CHECK(crd && std::get<NewOrder>(*crd).reduceOnly);

  // self-trade-prevention mode carried over REST.
  auto cst = RestJson::decode(
      R"({"action":"new","id":9,"symbol":1,"side":"buy","ordType":"limit","qty":5,"price":100,"stp":"cancelOldest"})");
  CHECK(cst && std::get<NewOrder>(*cst).stp == STPMode::CancelOldest);

  auto cc = RestJson::decode(R"({"action":"cancel","id":7,"symbol":1,"account":3})");
  CHECK(cc && std::get_if<CancelOrder>(&*cc) && std::get<CancelOrder>(*cc).id == 7);

  auto cm = RestJson::decode(R"({"action":"modify","id":7,"symbol":1,"price":101,"qty":3})");
  CHECK(cm && std::get_if<ModifyOrder>(&*cm) && std::get<ModifyOrder>(*cm).newQty == qty(3));

  // end-to-end: JSON orders -> engine -> JSON events
  std::vector<std::string> out;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { out.push_back(RestJson::encode(e)); });
  eng.submit(*RestJson::decode(
      R"({"action":"new","id":1,"symbol":1,"side":"sell","ordType":"limit","qty":5,"price":100})"));
  eng.submit(*RestJson::decode(
      R"({"action":"new","id":2,"symbol":1,"side":"buy","ordType":"limit","qty":3,"price":100,"account":2})"));
  bool sawTrade = false;
  for (const auto& j : out)
  {
    if (j.find("\"type\":\"trade\"") != std::string::npos)
    {
      sawTrade = true;
    }
  }
  CHECK(sawTrade);
}

}  // namespace

TEST(Gateway, EngineSuite)
{
  test_ouch_roundtrip();
  test_fix_parse();
  test_ouch_endtoend();
  test_fix_execreport();
  test_rest_json();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
