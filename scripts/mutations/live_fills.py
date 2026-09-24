#!/usr/bin/env python3
"""Mutation harness for the live-fill contract on the exchange connectors
(fillPrice on every venue, the per-order fill watermark that de-duplicates
Bybit's two private topics and Bitget's re-pushed order state, the engine's
order id on Bybit fills, Bitget fill size and status, Hyperliquid order events,
and the recvNs / publishTsNs / sourceExchange stamps on the feeds).

A passing test proves nothing on its own; what it has to do is fail when the
code it covers is wrong. This script breaks one piece of the fix at a time, in
the source, and checks that the tests go red -- and that an unmutated tree is
green before and after.

Every run is honest about the build. Per mutation: the file's sha256 is printed
before and after, the anchor has to occur exactly the expected number of times,
the mutated .cpp's object inside the flox-connectors library and the test
target's own objects are deleted so nothing can be served from cache, the
rebuild output has to contain "Building CXX" or the run is refused, and each
test binary runs under a timeout. A mutation that does not compile is not a
mutation and is reported as such.

A mutation that survives its own target is then run against every offline
connector test in the tree (the sweep), so "survived" means no connector test
anywhere notices it.

Usage:

    python3 scripts/mutations/live_fills.py            # control, all mutations, control
    python3 scripts/mutations/live_fills.py --list
    python3 scripts/mutations/live_fills.py --only bybit-execution-fillprice-dropped

The build directory is expected to be configured the way CI configures the
connectors job:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
          -DFLOX_BUILD_CONNECTORS=ON -DFLOX_BUILD_TESTS=ON \
          -DFLOX_ENABLE_POLYMARKET_ORDER_EXECUTOR=OFF -DFLOX_NATIVE=OFF
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build"
TESTS_DIR = BUILD / "connectors" / "tests"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 1800
TEST_TIMEOUT = 120

LIB = "flox-connectors"

BYBIT_CONN = "connectors/src/bybit/bybit_exchange_connector.cpp"
BYBIT_EXEC = "connectors/src/bybit/bybit_order_executor.cpp"
BITGET_CONN = "connectors/src/bitget/bitget_exchange_connector.cpp"
HL_CONN = "connectors/src/hyperliquid/hyperliquid_exchange_connector.cpp"
HL_EXEC = "connectors/src/hyperliquid/hyperliquid_order_executor.cpp"
POLY_CONN = "connectors/src/polymarket/polymarket_exchange_connector.cpp"
WATERMARK = "connectors/include/flox-connectors/execution/fill_watermark.h"

BY = "unit_test_bybit_fill_contract"
BG = "unit_test_bitget_fill_contract"
HL = "unit_test_hyperliquid_fill_contract"
WM = "unit_test_fill_watermark"
BYGAP = "unit_test_bybit_gap"
BYOLD = "unit_test_bybit_private_stream_fills"
POLYTS = "unit_test_polymarket_timestamps"

# Every offline connector test in the tree. A mutation that survives its own
# target is run against all of these before it is called green.
SWEEP = [
    BG,
    "unit_test_bybit_connector_lifecycle",
    BY,
    BYGAP,
    "unit_test_bybit_option_symbol",
    BYOLD,
    WM,
    "unit_test_fixed_point_parse",
    HL,
    "unit_test_ix_ws_client_shutdown",
    "unit_test_ix_ws_client_stop_latency",
    "unit_test_order_serialization_bitget",
    "unit_test_order_serialization_bybit",
    "unit_test_order_serialization_hyperliquid",
    "unit_test_polymarket_delta",
    POLYTS,
]

# Which library objects a mutated file feeds. A header is listed against every
# .cpp that includes it, because the library object is what the test links.
SOURCES_OF = {
    BYBIT_CONN: ["bybit_exchange_connector.cpp"],
    BYBIT_EXEC: ["bybit_order_executor.cpp"],
    BITGET_CONN: ["bitget_exchange_connector.cpp"],
    HL_CONN: ["hyperliquid_exchange_connector.cpp"],
    HL_EXEC: ["hyperliquid_order_executor.cpp"],
    POLY_CONN: ["polymarket_exchange_connector.cpp"],
    WATERMARK: ["bybit_exchange_connector.cpp", "bitget_exchange_connector.cpp"],
}


@dataclass
class Mutation:
    name: str
    why: str
    file: str
    old: str
    new: str
    target: str
    test: str | None = None  # gtest filter; None runs the whole binary
    occurrence: int = 1
    expected_occurrences: int = 1
    extra_targets: list[str] = field(default_factory=list)
    # A mutant proven to compute the same thing as the original on every
    # input. Surviving is then the correct outcome and not a hole: no test can
    # kill it, and one that appeared to would be asserting something other
    # than behaviour. It stays in the list so that the day the code changes
    # shape and the two stop being equivalent, the survival stops being
    # expected -- whoever sees EQUIVALENT here is pointed at the argument in
    # `why` and at the test named there.
    equivalent: bool = False


MUTATIONS: list[Mutation] = [
    # ---- fillPrice, the field no connector used to set ---------------------
    Mutation(
        name="bybit-execution-fillprice-dropped",
        why="the execution topic stops carrying execPrice, so a fill is booked at price 0",
        file=BYBIT_CONN,
        old="""        ev.fillPrice = *priceOpt;
        ev.order.price = *priceOpt;""",
        new="""        ev.order.price = *priceOpt;""",
        target=BY,
        test="BybitFillContract.ExecutionFillCarriesExecPrice",
    ),
    Mutation(
        name="bybit-order-topic-zero-avgprice-written-through",
        why=("the guard that keeps avgPrice \"0\" -- the venue saying it has no execution price "
             "yet -- out of fillPrice is removed, so a parsed zero is written where the unset "
             "default used to stay. EQUIVALENT: Price has no unset state, so the default and a "
             "parsed \"0\" are the same 64 bits and the assignment changes nothing on any "
             "input. Asserted in BybitFillContract."
             "AnUnsetFillPriceIsIndistinguishableFromAParsedZero, which fails the day that stops "
             "being true and the guard becomes observable"),
        file=BYBIT_CONN,
        old="""            if (avgOpt->raw() > 0)
            {
              ev.fillPrice = *avgOpt;
            }""",
        new="""            ev.fillPrice = *avgOpt;""",
        target=BY,
        test=None,
        extra_targets=[BYOLD],
        equivalent=True,
    ),
    Mutation(
        name="watermark-repeated-completion-queues-again",
        why=("complete() stops being idempotent, so a venue re-pushing a terminal frame walks "
             "the order's own history window forward and evicts the entry that window exists to "
             "protect"),
        file=WATERMARK,
        old="""    if (entry.completed)
    {
      return;
    }
""",
        new="",
        target=WM,
        test=None,
        extra_targets=[BG],
    ),
    Mutation(
        name="bybit-order-topic-fillprice-dropped",
        why="the order topic stops carrying avgPrice, so a fill derived from it is booked at price 0",
        file=BYBIT_CONN,
        old="""              ev.fillPrice = *avgOpt;""",
        new="""              (void)avgOpt;""",
        target=BY,
        test="BybitFillContract.OrderTopicFillCarriesAveragePrice",
    ),
    Mutation(
        name="bitget-fillprice-dropped",
        why="the private orders channel stops carrying fillPrice",
        file=BITGET_CONN,
        old="""            ev.fillPrice = *fillPriceOpt;""",
        new="""            (void)fillPriceOpt;""",
        target=BG,
        test="BitgetFillContract.*",
    ),
    Mutation(
        name="hyperliquid-fillprice-dropped",
        why="the inline fill from the submit response stops carrying avgPx",
        file=HL_EXEC,
        old="""  ev.fillQty = fillQty;
  ev.fillPrice = fillPrice;""",
        new="""  ev.fillQty = fillQty;
  (void)fillPrice;""",
        target=HL,
        test="HyperliquidFillContract.InlineFillReachesTheBus",
    ),

    # ---- the watermark itself ---------------------------------------------
    Mutation(
        name="watermark-advance-does-not-store",
        why=("advance() returns the increment but never stores the new total, so the second "
             "channel to report an execution is measured against zero and publishes it again"),
        file=WATERMARK,
        old="""    entry.cumulative = cumulative;
    return Quantity::fromRaw(increment);""",
        new="""    return Quantity::fromRaw(increment);""",
        target=WM,
        test="FillWatermark.*",
    ),
    Mutation(
        name="bybit-second-topic-publishes-anyway",
        why=("applyFillWatermark stops suppressing a fill that added no quantity, so both "
             "private topics publish the same execution"),
        file=BYBIT_CONN,
        old="""  const bool isFill =
      (ev.status == OrderEventStatus::PARTIALLY_FILLED || ev.status == OrderEventStatus::FILLED);
  return !isFill || !ev.fillQty.isZero();""",
        new="""  return true;""",
        target=BY,
        test="BybitFillContract.OrderAndExecutionFramesForOneFillDispatchOneFill",
    ),
    Mutation(
        name="bybit-increment-published-as-cumulative",
        why=("the watermark still de-duplicates, but the event carries the order's cumulative "
             "filled quantity as fillQty instead of the increment"),
        file=BYBIT_CONN,
        old="""  ev.fillQty = _reportedFill.advance(ev.order.id, cumulative);""",
        new="""  ev.fillQty = _reportedFill.advance(ev.order.id, cumulative).isZero() ? Quantity{} : cumulative;""",
        target=BY,
        test="BybitFillContract.OrderAndExecutionFramesForOneFillDispatchOneFill",
    ),
    Mutation(
        name="watermark-entry-evicted-at-completion",
        why=("complete() erases the order instead of queueing it, so the duplicate of the last "
             "fill -- which arrives after the terminal status -- is published a second time"),
        file=WATERMARK,
        old="""    auto& entry = _entries[id];
    if (entry.completed)
    {
      return;
    }
    entry.completed = true;
    _completed.push_back(id);""",
        new="""    _entries.erase(id);""",
        target=WM,
        test="FillWatermark.ACompletedOrderStillRejectsItsDuplicate",
    ),
    Mutation(
        name="watermark-eviction-bound-removed",
        why="completed orders are never evicted, so the map grows for the life of the process",
        file=WATERMARK,
        old="""    while (_completed.size() > _historySize)
    {
      _entries.erase(_completed.front());
      _completed.pop_front();
    }
""",
        new="",
        target=WM,
        test="FillWatermark.EvictsCompletedOrdersOnceTheHistoryIsFull",
    ),

    # ---- Bitget --------------------------------------------------------------
    Mutation(
        name="bitget-partially-filled-back-to-submitted",
        why="partially_filled falls into the else again and reaches the engine as SUBMITTED",
        file=BITGET_CONN,
        old="""        else if (status == "partially_filled")
        {
          // Used to fall into the else below and reach the engine as
          // SUBMITTED, so a partial fill dispatched onOrderSubmitted and the
          // position never moved.
          ev.status = OrderEventStatus::PARTIALLY_FILLED;
        }
        else if (status == "canceled" || status == "cancelled")""",
        new="""        else if (status == "canceled" || status == "cancelled")""",
        target=BG,
        test="BitgetFillContract.PartialFillCarriesQtyPriceAndStatus",
    ),
    Mutation(
        name="bitget-fillqty-zeroed",
        why="the published order event carries fillQty 0, so no position moves",
        file=BITGET_CONN,
        old="""        ev.publishNs = nowMonoNanos();
        _orderBus->publish(std::move(ev));""",
        new="""        ev.fillQty = Quantity{};
        ev.publishNs = nowMonoNanos();
        _orderBus->publish(std::move(ev));""",
        target=BG,
        test="BitgetFillContract.*",
    ),
    Mutation(
        name="bitget-basevolume-not-parsed",
        why=("baseVolume (the size of the latest fill) is no longer read; the cumulative "
             "watermark increment is the only source of fillQty left"),
        file=BITGET_CONN,
        old="""            ev.fillQty = *fillQtyOpt;""",
        new="""            (void)fillQtyOpt;""",
        target=BG,
        test="BitgetFillContract.*",
    ),
    Mutation(
        name="bitget-watermark-fillqty-fallback-removed",
        why=("the watermark increment no longer fills in for a missing baseVolume, so a fill "
             "push without it publishes quantity 0"),
        file=BITGET_CONN,
        old="""          if (ev.fillQty.isZero())
          {
            ev.fillQty = newlyFilled;
          }""",
        new="""          (void)newlyFilled;""",
        target=BG,
        test="BitgetFillContract.*",
    ),
    Mutation(
        name="bitget-repush-booked-twice",
        why=("the cumulative de-duplication is removed, so the order state the venue re-pushes "
             "after a private resubscribe books the same fill a second time"),
        file=BITGET_CONN,
        old="""        const bool isFill = (ev.status == OrderEventStatus::FILLED ||
                             ev.status == OrderEventStatus::PARTIALLY_FILLED);
        if (isFill && haveCumulative)
        {
          // A push that carries no new cumulative quantity is the venue
          // repeating itself, not a second execution.
          const Quantity newlyFilled = _reportedFill.advance(ev.order.id, ev.order.filledQuantity);
          if (newlyFilled.isZero())
          {
            continue;
          }
          if (ev.fillQty.isZero())
          {
            ev.fillQty = newlyFilled;
          }
        }""",
        new="""        (void)haveCumulative;""",
        target=WM,
        test="BitgetFillContractExtra.RepushedOrderStateDoesNotBookASecondFill",
        extra_targets=[BG],
    ),

    # ---- Hyperliquid order events ------------------------------------------
    Mutation(
        name="hyperliquid-error-status-not-published",
        why=("statuses[0].error is not parsed again, so a venue rejection is recorded as a "
             "submitted order and no REJECTED event reaches the bus"),
        file=HL_EXEC,
        old="""        if (std::string_view reason = asString(s0["error"]); !reason.empty())
        {
          // The order never came into existence at the venue, so it is not
          // handed to the tracker at all -- same convention as the Bybit
          // executor, whose rejected submits leave no tracker record either.
          publishRejection(order, std::string(reason));
          return;
        }
""",
        new="",
        target=HL,
        test="HyperliquidFillContract.VenueRejectionReachesTheBusWithItsReason",
    ),
    Mutation(
        name="hyperliquid-reject-reason-dropped",
        why="the rejection is published, but the venue's own reason text is replaced by a generic one",
        file=HL_EXEC,
        old="""          publishRejection(order, std::string(reason));""",
        new="""          publishRejection(order, std::string("Venue rejected the order"));""",
        target=HL,
        test="HyperliquidFillContract.VenueRejectionReachesTheBusWithItsReason",
    ),
    Mutation(
        name="hyperliquid-transport-error-only-logged",
        why=("a failed submit round-trip is logged and nothing else -- the strategy is never "
             "told the order does not exist"),
        file=HL_EXEC,
        old="""        publishRejection(order, std::string("Transport error: ") + std::string(err));""",
        new="""        FLOX_LOG_ERROR("[HL] submit error: " << err);""",
        target=HL,
        test=None,
    ),

    # ---- sourceExchange on the book feeds ----------------------------------
    Mutation(
        name="bybit-book-sourceexchange-dropped",
        why="Bybit book events go out with InvalidExchangeId, so CompositeBookMatrix drops them",
        file=BYBIT_CONN,
        old="""      ev->recvNs = MonoNanos::fromRaw(recvNs);
      ev->sourceExchange = _exchangeId;""",
        new="""      ev->recvNs = MonoNanos::fromRaw(recvNs);""",
        target=BYGAP,
        test=None,
    ),
    Mutation(
        name="bitget-book-sourceexchange-dropped",
        why="Bitget book events go out with InvalidExchangeId",
        file=BITGET_CONN,
        old="""      ev->recvNs = MonoNanos::fromRaw(recvNs);
      ev->sourceExchange = _exchangeId;""",
        new="""      ev->recvNs = MonoNanos::fromRaw(recvNs);""",
        target=BG,
        test="BitgetFeedContract.*",
    ),
    Mutation(
        name="hyperliquid-book-sourceexchange-dropped",
        why="Hyperliquid book events go out with InvalidExchangeId",
        file=HL_CONN,
        old="""      ev->recvNs = MonoNanos::fromRaw(recvNs);
      ev->sourceExchange = _exchangeId;""",
        new="""      ev->recvNs = MonoNanos::fromRaw(recvNs);""",
        target=HL,
        test="HyperliquidFeedContract.*",
    ),
    Mutation(
        name="polymarket-book-sourceexchange-dropped",
        why="Polymarket snapshots go out with InvalidExchangeId",
        file=POLY_CONN,
        old="""  ev->recvNs = MonoNanos::fromRaw(recvNs);
  ev->sourceExchange = _exchangeId;
  ev->update.symbol = sym;""",
        new="""  ev->recvNs = MonoNanos::fromRaw(recvNs);
  ev->update.symbol = sym;""",
        target=POLYTS,
        test=None,
    ),

    # ---- the registry registration that resolves that id -------------------
    Mutation(
        name="bybit-registry-registration-removed",
        why="the connector never registers itself, so its exchange id stays InvalidExchangeId",
        file=BYBIT_CONN,
        old="""  if (_registry)
  {
    _exchangeId = _registry->registerExchange("bybit");
  }
""",
        new="",
        target=BY,
        test=None,
    ),
    Mutation(
        name="bitget-registry-registration-removed",
        why="the connector never registers itself, so its exchange id stays InvalidExchangeId",
        file=BITGET_CONN,
        old="""  if (_registry)
  {
    _exchangeId = _registry->registerExchange("bitget");
  }
""",
        new="",
        target=BG,
        test="BitgetFeedContract.*",
    ),

    # ---- recvNs / publishTsNs ----------------------------------------------
    Mutation(
        name="bitget-book-recvns-dropped",
        why="a Bitget book event carries recvNs 0, so checkStaleness skips the venue forever",
        file=BITGET_CONN,
        old="""      ev->recvNs = MonoNanos::fromRaw(recvNs);
      ev->sourceExchange = _exchangeId;""",
        new="""      ev->sourceExchange = _exchangeId;""",
        target=BG,
        test="BitgetFeedContract.*",
    ),
    Mutation(
        name="hyperliquid-book-recvns-dropped",
        why="a Hyperliquid book event carries recvNs 0",
        file=HL_CONN,
        old="""      ev->recvNs = MonoNanos::fromRaw(recvNs);
      ev->sourceExchange = _exchangeId;""",
        new="""      ev->sourceExchange = _exchangeId;""",
        target=HL,
        test="HyperliquidFeedContract.*",
    ),
    Mutation(
        name="bitget-order-recvns-dropped",
        why="a Bitget order event carries recvNs 0, so its arrival is untimed",
        file=BITGET_CONN,
        old="""        OrderEvent ev;
        ev.recvNs = MonoNanos::fromRaw(recvNs);""",
        new="""        OrderEvent ev;
        (void)recvNs;""",
        target=BG,
        test="BitgetFillContract.*",
    ),
    Mutation(
        name="bitget-book-publishts-dropped",
        why="a Bitget book event is published without publishTsNs",
        file=BITGET_CONN,
        old="""        ev->publishTsNs = nowMonoNanos();
        auto [res, _] = _bookUpdateBus->tryPublish(std::move(ev));""",
        new="""        auto [res, _] = _bookUpdateBus->tryPublish(std::move(ev));""",
        target=BG,
        test="BitgetFeedContract.*",
    ),
    Mutation(
        name="hyperliquid-book-publishts-dropped",
        why="a Hyperliquid book event is published without publishTsNs",
        file=HL_CONN,
        old="""        ev->publishTsNs = nowMonoNanos();
        auto [res, _] = _bookBus->tryPublish(std::move(ev));""",
        new="""        auto [res, _] = _bookBus->tryPublish(std::move(ev));""",
        target=HL,
        test="HyperliquidFeedContract.*",
    ),

    # ---- order identity on Bybit -------------------------------------------
    Mutation(
        name="bybit-orderlinkid-not-sent",
        why="order/create stops carrying the engine order id, so fills come back under the venue's id",
        file=BYBIT_EXEC,
        old="""  body.append("\\"orderLinkId\\":\\"").append(std::to_string(order.id)).append("\\",");
""",
        new="",
        target=BY,
        test="BybitFillContract.SubmitSendsEngineOrderIdAsOrderLinkId",
    ),
    Mutation(
        name="bybit-order-topic-id-from-venue-orderid",
        why="the order topic keys the event off the venue's orderId again",
        file=BYBIT_CONN,
        old="""        ev.order.id = static_cast<OrderId>(linkIdOpt ? *linkIdOpt : *orderIdOpt);""",
        new="""        ev.order.id = static_cast<OrderId>(orderIdOpt ? *orderIdOpt : *linkIdOpt);""",
        target=BY,
        test="BybitFillContract.FillCarriesEngineOrderIdFromOrderLinkId",
        occurrence=1,
        expected_occurrences=2,
    ),
    Mutation(
        name="bybit-execution-topic-id-from-venue-orderid",
        why="the execution topic keys the event off the venue's orderId again",
        file=BYBIT_CONN,
        old="""        ev.order.id = static_cast<OrderId>(linkIdOpt ? *linkIdOpt : *orderIdOpt);""",
        new="""        ev.order.id = static_cast<OrderId>(orderIdOpt ? *orderIdOpt : *linkIdOpt);""",
        target=BY,
        test="BybitFillContract.FillCarriesEngineOrderIdFromOrderLinkId",
        occurrence=2,
        expected_occurrences=2,
    ),
    Mutation(
        name="bybit-order-topic-foreign-order-dropped",
        why=("the order topic drops any frame without an orderLinkId instead of falling back to "
             "the venue id, so fills for orders placed outside this engine vanish"),
        file=BYBIT_CONN,
        old="""        auto linkIdOpt = parseOrderLinkId(d);
        if (!linkIdOpt && !orderIdOpt)
        {
          _logger->warn("[Bybit] Invalid orderId in order event");
          continue;
        }""",
        new="""        auto linkIdOpt = parseOrderLinkId(d);
        if (!linkIdOpt)
        {
          _logger->warn("[Bybit] Invalid orderId in order event");
          continue;
        }""",
        target=BY,
        test=None,
    ),
    Mutation(
        name="bybit-execution-topic-foreign-order-dropped",
        why=("same on the execution topic: a fill for an order this engine did not place is "
             "dropped rather than published under the venue id"),
        file=BYBIT_CONN,
        old="""        auto linkIdOpt = parseOrderLinkId(d);
        if (!linkIdOpt && !orderIdOpt)
        {
          _logger->warn("[Bybit] Invalid orderId in execution event");
          continue;
        }""",
        new="""        auto linkIdOpt = parseOrderLinkId(d);
        if (!linkIdOpt)
        {
          _logger->warn("[Bybit] Invalid orderId in execution event");
          continue;
        }""",
        target=BY,
        test=None,
    ),

    # ---- adversarial -------------------------------------------------------
    Mutation(
        name="adv-bybit-watermark-keyed-by-symbol",
        why=("the per-order watermark becomes a per-symbol watermark: two orders live on one "
             "symbol share a cumulative total, so one order's fill cancels the other's"),
        file=BYBIT_CONN,
        old="""  ev.fillQty = _reportedFill.advance(ev.order.id, cumulative);

  const bool terminal =
      (ev.status == OrderEventStatus::FILLED || ev.status == OrderEventStatus::CANCELED ||
       ev.status == OrderEventStatus::REJECTED || ev.status == OrderEventStatus::EXPIRED);
  if (terminal)
  {
    _reportedFill.complete(ev.order.id);
  }""",
        new="""  ev.fillQty = _reportedFill.advance(static_cast<OrderId>(ev.order.symbol), cumulative);

  const bool terminal =
      (ev.status == OrderEventStatus::FILLED || ev.status == OrderEventStatus::CANCELED ||
       ev.status == OrderEventStatus::REJECTED || ev.status == OrderEventStatus::EXPIRED);
  if (terminal)
  {
    _reportedFill.complete(static_cast<OrderId>(ev.order.symbol));
  }""",
        target=BY,
        test=None,
    ),
    Mutation(
        name="adv-bybit-execution-cumulative-ignored",
        why=("the execution topic stops using orderQty - leavesQty and always treats execQty as "
             "an increment, so a replayed execution frame is booked twice"),
        file=BYBIT_CONN,
        old="""        const Quantity cumulative =
            haveCumulative
                ? ev.order.filledQuantity
                : Quantity::fromRaw(_reportedFill.reported(ev.order.id).raw() + qtyOpt->raw());""",
        new="""        (void)haveCumulative;
        const Quantity cumulative =
            Quantity::fromRaw(_reportedFill.reported(ev.order.id).raw() + qtyOpt->raw());""",
        target=BY,
        test=None,
    ),
    Mutation(
        name="adv-bitget-recvns-stamped-after-publishts",
        why=("recvNs is re-stamped at publish time, after parsing and after publishTsNs, so it "
             "measures the wrong instant and the feed always looks fresher than it is"),
        file=BITGET_CONN,
        old="""        ev->publishTsNs = nowMonoNanos();
        auto [res, _] = _bookUpdateBus->tryPublish(std::move(ev));""",
        new="""        ev->publishTsNs = nowMonoNanos();
        ev->recvNs = MonoNanos::fromRaw(nowNsMonotonic());
        auto [res, _] = _bookUpdateBus->tryPublish(std::move(ev));""",
        target=BG,
        test="BitgetFeedContract.*",
    ),
    Mutation(
        name="adv-bitget-completion-only-on-filled",
        why=("a cancelled order is never marked complete, so its watermark entry is never "
             "eligible for eviction and the map grows without bound"),
        file=BITGET_CONN,
        old="""        if (ev.status == OrderEventStatus::FILLED || ev.status == OrderEventStatus::CANCELED)
        {
          _reportedFill.complete(ev.order.id);
        }""",
        new="""        if (ev.status == OrderEventStatus::FILLED)
        {
          _reportedFill.complete(ev.order.id);
        }""",
        target=BG,
        test=None,
    ),
    Mutation(
        name="adv-hyperliquid-fill-leaves-filledquantity-unset",
        why=("the fill event carries fillQty but the order it carries still says nothing has "
             "filled, so anything reading order.filledQuantity sees zero"),
        file=HL_EXEC,
        old="""  ev.order.filledQuantity = fillQty;
  ev.fillQty = fillQty;""",
        new="""  ev.fillQty = fillQty;""",
        target=HL,
        test=None,
    ),
]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_occurrence(text: str, old: str, new: str, occurrence: int, expected: int) -> str:
    count = text.count(old)
    if count != expected:
        raise SystemExit(
            f"mutation anchor found {count} time(s), expected {expected}:\n  {old!r}"
        )
    start = -1
    for _ in range(occurrence):
        start = text.index(old, start + 1)
    return text[:start] + new + text[start + len(old):]


def find_objects(pattern_path: str, name: str | None = None) -> list[Path]:
    cmd = ["find", str(BUILD), "-type", "f", "-name", name or "*.o", "-path", pattern_path]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.split()
    return [Path(p) for p in out]


def target_objects(target: str) -> list[Path]:
    """The test target's own object files, located the way `find` would."""
    return find_objects(f"*{target}.dir*")


def library_objects(source_file: str) -> list[Path]:
    """The mutated .cpp's object inside the flox-connectors library.

    Connector sources do not compile into the test binary: they compile into
    libflox-connectors.a, so the library object is what has to go.
    """
    objects: list[Path] = []
    for src in SOURCES_OF[source_file]:
        objects += find_objects(f"*{LIB}.dir*", f"{src}.o")
    return objects


def delete_objects(mutation_file: str, targets: list[str]) -> int:
    removed = 0
    for obj in library_objects(mutation_file):
        obj.unlink()
        removed += 1
    for target in targets:
        for obj in target_objects(target):
            obj.unlink()
            removed += 1
    return removed


def rebuild(targets: list[str]) -> tuple[bool, str]:
    """Rebuilds the connector library and the test targets that link it."""
    result = subprocess.run(
        [CMAKE, "--build", str(BUILD), "--target", LIB, *targets, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        return False, output
    if "Building CXX" not in output:
        raise SystemExit(
            "the rebuild compiled nothing -- the result would have been a stale "
            f"binary, so the run is refused:\n{output[-2000:]}"
        )
    return True, output


def run_test(target: str, gtest_filter: str | None) -> tuple[int, str]:
    cmd = [str(TESTS_DIR / target)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=TEST_TIMEOUT)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def control(targets: list[str]) -> bool:
    ok = True
    for target in targets:
        # Deleted first so the control binary is compiled from the source as it
        # stands right now, not served from whatever the last run left behind.
        for obj in target_objects(target):
            obj.unlink()
    built, output = rebuild(targets)
    if not built:
        raise SystemExit(f"control rebuild failed:\n{output[-4000:]}")
    for target in targets:
        code, out = run_test(target, None)
        state = "green" if code == 0 else "RED"
        summary = next((line for line in out.splitlines()
                        if line.startswith("[==========] ") and " ran." in line), "")
        print(f"  control {target:<42} {state}   {summary.strip()}")
        ok = ok and code == 0
    return ok


def run_mutation(m: Mutation) -> tuple[str, str]:
    """Returns (verdict, detail). Verdict is RED, GREEN or NOCOMPILE."""
    path = REPO / m.file
    original = path.read_text()
    before = sha256(path)
    primary_targets = [m.target, *m.extra_targets]

    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    print(f"  file    {m.file}")
    print(f"  sha256  before  {before}")

    mutated = replace_occurrence(original, m.old, m.new, m.occurrence, m.expected_occurrences)
    if mutated == original:
        raise SystemExit("mutation changed nothing")
    path.write_text(mutated)
    print(f"  sha256  mutated {sha256(path)}")

    verdict = "GREEN"
    detail = ""
    try:
        removed = delete_objects(m.file, primary_targets)
        print(f"  removed {removed} object file(s): the library object for the mutated source "
              f"and the objects of {', '.join(primary_targets)}")

        built, output = rebuild(primary_targets)
        if not built:
            verdict = "NOCOMPILE"
            tail = [ln for ln in output.splitlines() if "error" in ln.lower()][:4]
            detail = " | ".join(tail) or output.splitlines()[-1]
            print(f"  DID NOT COMPILE: {detail}")
            return verdict, detail
        compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
        print(f"  rebuilt {LIB} + {', '.join(primary_targets)}: "
              f"{compiled} 'Building CXX' line(s)")

        shown = m.test if m.test else "(whole binary)"
        killed_by = []
        for target in primary_targets:
            code, out = run_test(target, m.test)
            print(f"  {target} --gtest_filter={shown} -> exit {code} "
                  f"({'RED' if code else 'green'})")
            if code != 0:
                killed_by.append(target)

        if killed_by:
            verdict = "RED"
            detail = f"killed by {', '.join(killed_by)}"
        else:
            # Nothing its own target noticed. Every other offline connector test
            # gets a turn before this is called a hole in the suite.
            print("  survived its own target; sweeping every offline connector test")
            for obj_target in SWEEP:
                for obj in target_objects(obj_target):
                    obj.unlink()
            built, output = rebuild(SWEEP)
            if not built:
                verdict = "NOCOMPILE"
                detail = "sweep rebuild failed"
                print(f"  DID NOT COMPILE in the sweep:\n{output[-2000:]}")
                return verdict, detail
            sweep_killed = []
            for target in SWEEP:
                code, out = run_test(target, None)
                if code != 0:
                    sweep_killed.append(target)
            if sweep_killed:
                verdict = "RED"
                detail = f"killed by {', '.join(sweep_killed)} (sweep)"
                print(f"  sweep: RED in {', '.join(sweep_killed)}")
            elif m.equivalent:
                verdict = "EQUIVALENT"
                detail = "no observable difference; see `why`"
                print(f"  sweep: all {len(SWEEP)} connector test binaries GREEN "
                      f"-- EXPECTED, this mutant is equivalent")
            else:
                verdict = "GREEN"
                detail = f"{len(SWEEP)} connector test binaries all green"
                print(f"  sweep: all {len(SWEEP)} connector test binaries GREEN "
                      f"-- MUTATION SURVIVED")
        return verdict, detail
    finally:
        path.write_text(original)
        after = sha256(path)
        print(f"  sha256  after   {after}")
        if after != before:
            raise SystemExit("restore failed: the file does not hash back to its original")
        delete_objects(m.file, primary_targets)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.name:<48} {m.target:<40} {m.test or '(whole binary)'}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(
            f"{BUILD} is not configured; run\n"
            "  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo "
            "-DFLOX_BUILD_CONNECTORS=ON -DFLOX_BUILD_TESTS=ON "
            "-DFLOX_ENABLE_POLYMARKET_ORDER_EXECUTOR=OFF -DFLOX_NATIVE=OFF"
        )

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")
    control_targets = sorted({t for m in selected for t in [m.target, *m.extra_targets]})

    print("control run before the mutations")
    if not control(control_targets):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, *run_mutation(m)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(control_targets)

    print("\nsummary")
    print(f"  {'verdict':<10} {'mutation':<48} {'target':<40} detail")
    for m, verdict, detail in results:
        print(f"  {verdict:<10} {m.name:<48} {m.target:<40} {detail}")

    survived = [m.name for m, verdict, _ in results if verdict == "GREEN"]
    equivalent = [m.name for m, verdict, _ in results if verdict == "EQUIVALENT"]
    unexpectedly_killed = [m.name for m, verdict, _ in results
                           if m.equivalent and verdict == "RED"]
    broken = [m.name for m, verdict, _ in results if verdict == "NOCOMPILE"]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived every connector test: "
              f"{', '.join(survived)}")
    if equivalent:
        print(f"\n{len(equivalent)} mutation(s) survived because they are equivalent, which is "
              f"the expected outcome: {', '.join(equivalent)}")
    if unexpectedly_killed:
        print(f"\n{len(unexpectedly_killed)} mutation(s) marked equivalent were killed -- the "
              f"code no longer matches the argument in `why`: {', '.join(unexpectedly_killed)}")
    if broken:
        print(f"\n{len(broken)} mutation(s) did not compile and prove nothing: "
              f"{', '.join(broken)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and not broken and restored else 1


if __name__ == "__main__":
    sys.exit(main())
