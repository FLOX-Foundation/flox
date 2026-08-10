/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/control_api.h"
#include "flox-venue/control_plane.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"

#include <string>

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
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

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig mk(SymbolId id)
{
  SymbolConfig c;
  c.id = id;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}

NewOrder limit(OrderId id, SymbolId sym, Side s, double p, double q)
{
  NewOrder o;
  o.id = id;
  o.symbol = sym;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  return o;
}

}  // namespace

TEST(ControlPlane, EngineSuite)
{
  std::printf("test_control_plane\n");
  InstrumentRegistry reg;
  CHECK(reg.listInstrument(mk(1)));
  CHECK(reg.listInstrument(mk(2)));
  CHECK(!reg.listInstrument(mk(1)));  // duplicate id rejected
  CHECK(reg.size() == 2);
  CHECK(reg.has(1) && reg.get(2) != nullptr);
  CHECK(reg.get(2)->tickSize == px(0.01));

  CHECK(reg.setPriceBand(1, px(90), px(110)));
  CHECK(reg.get(1)->minPrice == px(90));

  CHECK(reg.halt(1, true));
  CHECK(reg.get(1)->halted);
  CHECK(reg.setTriggerRef(2, TriggerRef::Mark));
  CHECK(reg.get(2)->triggerRef == TriggerRef::Mark);

  // Halt applied to a live shard: orders on a halted instrument are rejected.
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng(*reg.get(1), [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  eng.setHalted(true);
  eng.submit(limit(1, 1, Side::SELL, 100, 5));
  bool rejected = false;
  for (auto& e : ev)
  {
    if (auto* r = std::get_if<OrderRejected>(&e); r && r->reason == RejectReason::Halted)
    {
      rejected = true;
    }
  }
  CHECK(rejected);

  // Resume: trading works again.
  ev.clear();
  eng.setHalted(false);
  eng.submit(limit(1, 1, Side::SELL, 100, 5));
  eng.submit(limit(2, 1, Side::BUY, 100, 3));
  bool traded = false;
  for (auto& e : ev)
  {
    if (std::get_if<Trade>(&e))
    {
      traded = true;
    }
  }
  CHECK(traded);

  // Control-plane API (the gRPC/MCP surface): JSON request -> JSON result.
  std::printf("test_control_api\n");
  InstrumentRegistry reg2;
  ControlApi api(reg2);
  auto has = [](const std::string& s, const char* sub)
  { return s.find(sub) != std::string::npos; };
  CHECK(has(api.handle(R"({"method":"listInstrument","symbol":1,"tick":0.01,"minPrice":50,"maxPrice":150})"),
            "\"ok\":true"));
  CHECK(has(api.handle(R"({"method":"listInstrument","symbol":2,"tick":0.01})"), "\"ok\":true"));
  CHECK(has(api.handle(R"({"method":"listInstrument","symbol":1})"), "exists"));
  CHECK(has(api.handle(R"({"method":"halt","symbol":1,"halted":true})"), "\"ok\":true"));
  CHECK(reg2.get(1)->halted);
  CHECK(has(api.handle(R"({"method":"get","symbol":1})"), "\"halted\":true"));
  CHECK(has(api.handle(R"({"method":"setTriggerRef","symbol":2,"ref":"mark"})"), "\"ok\":true"));
  CHECK(reg2.get(2)->triggerRef == TriggerRef::Mark);
  CHECK(has(api.handle(R"({"method":"list"})"), "\"instruments\":["));
  CHECK(has(api.handle(R"({"method":"bogus"})"), "unknown_method"));
  CHECK(has(api.handle(R"({"method":"halt","symbol":99,"halted":true})"), "unknown_symbol"));

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
