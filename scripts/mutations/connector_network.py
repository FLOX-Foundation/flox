#!/usr/bin/env python3
"""Mutation harness for the connector network, limiter, precision and expiry fixes.

The fix under test spans five pieces:

  * CurlTransport performs REST requests off the calling thread, with
    millisecond timeouts and NOSIGNAL, through a bounded handoff queue whose
    overflow answers onError (connectors/src/net/curl_transport.cpp,
    connectors/include/flox-connectors/net/curl_transport.h);
  * ActiveRateLimitPolicy::gate(orderId, action, onRejected) replaces
    tryAcquire: a token sends inline, REJECT/CALLBACK answer onRejected, and
    WAIT defers onto the policy's own sender thread, re-checking the bucket
    after every sleep (connectors/include/flox-connectors/execution/executor_policies.h);
  * every send path of the Hyperliquid and Bitget executors goes through that
    gate, and a refusal is published as REJECTED_RATE_LIMIT;
  * Bitget formats trigger and limit prices at the instrument's own precision
    (decimalsForTick from SymbolInfo::tickSize) and sends STOP_LIMIT /
    TAKE_PROFIT_LIMIT as a normal_plan with orderType "limit" and a price;
  * a Bybit option expiry is parsed from ASCII tables through
    std::chrono::sys_days, and Decimal::toString formats the raw integer
    instead of going through std::to_string(double).

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks each piece one at a time and
checks that the test written for it goes red -- and that an unmutated tree is
green before and after.

Two test surfaces answer for every mutation:

    the expected-red filter   one gtest filter on one binary (fast)
    the whole binary          the same binary with no filter

A mutation that stays green against its filter is re-run against the whole
binary before it is called a survivor, and every survivor is then swept
across every related binary -- the six acceptance binaries, test_decimal,
test_decimal_locale, and every other offline connector binary that links the
changed executors -- in case something else catches it.

Every run is honest about the build: the mutated file's hash is printed before
and after, every object file that depends on the mutated file (found through
the compiler's own .o.d dependency files, via find) is deleted so nothing is
served from cache, the rebuild output has to contain "Building CXX" or the run
is refused, and every build and test runs under a timeout. A mutation that
does not compile is not a mutation.

Usage:

    python3 scripts/mutations/connector_network.py             # control, mutations, sweep, control
    python3 scripts/mutations/connector_network.py --list
    python3 scripts/mutations/connector_network.py --only wait-sends-inline
    python3 scripts/mutations/connector_network.py --skip-sweep

The build directory is expected to be configured already:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
          -DFLOX_BUILD_CONNECTORS=ON -DFLOX_BUILD_TESTS=ON \\
          -DFLOX_ENABLE_POLYMARKET_ORDER_EXECUTOR=OFF -DFLOX_NATIVE=OFF

The timing binaries (unit_test_curl_transport_timeout,
unit_test_rate_limit_wait_policy) bind loopback listeners and assert on
elapsed milliseconds; everything here runs one binary at a time, never in
parallel, for that reason.
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
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 900
# The curl binary is the slow one: a mutation that removes the timeout
# altogether makes every one of its five cases sit out its own 10 s bound.
TEST_TIMEOUT = 240

TRANSPORT_CPP = "connectors/src/net/curl_transport.cpp"
TRANSPORT_H = "connectors/include/flox-connectors/net/curl_transport.h"
POLICIES_H = "connectors/include/flox-connectors/execution/executor_policies.h"
HL_CPP = "connectors/src/hyperliquid/hyperliquid_order_executor.cpp"
BITGET_CPP = "connectors/src/bitget/bitget_order_executor.cpp"
BYBIT_CONNECTOR_CPP = "connectors/src/bybit/bybit_exchange_connector.cpp"
DECIMAL_H = "include/flox/util/base/decimal.h"

# The six acceptance binaries, plus the two the Decimal::toString change is
# answerable to.
CURL_T = "unit_test_curl_transport_timeout"
WAIT_T = "unit_test_rate_limit_wait_policy"
BITGET_RL_T = "unit_test_bitget_rate_limit_paths"
HL_RL_T = "unit_test_hyperliquid_rate_limit_reject"
BITGET_PRICE_T = "unit_test_bitget_plan_order_price"
BYBIT_EXPIRY_T = "unit_test_bybit_option_expiry_tz"
DECIMAL_LOCALE_T = "test_decimal_locale"
DECIMAL_T = "test_decimal"

ACCEPTANCE = [CURL_T, WAIT_T, BITGET_RL_T, HL_RL_T, BITGET_PRICE_T, BYBIT_EXPIRY_T]

# Every offline connector binary links flox-connectors, so every one of them
# links the changed executors, the transport and the limiter. The sweep runs
# all of them plus the two Decimal binaries.
SWEEP = ACCEPTANCE + [
    DECIMAL_LOCALE_T,
    DECIMAL_T,
    "unit_test_order_serialization_bitget",
    "unit_test_order_serialization_bybit",
    "unit_test_order_serialization_hyperliquid",
    "unit_test_bitget_fill_contract",
    "unit_test_bybit_fill_contract",
    "unit_test_hyperliquid_fill_contract",
    "unit_test_bybit_option_symbol",
    "unit_test_bybit_connector_lifecycle",
    "unit_test_bybit_gap",
    "unit_test_bybit_private_stream_fills",
    "unit_test_fill_watermark",
    "unit_test_fixed_point_parse",
    "unit_test_unpriced_fill",
    "unit_test_polymarket_delta",
    "unit_test_polymarket_timestamps",
    "unit_test_ix_ws_client_shutdown",
    "unit_test_ix_ws_client_stop_latency",
]

# Object files are deleted by dependency, but only inside the target
# directories the upcoming build will actually repopulate, so the tree is
# never left with objects nobody is going to rebuild.
LIB_TARGET_DIRS = ["flox.dir", "flox-connectors.dir"]


@dataclass
class Edit:
    file: str
    old: str
    new: str
    occurrence: int = 1
    expected_occurrences: int = 1


@dataclass
class Mutation:
    name: str
    why: str
    # The binary whose named cases must go red, and the gtest filter naming
    # them. The whole binary is the fallback surface when the filter stays
    # green.
    binary: str
    test: str
    file: str = ""
    old: str = ""
    new: str = ""
    occurrence: int = 1
    expected_occurrences: int = 1
    edits: list[Edit] = field(default_factory=list)
    # A mutation expected to survive for a documented reason that is not a
    # hole in the tests. It still runs; it just does not fail the script.
    equivalent_reason: str = ""
    # Filled in from HOLES below for a mutation that survived: what a test
    # would have to assert to catch it. A hole still fails the script -- the
    # reason records what is missing, it does not excuse it.
    hole_reason: str = ""

    def editList(self) -> list[Edit]:
        if self.edits:
            return self.edits
        return [Edit(self.file, self.old, self.new, self.occurrence, self.expected_occurrences)]

    def files(self) -> list[str]:
        seen: list[str] = []
        for e in self.editList():
            if e.file not in seen:
                seen.append(e.file)
        return seen


MUTATIONS: list[Mutation] = [
    # ---------------------------------------------------------------- curl timeouts
    Mutation(
        name="request-timeout-truncated-to-whole-seconds",
        why="CURLOPT_TIMEOUT_MS goes back to the second-resolution CURLOPT_TIMEOUT with the "
            "old max(1, ms/1000): 1500 ms becomes one second and 250 ms is raised to one",
        binary=CURL_T,
        test="CurlTransportTimeout.FifteenHundredMillisecondsIsHonouredAsFifteenHundred:"
             "CurlTransportTimeout.SubSecondTimeoutIsNotRaisedToAWholeSecond",
        file=TRANSPORT_CPP,
        old="  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, req.requestTimeoutMs);",
        new="  curl_easy_setopt(h, CURLOPT_TIMEOUT,\n"
            "                   req.requestTimeoutMs / 1000 > 0 ? req.requestTimeoutMs / 1000 : 1L);",
    ),
    Mutation(
        name="request-timeout-multiplied-by-a-thousand",
        why="the millisecond option is fed seconds: a 1500 ms budget becomes 1500 s, so the "
            "request never gives up inside any bound the caller configured",
        binary=CURL_T,
        test="CurlTransportTimeout.FifteenHundredMillisecondsIsHonouredAsFifteenHundred:"
             "CurlTransportTimeout.SubSecondTimeoutIsNotRaisedToAWholeSecond",
        file=TRANSPORT_CPP,
        old="  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, req.requestTimeoutMs);",
        new="  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, req.requestTimeoutMs * 1000);",
    ),
    Mutation(
        name="connect-timeout-in-whole-seconds",
        why="the connect timeout goes back to second resolution, so a sub-second connect "
            "budget is truncated to zero (no limit) instead of being honoured",
        binary=CURL_T,
        test="CurlTransportTimeout.ConnectTimeoutIsBoundedInMilliseconds",
        file=TRANSPORT_CPP,
        old="  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, req.connectTimeoutMs);",
        new="  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, req.connectTimeoutMs / 1000);",
    ),
    Mutation(
        name="connect-timeout-not-set",
        why="no connect timeout is set at all, so a venue address that black-holes the SYN "
            "is bounded only by libcurl's 300 s default",
        binary=CURL_T,
        test="CurlTransportTimeout.ConnectTimeoutIsBoundedInMilliseconds",
        file=TRANSPORT_CPP,
        old="  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, req.connectTimeoutMs);",
        new="  // BUG: no connect timeout is set.",
    ),
    Mutation(
        name="nosignal-dropped",
        why="CURLOPT_NOSIGNAL is no longer set, so libcurl is free to use its signal-based "
            "timeout path -- one-second granularity and not safe from several threads",
        binary=CURL_T,
        test="CurlTransportTimeout.*",
        file=TRANSPORT_CPP,
        old="  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);\n",
        new="",
        equivalent_reason=
            "unobservable in this process on this build. CURLOPT_NOSIGNAL only decides whether "
            "libcurl may use its signal-based (SIGALRM) timeout path, which it takes only when "
            "it has to resolve a name through a synchronous resolver; curl-config --features on "
            "this host reports AsynchDNS (libcurl 8.7.1), so that path does not exist here, and "
            "every offline case connects to an IP literal, so nothing is resolved at all. The "
            "option still has to be set -- on a libcurl built without AsynchDNS it is the "
            "difference between millisecond timeouts and one-second ones, and between safe and "
            "unsafe use from several sender threads -- but no test in this process can tell the "
            "two versions apart",
    ),
    # ---------------------------------------------------------------- async post()
    Mutation(
        name="post-performs-the-request-inline",
        why="submit() stops queueing and performs the request on the calling thread again, "
            "so the strategy thread is held for the whole round trip",
        binary=CURL_T,
        test="CurlTransportTimeout.PostReturnsAtOnceAndCompletesOnAnotherThread:"
             "CurlTransportTimeout.PostDoesNotHoldTheCallerBeyondTheConfiguredTimeout:"
             "CurlTransportTimeout.OrderSubmitDoesNotHoldTheStrategyThread",
        file=TRANSPORT_CPP,
        old="    _queue.push_back(std::move(req));\n  }\n  _queueCv.notify_one();\n}",
        new="  }\n  // BUG: performed on the caller's thread, the way it used to be.\n"
            "  perform(req);\n}",
    ),
    Mutation(
        name="send-queue-unbounded",
        why="the handoff queue loses its bound: a venue that stops draining is absorbed into "
            "memory instead of being refused",
        binary=CURL_T,
        test="CurlTransportTimeout.FullSendQueueRefusesAndStopAnswersTheRemainder",
        file=TRANSPORT_CPP,
        old="    if (_queue.size() >= _dispatchConfig.maxQueueDepth)",
        new="    if (_queue.size() >= static_cast<std::size_t>(-1))",
    ),
    Mutation(
        name="send-queue-overflow-dropped-silently",
        why="a request refused by the full queue is dropped without answering onError, so the "
            "caller publishes nothing and believes the order is in flight",
        binary=CURL_T,
        test="CurlTransportTimeout.FullSendQueueRefusesAndStopAnswersTheRemainder",
        file=TRANSPORT_CPP,
        old="      invokeSafely(req.onError, \"Transport send queue is full\", \"onError\");\n"
            "      return;",
        new="      // BUG: dropped with no answer to the caller.\n      return;",
    ),
    Mutation(
        name="stop-does-not-answer-the-remainder",
        why="stop() drops whatever is still queued instead of failing it through onError, so "
            "an order that never left the process produces no rejection",
        binary=CURL_T,
        test="CurlTransportTimeout.FullSendQueueRefusesAndStopAnswersTheRemainder",
        file=TRANSPORT_CPP,
        old="  for (auto& req : leftovers)\n  {\n"
            "    invokeSafely(req.onError, \"Transport stopped before the request was sent\", \"onError\");\n"
            "  }",
        new="  // BUG: the remainder is dropped without telling anyone.\n  (void)leftovers;",
    ),
    Mutation(
        name="stop-does-not-join-the-senders",
        why="stop() detaches the sender threads instead of joining them, so a request can be "
            "performed against a half-destroyed transport",
        binary=CURL_T,
        test="CurlTransportTimeout.*",
        file=TRANSPORT_CPP,
        old="  for (auto& t : _senders)\n  {\n    if (t.joinable())\n    {\n      t.join();\n"
            "    }\n  }\n  _senders.clear();",
        new="  // BUG: the sender threads are never joined.\n  for (auto& t : _senders)\n  {\n"
            "    if (t.joinable())\n    {\n      t.detach();\n    }\n  }\n  _senders.clear();",
    ),
    Mutation(
        name="two-sender-threads",
        why="the default sender count goes to two, so requests no longer leave in the order "
            "they were handed over -- a cancel can overtake the place it cancels",
        binary=CURL_T,
        test="CurlTransportTimeout.TwoPostsLeaveInHandoverOrder",
        file=TRANSPORT_H,
        old="  std::size_t senderThreads{1};",
        new="  std::size_t senderThreads{2};",
    ),
    Mutation(
        name="queued-request-borrows-the-callers-body",
        why="the queued Request holds a string_view of the caller's body instead of owning a "
            "copy, so the sender thread reads a buffer the caller has already destroyed",
        binary=CURL_T,
        test="CurlTransportTimeout.QueuedRequestOwnsTheBodyItWasGiven",
        edits=[
            Edit(TRANSPORT_H, "    std::string url;\n    std::string body;",
                 "    std::string url;\n    std::string_view body;"),
            Edit(TRANSPORT_CPP, "  req.body.assign(body);", "  req.body = body;"),
            Edit(
                TRANSPORT_CPP,
                "  curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.c_str());\n"
                "  curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));",
                "  const std::string bodyCopy(req.body);\n"
                "  curl_easy_setopt(h, CURLOPT_POSTFIELDS, bodyCopy.c_str());\n"
                "  curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyCopy.size()));",
            ),
        ],
    ),
    # ---------------------------------------------------------------- the gate
    Mutation(
        name="gate-sends-without-consulting-the-bucket",
        why="gate() runs the action whatever the bucket says, which is a client-side limiter "
            "that limits nothing",
        binary=BITGET_RL_T,
        test="BitgetRateLimitPaths.*",
        file=POLICIES_H,
        old="    if (_limiter->tryAcquire())\n    {\n      action();\n      return;\n    }",
        new="    if (_limiter->tryAcquire() || true)\n    {\n      action();\n      return;\n    }",
    ),
    Mutation(
        name="wait-sends-inline",
        why="RateLimitPolicy::WAIT stops deferring and sends immediately, so every waiter "
            "goes out over the budget",
        binary=WAIT_T,
        test="RateLimitWaitPolicy.WaitNeverSendsOverTheLimit:"
             "RateLimitWaitPolicy.WaitDoesNotBlockTheSubmittingThread",
        file=POLICIES_H,
        old="      case RateLimitPolicy::WAIT:\n"
            "        defer(orderId, std::function<void()>(std::forward<Action>(action)),\n"
            "              std::function<void()>(std::forward<OnRejected>(onRejected)));\n"
            "        return;",
        new="      case RateLimitPolicy::WAIT:\n        action();\n        return;",
    ),
    Mutation(
        name="wait-sleeps-on-the-caller",
        why="the original bug restored: WAIT sleeps on the calling thread and then proceeds "
            "whether or not the retry found a token",
        binary=WAIT_T,
        test="RateLimitWaitPolicy.WaitDoesNotBlockTheSubmittingThread:"
             "RateLimitWaitPolicy.WaitNeverSendsOverTheLimit",
        file=POLICIES_H,
        old="      case RateLimitPolicy::WAIT:\n"
            "        defer(orderId, std::function<void()>(std::forward<Action>(action)),\n"
            "              std::function<void()>(std::forward<OnRejected>(onRejected)));\n"
            "        return;",
        new="      case RateLimitPolicy::WAIT:\n        std::this_thread::sleep_for(waitTime);\n"
            "        (void)_limiter->tryAcquire();\n        action();\n        return;",
    ),
    Mutation(
        name="wait-sends-after-one-sleep-without-rechecking",
        why="the deferred sender stops re-checking the bucket after sleeping and sends on the "
            "first wake, so several deferrals wake into one refill and all send",
        binary=WAIT_T,
        test="RateLimitWaitPolicy.WaitNeverSendsOverTheLimit",
        file=POLICIES_H,
        old="      _cv.wait_for(lock, slice,\n                   [this]\n                   {\n"
            "                     return _stopping;\n                   });\n"
            "      if (_stopping)\n      {\n        return false;\n      }\n    }\n  }",
        new="      _cv.wait_for(lock, slice,\n                   [this]\n                   {\n"
            "                     return _stopping;\n                   });\n"
            "      if (_stopping)\n      {\n        return false;\n      }\n"
            "      // BUG: proceeds after one sleep whether or not a token is there.\n"
            "      return true;\n    }\n  }",
    ),
    Mutation(
        name="deferral-queue-unbounded",
        why="the deferral queue loses its bound, so a venue the strategy outruns is absorbed "
            "into memory instead of refusing the submit",
        binary=WAIT_T,
        test="RateLimitWaitPolicy.DeferralQueueIsBoundedAndOverflowIsRefused",
        file=POLICIES_H,
        old="  static constexpr std::size_t kMaxDeferred = 1024;",
        new="  static constexpr std::size_t kMaxDeferred = static_cast<std::size_t>(-1);",
    ),
    Mutation(
        name="deferral-overflow-dropped-silently",
        why="a submit refused by a full deferral queue is dropped without calling onRejected, "
            "so nothing is published and the order simply disappears",
        binary=WAIT_T,
        test="RateLimitWaitPolicy.DeferralQueueIsBoundedAndOverflowIsRefused",
        file=POLICIES_H,
        old="        FLOX_LOG_WARN(\"[RateLimit] Deferral queue full, rejecting orderId=\" << orderId);\n"
            "        onRejected();\n        return;",
        new="        FLOX_LOG_WARN(\"[RateLimit] Deferral queue full, dropping orderId=\" << orderId);\n"
            "        return;",
    ),
    Mutation(
        name="reject-policy-runs-the-action",
        why="RateLimitPolicy::REJECT sends the request it was supposed to refuse",
        binary=WAIT_T,
        test="RateLimitWaitPolicy.RejectPolicyStillRefusesAndReportsIt",
        file=POLICIES_H,
        old="                      << \"ms\");\n        onRejected();\n        return;",
        new="                      << \"ms\");\n        action();\n        return;",
    ),
    Mutation(
        name="reject-policy-says-nothing",
        why="REJECT refuses the request without calling onRejected, which is the silence the "
            "whole onRejected path was added to end",
        binary=HL_RL_T,
        test="HyperliquidRateLimitReject.RejectedSubmitIsReported:"
             "HyperliquidRateLimitReject.RejectedCancelIsReported:"
             "HyperliquidRateLimitReject.RejectedReplaceIsReported",
        file=POLICIES_H,
        old="                      << \"ms\");\n        onRejected();\n        return;",
        new="                      << \"ms\");\n        return;",
    ),
    Mutation(
        name="callback-policy-runs-the-action",
        why="RateLimitPolicy::CALLBACK notifies and then sends anyway, over the budget",
        binary=WAIT_T,
        test="RateLimitWaitPolicy.CallbackPolicyNotifiesRefusesAndDoesNotSend",
        file=POLICIES_H,
        old="          _config.onRateLimited(orderId, waitTime);\n        }\n        onRejected();\n"
            "        return;",
        new="          _config.onRateLimited(orderId, waitTime);\n        }\n        action();\n"
            "        return;",
    ),
    Mutation(
        name="callback-policy-says-nothing",
        why="CALLBACK denies the request but never calls onRejected, so nothing reaches the "
            "execution bus and the tracker keeps reporting the order",
        binary=HL_RL_T,
        test="HyperliquidRateLimitReject.CallbackPolicyRefusalIsReported",
        file=POLICIES_H,
        old="          _config.onRateLimited(orderId, waitTime);\n        }\n        onRejected();\n"
            "        return;",
        new="          _config.onRateLimited(orderId, waitTime);\n        }\n        return;",
    ),
    Mutation(
        name="unlimited-policy-drops-the-request",
        why="NoRateLimitPolicy::gate stops running the action, so an executor built with no "
            "limiter sends nothing at all",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.*",
        file=POLICIES_H,
        old="  template <typename Action, typename OnRejected>\n"
            "  void gate(OrderId, Action&& action, OnRejected&&)\n  {\n    action();\n  }",
        new="  template <typename Action, typename OnRejected>\n"
            "  void gate(OrderId, Action&&, OnRejected&&)\n  {\n"
            "    // BUG: the action is never run.\n  }",
    ),
    # ---------------------------------------------------------------- Hyperliquid
    Mutation(
        name="hl-submit-bypasses-the-gate",
        why="the Hyperliquid submit path sends without consulting the limiter",
        binary=HL_RL_T,
        test="HyperliquidRateLimitReject.RejectedSubmitIsReported",
        file=HL_CPP,
        old="  _policies.rateLimit.gate(\n      order.id,\n      [this, order]\n      {\n"
            "        sendSubmitOrder(order);\n      },\n      [this, order]\n      {\n"
            "        publishRateLimited(order);\n      });",
        new="  sendSubmitOrder(order);",
    ),
    Mutation(
        name="hl-cancel-bypasses-the-gate",
        why="the Hyperliquid cancel path sends without consulting the limiter, spending "
            "budget the submit path then does not have",
        binary=HL_RL_T,
        test="HyperliquidRateLimitReject.RejectedCancelIsReported",
        file=HL_CPP,
        old="  _policies.rateLimit.gate(\n      localId,\n      [this, localId]\n      {\n"
            "        sendCancelOrder(localId);\n      },\n      [this, local]\n      {\n"
            "        publishRateLimited(local);\n      });",
        new="  (void)local;\n  sendCancelOrder(localId);",
    ),
    Mutation(
        name="hl-replace-bypasses-the-gate",
        why="the Hyperliquid replace path sends without consulting the limiter",
        binary=HL_RL_T,
        test="HyperliquidRateLimitReject.RejectedReplaceIsReported",
        file=HL_CPP,
        old="  _policies.rateLimit.gate(\n      oldLocalId,\n      [this, oldLocalId, n]\n      {\n"
            "        sendReplaceOrder(oldLocalId, n);\n      },\n      [this, local]\n      {\n"
            "        publishRateLimited(local);\n      });",
        new="  (void)local;\n  sendReplaceOrder(oldLocalId, n);",
    ),
    Mutation(
        name="hl-cancel-rejection-loses-the-tracked-order",
        why="the refusal is built before the tracker entry is resolved, so the event carries "
            "a bare order with nothing but the id -- no symbol, side, price or quantity",
        binary=HL_RL_T,
        test="HyperliquidRateLimitReject.RejectedCancelIsReported",
        file=HL_CPP,
        old="  auto target = _orderTracker->get(localId);\n  if (!target)\n  {\n"
            "    FLOX_LOG_ERROR(\"[HL] cancelOrder: no orderState for localId \" << localId);\n"
            "    return;\n  }\n\n  Order local = target->localOrder;",
        new="  // BUG: the rejection is built before the tracker entry is resolved.\n"
            "  Order local;\n  local.id = localId;",
    ),
    Mutation(
        name="hl-refusal-uses-the-generic-reject-status",
        why="the client-side refusal is published as REJECTED rather than "
            "REJECTED_RATE_LIMIT, so nothing downstream can tell a venue rejection from a "
            "budget refusal",
        binary=HL_RL_T,
        test="HyperliquidRateLimitReject.*",
        file=HL_CPP,
        old="  ev.status = OrderEventStatus::REJECTED_RATE_LIMIT;",
        new="  ev.status = OrderEventStatus::REJECTED;",
    ),
    Mutation(
        name="hl-refusal-is-not-published",
        why="publishRateLimited builds the event and never publishes it, which is exactly the "
            "silence the fix was for",
        binary=HL_RL_T,
        test="HyperliquidRateLimitReject.RejectedSubmitIsReported:"
             "HyperliquidRateLimitReject.RejectedCancelIsReported:"
             "HyperliquidRateLimitReject.RejectedReplaceIsReported",
        file=HL_CPP,
        old="  ev.rejectReason = \"client-side rate limit\";\n  ev.publishNs = nowMonoNanos();\n"
            "  _orderBus->publish(std::move(ev));",
        new="  ev.rejectReason = \"client-side rate limit\";\n  ev.publishNs = nowMonoNanos();\n"
            "  // BUG: built and dropped.",
    ),
    # ---------------------------------------------------------------- Bitget send paths
    Mutation(
        name="bitget-submit-bypasses-the-gate",
        why="the Bitget submit path sends without consulting the limiter",
        binary=BITGET_RL_T,
        test="BitgetRateLimitPaths.SecondSubmitIsThrottled",
        file=BITGET_CPP,
        old="  _policies.rateLimit.gate(\n      order.id,\n      [this, order]\n      {\n"
            "        sendSubmitOrder(order);\n      },\n      [this, order]\n      {\n"
            "        publishRateLimited(order);\n      });",
        new="  sendSubmitOrder(order);",
    ),
    Mutation(
        name="bitget-set-leverage-bypasses-the-gate",
        why="setLeverage goes straight to the wire again, spending venue budget nothing "
            "accounts for",
        binary=BITGET_RL_T,
        test="BitgetRateLimitPaths.SetLeverageIsThrottled",
        file=BITGET_CPP,
        old="  _policies.rateLimit.gate(\n      kNoOrderId,\n      [this, symbol, leverage]\n      {\n"
            "        sendSetLeverage(symbol, leverage);\n      },\n      [symbol]\n      {\n"
            "        FLOX_LOG_WARN(\"[BitgetOE] setLeverage for \" << symbol\n"
            "                                                    << \" refused by the client-side rate limit\");\n"
            "      });",
        new="  sendSetLeverage(symbol, leverage);",
    ),
    Mutation(
        name="bitget-submit-with-leverage-bypasses-the-gate",
        why="submitOrderWithLeverage sends its set-leverage leg and its order over the budget",
        binary=BITGET_RL_T,
        test="BitgetRateLimitPaths.SubmitOrderWithLeverageIsThrottled",
        file=BITGET_CPP,
        old="  _policies.rateLimit.gate(\n      order.id,\n      [this, order, leverage, slPrice, tpPrice]\n"
            "      {\n        sendSubmitOrderWithLeverage(order, leverage, slPrice, tpPrice);\n"
            "      },\n      [this, order]\n      {\n        publishRateLimited(order);\n      });",
        new="  sendSubmitOrderWithLeverage(order, leverage, slPrice, tpPrice);",
    ),
    Mutation(
        name="bitget-place-pos-tpsl-bypasses-the-gate",
        why="placePosTpsl sends the protective stop over the budget instead of through the gate",
        binary=BITGET_RL_T,
        test="BitgetRateLimitPaths.PlacePosTpslIsThrottled",
        file=BITGET_CPP,
        old="  _policies.rateLimit.gate(\n      localId,\n"
            "      [this, symbol, holdSide, slPrice, tpPrice, localId]\n      {\n"
            "        sendPlacePosTpsl(symbol, holdSide, slPrice, tpPrice, localId);\n      },\n"
            "      [this, symbol, localId]\n      {\n"
            "        // A protective stop that never left the process is worth an event:\n"
            "        // the position is unprotected and only this connector knows it.\n"
            "        Order refused;\n        refused.id = localId;\n"
            "        refused.symbol = symbol;\n        publishRateLimited(refused);\n      });",
        new="  sendPlacePosTpsl(symbol, holdSide, slPrice, tpPrice, localId);",
    ),
    Mutation(
        name="bitget-modify-pos-tpsl-bypasses-the-gate",
        why="modifyPosTpsl -- the path a trailing stop walks on every bar, the one that fires "
            "most often -- goes straight to the wire again",
        binary=BITGET_RL_T,
        test="BitgetRateLimitPaths.ModifyPosTpslIsThrottled",
        file=BITGET_CPP,
        old="  _policies.rateLimit.gate(\n      kNoOrderId,\n"
            "      [this, symbol, exchangeOrderId, newTriggerPrice, qty]\n      {\n"
            "        sendModifyPosTpsl(symbol, exchangeOrderId, newTriggerPrice, qty);\n      },\n"
            "      [exchangeOrderId]\n      {\n"
            "        FLOX_LOG_WARN(\"[BitgetOE] modifyPosTpsl for \"\n"
            "                      << exchangeOrderId\n"
            "                      << \" refused by the client-side rate limit: the stop \"\n"
            "                         \"stays where the venue last accepted it\");\n      });",
        new="  sendModifyPosTpsl(symbol, exchangeOrderId, newTriggerPrice, qty);",
    ),
    Mutation(
        name="bitget-cancel-bypasses-the-gate",
        why="the Bitget cancel path sends without consulting the limiter",
        binary=BITGET_RL_T,
        test="BitgetRateLimitPaths.CancelIsThrottled:"
             "BitgetRateLimitPaths.BurstAcrossEveryPathIsThrottled",
        file=BITGET_CPP,
        old="  Order target = st->localOrder;\n  _policies.rateLimit.gate(\n      id,\n"
            "      [this, id]\n      {\n        sendCancelOrder(id);\n      },\n"
            "      [this, target]\n      {\n        publishRateLimited(target);\n      });",
        new="  (void)st;\n  sendCancelOrder(id);",
    ),
    # ---------------------------------------------------------------- precision
    Mutation(
        name="decimals-for-tick-off-by-one",
        why="decimalsForTick returns one digit too few, so every trigger and limit price is "
            "formatted coarser than the instrument quotes",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.PosTpslTriggerKeepsInstrumentPrecision:"
             "BitgetPlanOrderPrice.PosTpslTriggerOnACoarseInstrumentIsUnchanged",
        file=BITGET_CPP,
        old="  int decimals = kPriceDecimals;\n  while (decimals > 0 && raw % 10 == 0)\n  {\n"
            "    raw /= 10;\n    --decimals;\n  }\n  return decimals;",
        new="  int decimals = kPriceDecimals;\n  while (decimals > 0 && raw % 10 == 0)\n  {\n"
            "    raw /= 10;\n    --decimals;\n  }\n  return decimals > 0 ? decimals - 1 : 0;",
    ),
    Mutation(
        name="unset-tick-gives-one-digit",
        why="an instrument the registry has no tick size for goes back to the hardcoded one "
            "digit instead of everything the fixed-point type can carry",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.UnsetTickKeepsFullFixedPointPrecision",
        file=BITGET_CPP,
        old="    // No usable tick in the registry: send everything the fixed-point type can\n"
            "    // carry rather than silently rounding the strategy's price away.\n"
            "    return kPriceDecimals;",
        new="    // BUG: back to the hardcoded single digit.\n    return 1;",
    ),
    Mutation(
        name="trim-double-truncates-instead-of-rounding",
        why="trimDouble truncates at the instrument's precision instead of rounding, so every "
            "price is nudged toward zero by up to one tick",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.PosTpslTriggerOnACoarseInstrumentIsUnchanged",
        file=BITGET_CPP,
        old="  double rounded = std::round(v * scale) / scale;",
        new="  double rounded = std::trunc(v * scale) / scale;",
    ),
    Mutation(
        name="trim-double-always-rounds-up",
        why="trimDouble rounds away from zero at every digit, so a stop is always placed one "
            "tick further out than the strategy asked for",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.PosTpslTriggerSurvivesOnALowPricedSymbol",
        file=BITGET_CPP,
        old="  double rounded = std::round(v * scale) / scale;",
        new="  double rounded = std::ceil(v * scale) / scale;",
    ),
    Mutation(
        name="plan-trigger-back-to-price-tostring",
        why="the plan order's triggerPrice goes back to Price::toString(), which writes six "
            "fractional digits regardless of what the instrument quotes in",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.StopLimitCarriesItsLimitPrice",
        file=BITGET_CPP,
        old="      .append(trimDouble(order.triggerPrice.toDouble(), decimals))",
        new="      .append(order.triggerPrice.toString())",
    ),
    Mutation(
        name="pos-tpsl-stop-loss-back-to-one-digit",
        why="the stop-loss trigger of placePosTpsl is formatted with the old hardcoded single "
            "digit instead of the instrument's precision",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.PosTpslTriggerKeepsInstrumentPrecision:"
             "BitgetPlanOrderPrice.PosTpslTriggerSurvivesOnALowPricedSymbol",
        file=BITGET_CPP,
        old="        .append(trimDouble(slPrice, decimals))",
        new="        .append(trimDouble(slPrice, 1))",
    ),
    Mutation(
        name="pos-tpsl-take-profit-back-to-one-digit",
        why="the take-profit trigger of placePosTpsl keeps the old single digit, so a target "
            "on a finely quoted instrument is rounded away",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.PosTpslTakeProfitKeepsInstrumentPrecision",
        file=BITGET_CPP,
        old="        .append(trimDouble(tpPrice, decimals))",
        new="        .append(trimDouble(tpPrice, 1))",
    ),
    Mutation(
        name="modify-pos-tpsl-back-to-one-digit",
        why="the trailing stop's new trigger is formatted with one digit again, which is the "
            "finding itself: the stop is walked to a price the strategy did not choose",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.ModifyPosTpslTriggerKeepsInstrumentPrecision",
        file=BITGET_CPP,
        old="      .append(trimDouble(newTriggerPrice, decimalsForTick(info->tickSize)))",
        new="      .append(trimDouble(newTriggerPrice, 1))",
    ),
    # ---------------------------------------------------------------- plan orders
    Mutation(
        name="conditional-limit-sent-as-market",
        why="every plan order goes out as orderType \"market\" again, turning a STOP_LIMIT "
            "into a stop-market at the venue with no rejection and no event",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.StopLimitCarriesItsLimitPrice:"
             "BitgetPlanOrderPrice.TakeProfitLimitCarriesItsLimitPrice",
        file=BITGET_CPP,
        old="  body.append(\"\\\"orderType\\\":\\\"\").append(isLimitPlan ? \"limit\" : \"market\").append(\"\\\",\");",
        new="  body.append(\"\\\"orderType\\\":\\\"market\\\",\");",
    ),
    Mutation(
        name="conditional-limit-price-omitted",
        why="the plan order's type says limit but the price field is never written, so the "
            "venue gets a limit order with no limit",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.StopLimitCarriesItsLimitPrice:"
             "BitgetPlanOrderPrice.TakeProfitLimitCarriesItsLimitPrice",
        file=BITGET_CPP,
        old="  if (isLimitPlan)\n  {\n"
            "    body.append(\"\\\"price\\\":\\\"\").append(trimDouble(order.price.toDouble(), decimals)).append(\"\\\",\");\n"
            "  }",
        new="  // BUG: the limit price never reaches the venue.",
    ),
    Mutation(
        name="take-profit-limit-is-not-a-limit-plan",
        why="only STOP_LIMIT is recognised as a conditional limit order, so a "
            "TAKE_PROFIT_LIMIT silently degrades to take-profit-market",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.TakeProfitLimitCarriesItsLimitPrice",
        file=BITGET_CPP,
        old="      (order.type == OrderType::STOP_LIMIT || order.type == OrderType::TAKE_PROFIT_LIMIT);",
        new="      (order.type == OrderType::STOP_LIMIT);",
    ),
    Mutation(
        name="market-plan-also-carries-a-price",
        why="the price field is written for every plan order, so a stop-market is sent with a "
            "limit price it does not have",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.StopMarketStaysMarketWithNoPrice",
        file=BITGET_CPP,
        old="  if (isLimitPlan)\n  {\n"
            "    body.append(\"\\\"price\\\":\\\"\").append(trimDouble(order.price.toDouble(), decimals)).append(\"\\\",\");\n"
            "  }",
        new="  if (true)\n  {\n"
            "    body.append(\"\\\"price\\\":\\\"\").append(trimDouble(order.price.toDouble(), decimals)).append(\"\\\",\");\n"
            "  }",
    ),
    Mutation(
        name="conditional-limit-without-a-price-is-approximated",
        why="a conditional limit order with no limit price is sent with the trigger price "
            "standing in for it rather than being rejected, so the venue gets a bound the "
            "strategy never chose",
        binary=BITGET_PRICE_T,
        test="BitgetPlanOrderPrice.StopLimitWithoutALimitPriceIsRejected:"
             "BitgetPlanOrderPrice.TakeProfitLimitWithoutALimitPriceIsRejected",
        edits=[
            Edit(
                BITGET_CPP,
                "  if (isLimitPlan && order.price.raw() <= 0)\n  {\n"
                "    publishRejection(order, \"conditional limit order without a limit price\");\n"
                "    return;\n  }",
                "  // BUG: approximated with the trigger price instead of rejected.\n"
                "  const Price limitPrice =\n"
                "      (isLimitPlan && order.price.raw() <= 0) ? order.triggerPrice : order.price;",
            ),
            Edit(
                BITGET_CPP,
                "    body.append(\"\\\"price\\\":\\\"\").append(trimDouble(order.price.toDouble(), decimals)).append(\"\\\",\");",
                "    body.append(\"\\\"price\\\":\\\"\").append(trimDouble(limitPrice.toDouble(), decimals)).append(\"\\\",\");",
            ),
        ],
    ),
    # ---------------------------------------------------------------- option expiry
    Mutation(
        name="option-month-table-shifted-by-one",
        why="the month table is rotated by one, so AUG resolves to September and every option "
            "expiry lands a month out",
        binary=BYBIT_EXPIRY_T,
        test="BybitOptionExpiryTz.ExpiryIsUtcRegardlessOfProcessTimezone:"
             "BybitOptionExpiryTz.ExpiryIsUtcRegardlessOfLocale",
        file=BYBIT_CONNECTOR_CPP,
        old="  static constexpr std::string_view kMonths[12] = {\"JAN\", \"FEB\", \"MAR\", \"APR\", \"MAY\", \"JUN\",\n"
            "                                                   \"JUL\", \"AUG\", \"SEP\", \"OCT\", \"NOV\", \"DEC\"};",
        new="  static constexpr std::string_view kMonths[12] = {\"DEC\", \"JAN\", \"FEB\", \"MAR\", \"APR\", \"MAY\",\n"
            "                                                   \"JUN\", \"JUL\", \"AUG\", \"SEP\", \"OCT\", \"NOV\"};",
    ),
    Mutation(
        name="option-expiry-read-as-local-time",
        why="the date goes back through std::mktime, which reads the broken-down time as "
            "local, so the expiry moves by the host's UTC offset -- up to a full day",
        binary=BYBIT_EXPIRY_T,
        test="BybitOptionExpiryTz.ExpiryIsUtcRegardlessOfProcessTimezone",
        edits=[
            Edit(BYBIT_CONNECTOR_CPP, "#include <cctype>", "#include <cctype>\n#include <ctime>"),
            Edit(
                BYBIT_CONNECTOR_CPP,
                "  return std::chrono::system_clock::time_point(\n"
                "      std::chrono::duration_cast<std::chrono::system_clock::duration>(\n"
                "          std::chrono::sys_days(ymd).time_since_epoch()));",
                "  // BUG: read as local time again.\n  std::tm tm{};\n"
                "  tm.tm_mday = static_cast<int>(*day);\n"
                "  tm.tm_mon = static_cast<int>(month) - 1;\n"
                "  tm.tm_year = 100 + *year;\n"
                "  return std::chrono::system_clock::from_time_t(std::mktime(&tm));",
            ),
        ],
    ),
    Mutation(
        name="option-single-digit-day-rejected",
        why="only the seven-character day form is accepted, so a symbol written 1JAN25 fails "
            "to parse and registers as a Spot instrument with no strike and no expiry",
        binary=BYBIT_EXPIRY_T,
        test="BybitOptionExpiryTz.*",
        file=BYBIT_CONNECTOR_CPP,
        old="  if (token.size() != 7 && token.size() != 6)",
        new="  if (token.size() != 7)",
    ),
    Mutation(
        name="option-year-pivot-in-the-previous-century",
        why="the two-digit year pivots on 1900, so every option expires a hundred years ago",
        binary=BYBIT_EXPIRY_T,
        test="BybitOptionExpiryTz.ExpiryIsUtcRegardlessOfProcessTimezone:"
             "BybitOptionExpiryTz.ExpiryIsUtcRegardlessOfLocale",
        file=BYBIT_CONNECTOR_CPP,
        old="  const std::chrono::year_month_day ymd{std::chrono::year{2000 + *year}, std::chrono::month{month},",
        new="  const std::chrono::year_month_day ymd{std::chrono::year{1900 + *year}, std::chrono::month{month},",
    ),
    Mutation(
        name="option-month-token-not-uppercased",
        why="the month token is matched without being upper-cased first",
        binary=BYBIT_EXPIRY_T,
        test="BybitOptionExpiryTz.*",
        file=BYBIT_CONNECTOR_CPP,
        old="  std::string monthToken(token.substr(dayDigits, 3));\n  for (char& c : monthToken)\n"
            "  {\n    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));\n  }",
        new="  const std::string monthToken(token.substr(dayDigits, 3));",
        equivalent_reason="Bybit writes the expiry token in upper case and the symbol strings "
            "reaching parseOptionSymbol come from the venue's own instrument feed, so the "
            "toupper pass has no input it changes; it is defence against a hypothetical "
            "lower-case caller, not behaviour any test or any production path exercises.",
    ),
    # ---------------------------------------------------------------- Decimal::toString
    Mutation(
        name="decimal-tostring-back-to-std-to-string",
        why="Decimal::toString goes back through std::to_string(double), which writes the C "
            "locale's decimal separator into every order body",
        binary=DECIMAL_LOCALE_T,
        test="DecimalToString.KeepsTheDecimalPointUnderACommaDecimalLocale",
        file=DECIMAL_H,
        old="    static constexpr int kFractionDigits = 6;\n"
            "    static constexpr uint64_t kFractionScale = 1000000;\n"
            "    static_assert(static_cast<uint64_t>(Scale) <= (UINT64_MAX / kFractionScale),\n"
            "                  \"Scale too large to rescale to six fractional digits in 64 bits\");\n\n"
            "    const bool negative = _raw < 0;\n"
            "    // Negated as unsigned: the most negative int64_t has no positive\n"
            "    // counterpart.\n"
            "    const uint64_t magnitude =\n"
            "        negative ? (0ULL - static_cast<uint64_t>(_raw)) : static_cast<uint64_t>(_raw);\n"
            "    const uint64_t scale = static_cast<uint64_t>(Scale);\n\n"
            "    uint64_t whole = magnitude / scale;\n"
            "    const uint64_t remainder = magnitude % scale;\n"
            "    uint64_t fraction = (remainder * kFractionScale + scale / 2) / scale;\n"
            "    if (fraction >= kFractionScale)\n    {\n      fraction -= kFractionScale;\n"
            "      ++whole;\n    }\n\n    std::string out;\n    out.reserve(24);\n"
            "    if (negative)\n    {\n      out.push_back('-');\n    }\n"
            "    out.append(std::to_string(whole));\n    out.push_back('.');\n"
            "    for (uint64_t divisor = kFractionScale / 10; divisor > 0; divisor /= 10)\n    {\n"
            "      out.push_back(static_cast<char>('0' + (fraction / divisor) % 10));\n    }\n"
            "    return out;",
        new="    // BUG: back through the C locale.\n    return std::to_string(toDouble());",
    ),
    Mutation(
        name="decimal-tostring-drops-the-sign",
        why="a negative value is formatted without its minus sign, so a short quantity or a "
            "negative price reaches the venue as its own opposite",
        binary=DECIMAL_LOCALE_T,
        test="DecimalToString.WritesTheVenueFormat",
        file=DECIMAL_H,
        old="    if (negative)\n    {\n      out.push_back('-');\n    }",
        new="    // BUG: the sign is dropped.",
    ),
    Mutation(
        name="decimal-tostring-truncates-the-seventh-digit",
        why="the rescale to six fractional digits truncates instead of rounding, so the "
            "eighth digit the raw integer carries is silently cut rather than rounded the way "
            "printf did",
        binary=DECIMAL_LOCALE_T,
        test="DecimalToString.RoundsTheSeventhDigitRatherThanTruncating",
        file=DECIMAL_H,
        old="    uint64_t fraction = (remainder * kFractionScale + scale / 2) / scale;",
        new="    uint64_t fraction = (remainder * kFractionScale) / scale;",
    ),
    Mutation(
        name="decimal-tostring-carry-boundary-off-by-one",
        why="the carry out of the fraction fires one step late, so a value that rounds up to "
            "the next whole unit is written as zero point zero",
        binary=DECIMAL_LOCALE_T,
        test="DecimalToString.RoundsTheSeventhDigitRatherThanTruncating",
        file=DECIMAL_H,
        old="    if (fraction >= kFractionScale)",
        new="    if (fraction > kFractionScale)",
    ),
    Mutation(
        name="decimal-tostring-five-fractional-digits",
        why="the venue-facing shape changes from six fractional digits to five, which is not "
            "what every order body on every venue has always carried",
        binary=DECIMAL_LOCALE_T,
        test="DecimalToString.WritesTheVenueFormat",
        file=DECIMAL_H,
        old="    static constexpr uint64_t kFractionScale = 1000000;",
        new="    static constexpr uint64_t kFractionScale = 100000;",
    ),
    Mutation(
        name="decimal-tostring-reserve-removed",
        why="the output string's reserve is dropped",
        binary=DECIMAL_LOCALE_T,
        test="DecimalToString.*",
        file=DECIMAL_H,
        old="    out.reserve(24);\n",
        new="",
        equivalent_reason="std::string::reserve is an allocation hint with no effect on the "
            "characters produced; every observable property of toString is identical with and "
            "without it. Included to show the harness reports a genuinely equivalent mutation "
            "as equivalent rather than as a hole.",
    ),
]


# What every surviving mutation would need in order to be caught. These are
# gaps in the tests, not excuses: the script still exits non-zero for each of
# them. They are recorded here so a later reader knows exactly what is not
# covered, and so a newly surviving mutation with no entry stands out.
HOLES: dict[str, str] = {
    # Every hole the first mutation pass found has since been closed by a
    # test; the table is kept because the next mutation that survives belongs
    # in it, and an empty one is the honest state to hand over.
}

for _m in MUTATIONS:
    _m.hole_reason = HOLES.get(_m.name, "")


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


def binary_path(name: str) -> Path:
    for candidate in (BUILD / "connectors" / "tests" / name, BUILD / "tests" / name):
        if candidate.exists():
            return candidate
    # Not built yet: the connectors location is where every connector binary
    # lands, and the core tests directory is where the two Decimal ones do.
    if name.startswith("unit_test_"):
        return BUILD / "connectors" / "tests" / name
    return BUILD / "tests" / name


def object_files(files: list[str], targets: list[str]) -> list[Path]:
    """Every object that depends on one of `files`, restricted to the target
    directories the upcoming build will repopulate.

    The compiler's own .o.d dependency files are the source of truth for "what
    includes this header", so a header mutation invalidates exactly the
    translation units that actually see it -- no more, no less.
    """
    scopes = [f"/{t}.dir/" for t in targets] + [f"/{d}/" for d in LIB_TARGET_DIRS]
    found: set[Path] = set()
    for f in files:
        abs_path = str(REPO / f)
        result = subprocess.run(
            ["find", str(BUILD), "-type", "f", "-name", "*.o.d", "-exec",
             "grep", "-lF", abs_path, "{}", "+"],
            capture_output=True, text=True,
        )
        for line in result.stdout.split():
            obj = Path(line[:-2]) if line.endswith(".d") else Path(line)
            posix = obj.as_posix()
            if any(scope in posix for scope in scopes):
                found.add(obj)
    return sorted(found)


def delete_objects(files: list[str], targets: list[str]) -> str:
    """Deletes every dependent object and proves none is left behind.

    The previous cycle deletes on its way out, so a count of zero deletions
    here is normal and is not the same thing as "nothing was invalidated":
    what matters is that no object depending on the mutated file survives into
    the build, which is asserted rather than assumed.
    """
    objs = object_files(files, targets)
    removed = 0
    absent = 0
    for obj in objs:
        if obj.exists():
            obj.unlink()
            removed += 1
        else:
            absent += 1
    still_there = [o for o in objs if o.exists()]
    if still_there:
        raise SystemExit(f"object files survived deletion: {still_there}")
    return (f"{len(objs)} dependent object(s): {removed} deleted, "
            f"{absent} already absent, 0 left")


def rebuild(targets: list[str]) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(BUILD), "--target", *targets, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise SystemExit(f"rebuild of {', '.join(targets)} failed:\n{output[-6000:]}")
    if "Building CXX" not in output:
        raise SystemExit(
            f"rebuild of {', '.join(targets)} compiled nothing -- the result would have been "
            f"a stale binary, so the run is refused:\n{output[-2000:]}"
        )
    return output


def run_binary(name: str, gtest_filter: str | None) -> tuple[int, str]:
    binary = binary_path(name)
    cmd = [str(binary)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=TEST_TIMEOUT,
                                cwd=BUILD)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def ran_line(output: str) -> str:
    return next((line.strip() for line in output.splitlines()
                 if line.startswith("[==========] ") and " ran." in line), "")


def control(binaries: list[str], files: list[str]) -> bool:
    ok = True
    # Deleted first so the control binary is compiled from the source as it
    # stands right now, not served from whatever the last run left behind --
    # and so the "Building CXX" proof below means something.
    print(f"  objects {delete_objects(files, binaries)} "
          f"-- the control is compiled, not cached")
    rebuild(binaries)
    for name in binaries:
        code, output = run_binary(name, None)
        state = "green" if code == 0 else "RED"
        print(f"  control {name:<44} {state}   {ran_line(output)}")
        ok = ok and code == 0
    return ok


def apply(m: Mutation) -> dict[str, str]:
    originals: dict[str, str] = {}
    for e in m.editList():
        path = REPO / e.file
        if e.file not in originals:
            originals[e.file] = path.read_text()
        text = path.read_text()
        mutated = replace_occurrence(text, e.old, e.new, e.occurrence, e.expected_occurrences)
        if mutated == text:
            raise SystemExit(f"{m.name}: an edit to {e.file} changed nothing")
        path.write_text(mutated)
    return originals


def restore(originals: dict[str, str]) -> None:
    for f, text in originals.items():
        (REPO / f).write_text(text)


def run_mutation(m: Mutation) -> tuple[bool, str]:
    """Returns (killed, killing test or empty)."""
    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    if m.equivalent_reason:
        print(f"  equivalent: {m.equivalent_reason}")
    before = {f: sha256(REPO / f) for f in m.files()}
    for f, h in before.items():
        print(f"  sha256  before  {h}  {f}")

    originals = apply(m)
    for f in m.files():
        print(f"  sha256  mutated {sha256(REPO / f)}  {f}")

    killed = False
    killer = ""
    try:
        print(f"  objects {delete_objects(m.files(), [m.binary])}")

        output = rebuild([m.binary])
        compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
        print(f"  rebuilt {m.binary}: {compiled} 'Building CXX' line(s)")

        code, test_output = run_binary(m.binary, m.test)
        print(f"  {m.binary} --gtest_filter={m.test} -> exit {code} "
              f"({'RED' if code else 'green'})")
        if code != 0:
            killed = True
            killer = f"{m.binary} {m.test}"
        else:
            # Whole-binary fallback: the filter may simply have named the
            # wrong cases.
            code, test_output = run_binary(m.binary, None)
            print(f"  {m.binary} (whole binary) -> exit {code} "
                  f"({'RED' if code else 'green'})")
            if code != 0:
                killed = True
                killer = f"{m.binary} (whole binary)"
        if not killed:
            failing = [line for line in test_output.splitlines() if line.startswith("[  FAILED  ]")]
            print("  MUTATION SURVIVED its own binary" + (f": {failing}" if failing else ""))
    finally:
        restore(originals)
        for f, h in before.items():
            after = sha256(REPO / f)
            print(f"  sha256  after   {after}  {f}")
            if after != h:
                raise SystemExit(f"restore failed: {f} does not hash back to its original")
        delete_objects(m.files(), [m.binary])

    return killed, killer


def sweep(m: Mutation) -> tuple[bool, str]:
    """Re-applies a survivor and runs every related binary whole."""
    print(f"\n[sweep {m.name}] across {len(SWEEP)} related binaries")
    before = {f: sha256(REPO / f) for f in m.files()}
    originals = apply(m)
    killed = False
    killer = ""
    try:
        print(f"  objects {delete_objects(m.files(), SWEEP)}")
        rebuild(SWEEP)
        for name in SWEEP:
            code, output = run_binary(name, None)
            if code != 0:
                failing = [line for line in output.splitlines()
                           if line.startswith("[  FAILED  ]") and "test" not in line.split("]")[1]]
                print(f"  {name:<44} RED   {failing[:3]}")
                if not killed:
                    killed = True
                    killer = name
            else:
                print(f"  {name:<44} green")
    finally:
        restore(originals)
        for f, h in before.items():
            if sha256(REPO / f) != h:
                raise SystemExit(f"restore failed: {f} does not hash back to its original")
        delete_objects(m.files(), SWEEP)
    return killed, killer


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    parser.add_argument("--skip-sweep", action="store_true",
                        help="do not re-run survivors across every related binary")
    parser.add_argument("--skip-control", action="store_true",
                        help="skip the control runs (for a single --only probe)")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            flag = "  [equivalent]" if m.equivalent_reason else ""
            print(f"{m.name:<48} {m.binary:<44} {m.test}{flag}")
        print(f"\n{len(MUTATIONS)} mutations")
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
    binaries = sorted({m.binary for m in selected})
    control_files = sorted({f for m in selected for f in m.files()})

    if not args.skip_control:
        print("control run before the mutations")
        if not control(binaries, control_files):
            raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results: list[tuple[Mutation, bool, str]] = []
    for m in selected:
        killed, killer = run_mutation(m)
        results.append((m, killed, killer))

    survivors = [(m, killer) for m, killed, killer in results if not killed]
    swept: dict[str, str] = {}
    if survivors and not args.skip_sweep:
        print(f"\n{len(survivors)} mutation(s) survived their own binary; "
              f"sweeping them across every related binary")
        for m, _ in survivors:
            killed, killer = sweep(m)
            if killed:
                swept[m.name] = killer

    if not args.skip_control:
        print("\ncontrol run after the mutations")
        restored = control(binaries, control_files)
    else:
        restored = True

    print("\nsummary")
    results_names = [m for m, _, _ in results]
    unexplained: list[str] = []
    for m, killed, killer in results:
        if killed:
            print(f"  RED         {m.name:<48} {killer}")
        elif m.name in swept:
            print(f"  RED(sweep)  {m.name:<48} {swept[m.name]}")
        elif m.equivalent_reason:
            print(f"  EQUIVALENT  {m.name:<48} {m.equivalent_reason[:70]}...")
        elif m.hole_reason:
            print(f"  HOLE        {m.name:<48} needs {m.hole_reason}")
            unexplained.append(m.name)
        else:
            print(f"  SURVIVED    {m.name:<48} no test goes red, and no reason recorded")
            unexplained.append(m.name)

    print(f"\n{len(results)} mutation(s): "
          f"{sum(1 for _, k, _ in results if k)} killed by their own binary, "
          f"{len(swept)} killed only by the sweep, "
          f"{sum(1 for m, k, _ in results if not k and m.equivalent_reason and m.name not in swept)}"
          f" equivalent, {len(unexplained)} surviving (hole(s))")
    if unexplained:
        undocumented = [n for n in unexplained
                        if not next(m for m in results_names if m.name == n).hole_reason]
        print(f"surviving mutation(s): {', '.join(unexplained)}")
        if undocumented:
            print(f"with no recorded reason at all: {', '.join(undocumented)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not unexplained and restored else 1


if __name__ == "__main__":
    sys.exit(main())
