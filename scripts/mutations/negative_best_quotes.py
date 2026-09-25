#!/usr/bin/env python3
"""Mutation harness for the negative-best-quote fix (bestBid/bestAsk deriving
the tick from the ladder index instead of reading a cached tick and testing
its sign; the C API _opt trio that separates "no quote" from a price of
exactly 0.0; QuickJS returning null instead of 0.0 for no quote).

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks the fix one piece at a time,
in the source, and checks that a test goes red.

Three areas, three kinds of primary target:

    include/flox/book/nlevel_order_book.h   -> test_nlevel_order_book_negative_prices
    src/capi/flox_capi.cpp,
    include/flox/capi/flox_capi.h           -> test_capi_negative_book
    src/quickjs/js_bindings.cpp             -> test_quickjs

Every mutation runs its primary target first: a narrow gtest filter naming
the test expected to catch it, falling back to the whole binary if the
filter stays green (in case some other test in the same file happens to
catch it instead). A mutation that survives its primary target is then
swept across the other binaries that can actually observe the mutated file
(a binary that does not link or include the mutated file is not swept --
rebuilding it recompiles nothing, so running it again would not be a real
check, only a repeat of the control run):

    book mutations  -> test_nlevel_order_book, test_nlevel_order_book_window,
                        test_nlevel_order_book_model, test_nlevel_tick_rounding,
                        test_capi_negative_book, test_capi_infra
    capi mutations   -> test_capi_infra
    quickjs mutation -> (nothing else in the tree touches js_bindings.cpp)

test_capi_book and test_capi_strategy, named in the task brief, are not
build targets in this tree (grep tests/CMakeLists.txt); test_capi_infra is
the actual binary exercising flox_book_*/flox_strategy_* through the C API
and stands in for both.

Every run is honest about the build: the mutated file's hash is printed
before and after, the primary target's own object files are deleted so
nothing can be served from cache, the rebuild output has to contain
"Building CXX" or the run is refused, and each binary runs under a timeout.
A mutation that does not compile is not a mutation and is reported as such.

A mutation may carry `equivalent_reason`: still run and still reported, but
does not fail the overall exit code -- the reason is printed next to the
verdict either way. None of the mutations below claim one; every survivor
here is reported as a real hole instead (see the task note).

Usage:

    python3 scripts/mutations/negative_best_quotes.py            # everything
    python3 scripts/mutations/negative_best_quotes.py --list
    python3 scripts/mutations/negative_best_quotes.py --only bestbid-index-wrong-side

The build directory is expected to be configured already:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLOX_BUILD_TESTS=ON \
          -DFLOX_BUILD_CAPI=ON -DFLOX_NATIVE=OFF -DFLOX_BUILD_QUICKJS=ON

(FLOX_BUILD_QUICKJS=ON is not part of the task's baseline build line, but is
needed to compile test_quickjs at all -- src/quickjs is not built without it.
Every mutation here still applies with FLOX_BUILD_QUICKJS left OFF; the two
quickjs-* mutations just have no target to run against and are skipped, not
counted as survivors.)
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
BIN_DIR = BUILD / "tests"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 300
TEST_TIMEOUT = 60

BOOK = "include/flox/book/nlevel_order_book.h"
CAPI_CPP = "src/capi/flox_capi.cpp"
CAPI_H = "include/flox/capi/flox_capi.h"
JS_BINDINGS = "src/quickjs/js_bindings.cpp"

NLEVEL_NEG = "test_nlevel_order_book_negative_prices"
CAPI_NEG = "test_capi_negative_book"
QUICKJS = "test_quickjs"

# The book sweep: every binary that includes nlevel_order_book.h, directly or
# (for the two capi ones) via flox_capi.cpp. test_capi_book / test_capi_strategy
# from the task brief do not exist in this tree -- test_capi_infra is the real
# binary covering flox_book_*/flox_strategy_*.
BOOK_SWEEP = [
    "test_nlevel_order_book",
    "test_nlevel_order_book_window",
    "test_nlevel_order_book_model",
    "test_nlevel_tick_rounding",
    "test_capi_negative_book",
    "test_capi_infra",
]
# The capi sweep: binaries that link flox_capi and exercise the strategy-side
# book accessors, minus the primary target itself.
CAPI_SWEEP = ["test_capi_infra"]
QUICKJS_SWEEP: list[str] = []


@dataclass
class Mutation:
    name: str
    why: str
    file: str
    old: str
    new: str
    primary: str
    gtest_filter: str | None = None
    occurrence: int = 1
    expected_occurrences: int = 1
    sweep: list[str] = field(default_factory=list)
    equivalent_reason: str | None = None
    # Set when the primary target's binary does not exist under the
    # configured build (quickjs mutations when FLOX_BUILD_QUICKJS=OFF).
    optional_binary: bool = False


MUTATIONS: list[Mutation] = [
    # ========================================================================
    # nlevel_order_book.h
    # ========================================================================
    Mutation(
        name="bestbid-index-wrong-side",
        why="bestBidIndex()'s cached-cursor check reads _bestAskIdx instead of "
            "_bestBidIdx, so on a book with both sides cached bestBid() reports "
            "the ask's tick as the bid",
        file=BOOK,
        old="""  inline std::optional<size_t> bestBidIndex() const
  {
    if (_bestBidIdx < MAX_LEVELS)
    {
      return _bestBidIdx;
    }""",
        new="""  inline std::optional<size_t> bestBidIndex() const
  {
    if (_bestAskIdx < MAX_LEVELS)
    {
      return _bestAskIdx;
    }""",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.BestQuotesReportTheStoredNegativeLevels",
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="bestask-index-wrong-side",
        why="bestAskIndex()'s cached-cursor check reads _bestBidIdx instead of "
            "_bestAskIdx, the mirror of the mutation above",
        file=BOOK,
        old="""  inline std::optional<size_t> bestAskIndex() const
  {
    if (_bestAskIdx < MAX_LEVELS)
    {
      return _bestAskIdx;
    }""",
        new="""  inline std::optional<size_t> bestAskIndex() const
  {
    if (_bestBidIdx < MAX_LEVELS)
    {
      return _bestBidIdx;
    }""",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.BestQuotesReportTheStoredNegativeLevels",
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="bestbid-cache-bound-off-by-one",
        why="the cached-cursor check becomes _bestBidIdx <= MAX_LEVELS, which is "
            "true even for the MAX_LEVELS empty sentinel itself, so an empty "
            "book (or one just cleared) reports a phantom bid at baseIndex+MAX_LEVELS",
        file=BOOK,
        old="    if (_bestBidIdx < MAX_LEVELS)\n    {\n      return _bestBidIdx;\n    }",
        new="    if (_bestBidIdx <= MAX_LEVELS)\n    {\n      return _bestBidIdx;\n    }",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.AnEmptySideStillReportsNoQuote",
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="bestask-cache-bound-off-by-one",
        why="the ask side of the mutation above: _bestAskIdx <= MAX_LEVELS treats "
            "the empty sentinel as a valid cached index",
        file=BOOK,
        old="    if (_bestAskIdx < MAX_LEVELS)\n    {\n      return _bestAskIdx;\n    }",
        new="    if (_bestAskIdx <= MAX_LEVELS)\n    {\n      return _bestAskIdx;\n    }",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.AnEmptySideStillReportsNoQuote",
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="bestbid-lazy-scan-skipped",
        why="bestBidIndex()'s linear-scan fallback (the lazy rebuild of the "
            "cached cursor) is short-circuited to nullopt, as if the rebuild "
            "were skipped entirely",
        file=BOOK,
        old="""    if (_minBid >= MAX_LEVELS)
    {
      return std::nullopt;
    }

    for (size_t i = normalizeUpperBound(_maxBid) + 1; i-- > _minBid;)
    {
      if (!_bids[i].isZero())
      {
        return i;
      }

      if (i == 0)
      {
        break;
      }
    }
    return std::nullopt;
  }""",
        new="""    if (_minBid >= MAX_LEVELS)
    {
      return std::nullopt;
    }

    return std::nullopt;
  }""",
        primary=NLEVEL_NEG,
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="mid-uses-bid-twice",
        why="mid() reads bestBidTick() for both sides -- the ask side vanishes "
            "from the average entirely",
        file=BOOK,
        old="""    const auto bidTick = bestBidTick();
    const auto askTick = bestAskTick();
    if (!bidTick || !askTick)""",
        new="""    const auto bidTick = bestBidTick();
    const auto askTick = bestBidTick();
    if (!bidTick || !askTick)""",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.MidAndSpreadAreReportedAtNegativePrices",
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="spread-sign-flipped",
        why="spread() computes bid-minus-ask instead of ask-minus-bid, so an "
            "ordinary (non-crossed) book reports a negative spread",
        file=BOOK,
        old="    return Price::fromRaw(_tickSize.raw() * (*ask - *bid));",
        new="    return Price::fromRaw(_tickSize.raw() * (*bid - *ask));",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.MidAndSpreadAreReportedAtNegativePrices",
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="iscrossed-strict-inequality",
        why="isCrossed() uses > instead of >=, so a book touching exactly "
            "(bid tick == ask tick) reports clean instead of crossed",
        file=BOOK,
        old="    return *bid >= *ask;",
        new="    return *bid > *ask;",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.ACrossedNegativeBookIsReportedCrossed",
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="besttick-baseindex-offbyone",
        why="bestBidTick() adds one extra tick past the index -- an off-by-one "
            "on the _baseIndex + index arithmetic",
        file=BOOK,
        old="""  std::optional<int64_t> bestBidTick() const
  {
    const auto i = bestBidIndex();
    if (!i)
    {
      return std::nullopt;
    }
    return _baseIndex + static_cast<int64_t>(*i);
  }""",
        new="""  std::optional<int64_t> bestBidTick() const
  {
    const auto i = bestBidIndex();
    if (!i)
    {
      return std::nullopt;
    }
    return _baseIndex + static_cast<int64_t>(*i) + 1;
  }""",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.BestQuotesReportTheStoredNegativeLevels",
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="besttick-sign-reintroduced",
        why="the exact bug this task fixes, reintroduced by hand: bestBidTick() "
            "tests the resulting tick's sign and reports no quote for a "
            "negative one, even though the ladder index says there is a bid there",
        file=BOOK,
        old="""  std::optional<int64_t> bestBidTick() const
  {
    const auto i = bestBidIndex();
    if (!i)
    {
      return std::nullopt;
    }
    return _baseIndex + static_cast<int64_t>(*i);
  }""",
        new="""  std::optional<int64_t> bestBidTick() const
  {
    const auto i = bestBidIndex();
    if (!i)
    {
      return std::nullopt;
    }
    const int64_t tick = _baseIndex + static_cast<int64_t>(*i);
    if (tick < 0)
    {
      return std::nullopt;
    }
    return tick;
  }""",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.NegativeBidIsNotTheSameAsAnAbsentBid",
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="bestasktick-wrong-side",
        why="bestAskTick() calls bestBidIndex() instead of bestAskIndex()",
        file=BOOK,
        old="""  std::optional<int64_t> bestAskTick() const
  {
    const auto i = bestAskIndex();
    if (!i)
    {
      return std::nullopt;
    }
    return _baseIndex + static_cast<int64_t>(*i);
  }""",
        new="""  std::optional<int64_t> bestAskTick() const
  {
    const auto i = bestBidIndex();
    if (!i)
    {
      return std::nullopt;
    }
    return _baseIndex + static_cast<int64_t>(*i);
  }""",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.BestQuotesReportTheStoredNegativeLevels",
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="bestbid-reads-asktick",
        why="bestBid() calls bestAskTick() instead of bestBidTick()",
        file=BOOK,
        old="""  inline std::optional<Price> bestBid() const override
  {
    const auto t = bestBidTick();""",
        new="""  inline std::optional<Price> bestBid() const override
  {
    const auto t = bestAskTick();""",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.NegativeBidIsNotTheSameAsAnAbsentBid",
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="bestask-reads-bidtick",
        why="bestAsk() calls bestBidTick() instead of bestAskTick()",
        file=BOOK,
        old="""  inline std::optional<Price> bestAsk() const override
  {
    const auto t = bestAskTick();""",
        new="""  inline std::optional<Price> bestAsk() const override
  {
    const auto t = bestBidTick();""",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.NegativeAskIsNotTheSameAsAnAbsentAsk",
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="clear-doesnt-reset-bestidx",
        why="clear() stops resetting _bestBidIdx/_bestAskIdx to the empty "
            "sentinel, so a stale cached index survives a clear() and the next "
            "bestBid()/bestAsk() trusts it without checking the (now zeroed) level",
        file=BOOK,
        old="""    _minBid = _minAsk = MAX_LEVELS;
    _maxBid = _maxAsk = 0;
    _baseIndex = 0;
    _bestBidIdx = _bestAskIdx = MAX_LEVELS;
  }""",
        new="""    _minBid = _minAsk = MAX_LEVELS;
    _maxBid = _maxAsk = 0;
    _baseIndex = 0;
  }""",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.AnEmptySideStillReportsNoQuote",
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="snapshot-bid-index-update-wrong-direction",
        why="applyBookUpdate's bid write site advances _bestBidIdx toward the "
            "LOWEST index instead of the highest (i < _bestBidIdx instead of "
            "i > _bestBidIdx), so the cursor tracks the worst bid, not the best",
        file=BOOK,
        old="        if (_bestBidIdx >= MAX_LEVELS || i > _bestBidIdx)\n"
            "        {\n          _bestBidIdx = i;\n        }",
        new="        if (_bestBidIdx >= MAX_LEVELS || i < _bestBidIdx)\n"
            "        {\n          _bestBidIdx = i;\n        }",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.PositiveQuotesAreUnchanged",
        occurrence=1,
        expected_occurrences=2,
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="reanchor-rebuild-bid-wrong-direction",
        why="the same wrong-direction bug, in reanchorWithData's from-scratch "
            "rebuild of _bestBidIdx after the window moves -- the delta-feed "
            "path a market walking through zero takes",
        file=BOOK,
        old="        if (_bestBidIdx >= MAX_LEVELS || i > _bestBidIdx)\n"
            "        {\n          _bestBidIdx = i;\n        }",
        new="        if (_bestBidIdx >= MAX_LEVELS || i < _bestBidIdx)\n"
            "        {\n          _bestBidIdx = i;\n        }",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.AMarketWalkingBelowZeroKeepsItsBestQuotes",
        occurrence=2,
        expected_occurrences=2,
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="snapshot-ask-index-update-wrong-direction",
        why="applyBookUpdate's ask write site advances _bestAskIdx toward the "
            "HIGHEST index instead of the lowest",
        file=BOOK,
        old="        if (_bestAskIdx >= MAX_LEVELS || i < _bestAskIdx)\n"
            "        {\n          _bestAskIdx = i;\n        }",
        new="        if (_bestAskIdx >= MAX_LEVELS || i > _bestAskIdx)\n"
            "        {\n          _bestAskIdx = i;\n        }",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.PositiveQuotesAreUnchanged",
        occurrence=1,
        expected_occurrences=2,
        sweep=BOOK_SWEEP,
    ),
    Mutation(
        name="reanchor-rebuild-ask-wrong-direction",
        why="the ask side of the reanchor rebuild mutation above",
        file=BOOK,
        old="        if (_bestAskIdx >= MAX_LEVELS || i < _bestAskIdx)\n"
            "        {\n          _bestAskIdx = i;\n        }",
        new="        if (_bestAskIdx >= MAX_LEVELS || i > _bestAskIdx)\n"
            "        {\n          _bestAskIdx = i;\n        }",
        primary=NLEVEL_NEG,
        gtest_filter="NLevelOrderBookNegativePrices.AMarketWalkingBelowZeroKeepsItsBestQuotes",
        occurrence=2,
        expected_occurrences=2,
        sweep=BOOK_SWEEP,
    ),

    # ========================================================================
    # C API: flox_capi.cpp / flox_capi.h
    # ========================================================================
    Mutation(
        name="capi-macro-removed",
        why="FLOX_HAS_OPTIONAL_RAW_BEST_QUOTE is removed from flox_capi.h, the "
            "header the test includes, so the acceptance case compiles its "
            "#else branch (an explicit FAIL())",
        file=CAPI_H,
        old="#define FLOX_HAS_OPTIONAL_RAW_BEST_QUOTE 1",
        new="/* FLOX_HAS_OPTIONAL_RAW_BEST_QUOTE removed by mutation */",
        primary=CAPI_NEG,
        gtest_filter="CapiNegativeBook.StrategyRawBestQuotesSeparateNoQuoteFromAPriceOfZero",
        sweep=CAPI_SWEEP,
    ),
    Mutation(
        name="capi-bid-opt-flag-inverted",
        why="flox_best_bid_raw_opt returns 1 for an empty side and 0 for a "
            "present one -- the presence flag inverted",
        file=CAPI_CPP,
        old="""uint8_t flox_best_bid_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  FLOX_CAPI_ENTER(s);
  auto bid = toStrategy(s)->ctx(symbol).book.bestBid();
  if (!bid)
  {
    return 0;
  }
  if (price_out)
  {
    *price_out = bid->raw();
  }
  return 1;
  FLOX_CAPI_LEAVE;
}""",
        new="""uint8_t flox_best_bid_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  FLOX_CAPI_ENTER(s);
  auto bid = toStrategy(s)->ctx(symbol).book.bestBid();
  if (!bid)
  {
    return 1;
  }
  if (price_out)
  {
    *price_out = bid->raw();
  }
  return 0;
  FLOX_CAPI_LEAVE;
}""",
        primary=CAPI_NEG,
        gtest_filter="CapiNegativeBook.StrategyRawBestQuotesSeparateNoQuoteFromAPriceOfZero",
        sweep=CAPI_SWEEP,
    ),
    Mutation(
        name="capi-ask-opt-flag-inverted",
        why="flox_best_ask_raw_opt's presence flag inverted, same as the bid "
            "mutation above. No acceptance or regression test calls "
            "flox_best_ask_raw_opt at all -- see the task note for this as a hole",
        file=CAPI_CPP,
        old="""uint8_t flox_best_ask_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  FLOX_CAPI_ENTER(s);
  auto ask = toStrategy(s)->ctx(symbol).book.bestAsk();
  if (!ask)
  {
    return 0;
  }
  if (price_out)
  {
    *price_out = ask->raw();
  }
  return 1;
  FLOX_CAPI_LEAVE;
}""",
        new="""uint8_t flox_best_ask_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  FLOX_CAPI_ENTER(s);
  auto ask = toStrategy(s)->ctx(symbol).book.bestAsk();
  if (!ask)
  {
    return 1;
  }
  if (price_out)
  {
    *price_out = ask->raw();
  }
  return 0;
  FLOX_CAPI_LEAVE;
}""",
        primary=CAPI_NEG,
        sweep=CAPI_SWEEP,
    ),
    Mutation(
        name="capi-mid-opt-uses-bid",
        why="flox_mid_price_raw_opt reads ctx(symbol).book.bestBid() instead of "
            "ctx(symbol).mid() -- the mid price becomes the bid price. No "
            "acceptance or regression test calls flox_mid_price_raw_opt at all",
        file=CAPI_CPP,
        old="""uint8_t flox_mid_price_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  FLOX_CAPI_ENTER(s);
  auto mid = toStrategy(s)->ctx(symbol).mid();""",
        new="""uint8_t flox_mid_price_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  FLOX_CAPI_ENTER(s);
  auto mid = toStrategy(s)->ctx(symbol).book.bestBid();""",
        primary=CAPI_NEG,
        sweep=CAPI_SWEEP,
    ),
    Mutation(
        name="capi-bid-opt-price-written-when-absent",
        why="flox_best_bid_raw_opt writes 0 into *price_out on the empty-side "
            "path before returning 0. The acceptance test reuses the same "
            "local across both calls and only checks it after the present-bid "
            "call, so a write on the absent path is never observed",
        file=CAPI_CPP,
        old="""  auto bid = toStrategy(s)->ctx(symbol).book.bestBid();
  if (!bid)
  {
    return 0;
  }
  if (price_out)
  {
    *price_out = bid->raw();
  }
  return 1;""",
        new="""  auto bid = toStrategy(s)->ctx(symbol).book.bestBid();
  if (!bid)
  {
    if (price_out)
    {
      *price_out = 0;
    }
    return 0;
  }
  if (price_out)
  {
    *price_out = bid->raw();
  }
  return 1;""",
        primary=CAPI_NEG,
        gtest_filter="CapiNegativeBook.StrategyRawBestQuotesSeparateNoQuoteFromAPriceOfZero",
        sweep=CAPI_SWEEP,
    ),
    Mutation(
        name="capi-bid-opt-null-deref-unconditional",
        why="flox_best_bid_raw_opt drops the `if (price_out)` guard and "
            "dereferences price_out unconditionally, contradicting the header "
            "comment that price_out may be NULL. Every caller in the tree "
            "passes a real pointer, so nothing here crashes and nothing catches it",
        file=CAPI_CPP,
        old="""uint8_t flox_best_bid_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  FLOX_CAPI_ENTER(s);
  auto bid = toStrategy(s)->ctx(symbol).book.bestBid();
  if (!bid)
  {
    return 0;
  }
  if (price_out)
  {
    *price_out = bid->raw();
  }
  return 1;
  FLOX_CAPI_LEAVE;
}""",
        new="""uint8_t flox_best_bid_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  FLOX_CAPI_ENTER(s);
  auto bid = toStrategy(s)->ctx(symbol).book.bestBid();
  if (!bid)
  {
    return 0;
  }
  *price_out = bid->raw();
  return 1;
  FLOX_CAPI_LEAVE;
}""",
        primary=CAPI_NEG,
        gtest_filter="CapiNegativeBook.StrategyRawBestQuotesSeparateNoQuoteFromAPriceOfZero",
        sweep=CAPI_SWEEP,
    ),

    # ========================================================================
    # QuickJS: src/quickjs/js_bindings.cpp
    # ========================================================================
    Mutation(
        name="quickjs-best-bid-zero-not-null",
        why="js_best_bid returns the JS number 0.0 instead of JS_NULL when "
            "there is no quote -- back to the bug the fix removed. No test in "
            "the tree calls the strategy-level symbol.bestBid()/bestAsk()/"
            "midPrice() JS wrappers (test_quickjs only exercises the unrelated "
            "window-snapshot book.bestBid(n) method), so nothing catches it",
        file=JS_BINDINGS,
        old="""static JSValue js_best_bid(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = 0;
  if (!flox_best_bid_raw_opt(h, sym, &raw))
  {
    return JS_NULL;
  }
  return JS_NewFloat64(ctx, flox_price_to_double(raw));
}""",
        new="""static JSValue js_best_bid(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = 0;
  if (!flox_best_bid_raw_opt(h, sym, &raw))
  {
    return JS_NewFloat64(ctx, 0.0);
  }
  return JS_NewFloat64(ctx, flox_price_to_double(raw));
}""",
        primary=QUICKJS,
        sweep=QUICKJS_SWEEP,
        optional_binary=True,
    ),
    Mutation(
        name="quickjs-mid-price-zero-not-null",
        why="js_mid_price returns 0.0 instead of JS_NULL when there is no "
            "quote -- same hole as the bid mutation above, on midPrice()",
        file=JS_BINDINGS,
        old="""static JSValue js_mid_price(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = 0;
  if (!flox_mid_price_raw_opt(h, sym, &raw))
  {
    return JS_NULL;
  }
  return JS_NewFloat64(ctx, flox_price_to_double(raw));
}""",
        new="""static JSValue js_mid_price(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = 0;
  if (!flox_mid_price_raw_opt(h, sym, &raw))
  {
    return JS_NewFloat64(ctx, 0.0);
  }
  return JS_NewFloat64(ctx, flox_price_to_double(raw));
}""",
        primary=QUICKJS,
        sweep=QUICKJS_SWEEP,
        optional_binary=True,
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


def object_files(target: str) -> list[Path]:
    """The target's own object files, located the way `find` would."""
    out = subprocess.run(
        ["find", str(BUILD), "-type", "f", "-name", "*.o", "-path", f"*{target}.dir*"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [Path(p) for p in out]


class BuildFailed(Exception):
    def __init__(self, target: str, output: str):
        super().__init__(f"rebuild of {target} failed")
        self.target = target
        self.output = output


def target_exists(target: str) -> bool:
    result = subprocess.run(
        [CMAKE, "--build", str(BUILD), "--target", "help"],
        capture_output=True, text=True, timeout=60, cwd=REPO,
    )
    return f"... {target}" in result.stdout


def rebuild(target: str) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(BUILD), "--target", target, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise BuildFailed(target, output)
    if "Building CXX" not in output:
        raise SystemExit(
            f"rebuild of {target} compiled nothing -- the result would have been "
            f"a stale binary, so the run is refused:\n{output[-2000:]}"
        )
    return output


def run_test(target: str, gtest_filter: str | None) -> tuple[int, str]:
    binary = BIN_DIR / target
    cmd = [str(binary)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=TEST_TIMEOUT)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def summary_line(output: str) -> str:
    return next((line for line in output.splitlines()
                if line.startswith("[==========] ") and " ran." in line), "")


def control(targets: list[str]) -> bool:
    ok = True
    for target in targets:
        for obj in object_files(target):
            obj.unlink()
        rebuild(target)
        code, output = run_test(target, None)
        state = "green" if code == 0 else "RED"
        print(f"  control {target:<34} {state}   {summary_line(output).strip()}")
        ok = ok and code == 0
    return ok


def run_mutation(m: Mutation) -> str:
    """'killed', 'alive', 'no-compile', or 'skipped' (optional binary missing)."""
    path = REPO / m.file
    original = path.read_text()
    before = sha256(path)
    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    print(f"  file    {m.file}")
    print(f"  sha256  before  {before}")

    mutated = replace_occurrence(original, m.old, m.new, m.occurrence, m.expected_occurrences)
    if mutated == original:
        raise SystemExit("mutation changed nothing")
    path.write_text(mutated)
    print(f"  sha256  mutated {sha256(path)}")

    verdict = "alive"
    try:
        removed = object_files(m.primary)
        for obj in removed:
            obj.unlink()
        print(f"  removed {len(removed)} object file(s) for {m.primary}")

        try:
            output = rebuild(m.primary)
        except BuildFailed as e:
            print(f"  {e.target} DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-20:]))
            return "no-compile"
        compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
        print(f"  rebuilt {m.primary}: {compiled} 'Building CXX' line(s)")

        code, test_output = run_test(m.primary, m.gtest_filter)
        red = code != 0
        flt = f" --gtest_filter={m.gtest_filter}" if m.gtest_filter else ""
        print(f"  {m.primary}{flt} -> exit {code} "
              f"({'RED, mutation killed' if red else 'green'})")
        if red:
            verdict = "killed"
            failed = [line for line in test_output.splitlines()
                      if line.startswith("[  FAILED  ]")]
            for line in failed[:6]:
                print(f"    {line}")
        elif m.gtest_filter:
            code2, test_output2 = run_test(m.primary, None)
            red2 = code2 != 0
            print(f"  {m.primary} (whole binary fallback) -> exit {code2} "
                  f"({'RED, mutation killed' if red2 else 'green'})")
            if red2:
                verdict = "killed"
                failed = [line for line in test_output2.splitlines()
                          if line.startswith("[  FAILED  ]")]
                for line in failed[:6]:
                    print(f"    {line}")

        if verdict == "alive" and m.sweep:
            print(f"  primary target survived -- sweeping {len(m.sweep)} related binary(ies)")
            for target in m.sweep:
                for obj in object_files(target):
                    obj.unlink()
                sweep_output = rebuild(target)
                sweep_compiled = sum(1 for line in sweep_output.splitlines()
                                     if "Building CXX" in line)
                scode, soutput = run_test(target, None)
                sred = scode != 0
                print(f"  sweep {target:<26} {sweep_compiled} 'Building CXX' line(s), "
                      f"exit {scode} ({'RED, caught here' if sred else 'green'})")
                if sred:
                    verdict = "killed"
                    failed = [line for line in soutput.splitlines()
                              if line.startswith("[  FAILED  ]")]
                    for line in failed[:6]:
                        print(f"    {line}")

        if verdict == "alive":
            if m.equivalent_reason:
                print("  GREEN, MUTATION SURVIVED -- claimed equivalent (see reason above)")
            else:
                print("  GREEN, MUTATION SURVIVED every binary asked")
    finally:
        path.write_text(original)
        after = sha256(path)
        print(f"  sha256  after   {after}")
        if after != before:
            raise SystemExit("restore failed: the file does not hash back to its original")
        for target in [m.primary] + m.sweep:
            for obj in object_files(target):
                obj.unlink()

    return verdict


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            eq = "  [equivalence claimed]" if m.equivalent_reason else ""
            opt = "  [optional binary]" if m.optional_binary else ""
            print(f"{m.name:<42} {m.file}  ->  {m.primary}{eq}{opt}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(
            f"{BUILD} is not configured; run\n"
            "  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLOX_BUILD_TESTS=ON "
            "-DFLOX_BUILD_CAPI=ON -DFLOX_NATIVE=OFF"
        )

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")

    quickjs_available = (BIN_DIR / QUICKJS).is_file()
    if not quickjs_available:
        for m in selected:
            if m.optional_binary:
                print(f"[{m.name}] skipped -- {QUICKJS} was not built "
                      f"(configure with -DFLOX_BUILD_QUICKJS=ON to include it)")
        selected = [m for m in selected if not m.optional_binary]

    if not selected:
        raise SystemExit("nothing left to run")

    targets = sorted({m.primary for m in selected} | {t for m in selected for t in m.sweep})

    print("control run before the mutations")
    if not control(targets):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, run_mutation(m)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(targets)

    print("\nsummary")
    label = {"killed": "RED  ", "alive": "ALIVE", "no-compile": "NOBLD"}
    for m, verdict in results:
        eq = "  (claimed equivalent)" if (verdict == "alive" and m.equivalent_reason) else ""
        print(f"  {label[verdict]}  {m.name:<42} {m.primary}{eq}")

    survived = [m.name for m, v in results if v == "alive" and not m.equivalent_reason]
    equivalent = [m.name for m, v in results if v == "alive" and m.equivalent_reason]
    nobuild = [m.name for m, v in results if v == "no-compile"]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived: {', '.join(survived)}")
    if equivalent:
        print(f"{len(equivalent)} mutation(s) survived but are claimed equivalent: "
              f"{', '.join(equivalent)}")
    if nobuild:
        print(f"{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
