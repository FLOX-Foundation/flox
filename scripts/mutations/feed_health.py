#!/usr/bin/env python3
"""Mutation harness for the feed-health contract on the exchange connectors
(the transport-close and quiet-feed events every venue now reports, Bybit's
book-gap event, Bitget's "seq" and "checksum" book integrity, the Polymarket
snapshot type, the dead config fields, and the Hyperliquid signer transport).

A passing test proves nothing on its own; what it has to do is fail when the
code it covers is wrong. This script breaks one piece of the fix at a time, in
the source, and checks that the tests go red -- and that an unmutated tree is
green before and after.

Every run is honest about the build. Per mutation: the file's sha256 is printed
before and after, each anchor has to occur exactly the expected number of
times, the mutated .cpp's object inside the flox-connectors library and the
test target's own objects are deleted so nothing can be served from cache, the
rebuild output has to contain "Building CXX" or the run is refused, and each
test binary runs under a timeout. A mutation that does not compile is not a
mutation and is reported as such.

A mutation that survives its own target is then run against every offline
connector test in the tree (the sweep), so "survived" means no connector test
anywhere notices it.

One mutation is in Python (the signing daemon). It has no build step; its
target is the pytest file that covers the daemon, and the C++ sweep is skipped
because no C++ binary compiles a .py file.

Usage:

    python3 scripts/mutations/feed_health.py            # control, all mutations, control
    python3 scripts/mutations/feed_health.py --list
    python3 scripts/mutations/feed_health.py --only bybit-gap-not-emitted

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
TEST_TIMEOUT = 180

LIB = "flox-connectors"

BYBIT_CONN = "connectors/src/bybit/bybit_exchange_connector.cpp"
BITGET_CONN = "connectors/src/bitget/bitget_exchange_connector.cpp"
HL_CONN = "connectors/src/hyperliquid/hyperliquid_exchange_connector.cpp"
POLY_CONN = "connectors/src/polymarket/polymarket_exchange_connector.cpp"
HL_SIGNER = "connectors/src/hyperliquid/hl_signer.cpp"
BASE_HDR = "include/flox/connector/abstract_exchange_connector.h"
POLY_CFG = "connectors/include/flox-connectors/polymarket/polymarket_config.h"
HL_HDR = "connectors/include/flox-connectors/hyperliquid/hyperliquid_exchange_connector.h"
SIGNERD = "connectors/utils/hl_signerd.py"

DISC = "unit_test_connector_health_disconnect"
STALE = "unit_test_connector_health_stale"
BYH = "unit_test_connector_health_bybit"
BGI = "unit_test_bitget_book_integrity"
PST = "unit_test_polymarket_snapshot_type"
PCF = "unit_test_polymarket_config_fields"
HLS = "unit_test_hl_signer_transport"

PYTEST_TARGET = "python/tests/test_hl_signerd_socket.py"

# Every offline connector test in the tree. A mutation that survives its own
# target is run against all of these before it is called green.
SWEEP = [
    BGI,
    "unit_test_bybit_connector_lifecycle",
    "unit_test_bybit_gap",
    "unit_test_bybit_option_symbol",
    "unit_test_bybit_private_stream_fills",
    BYH,
    DISC,
    STALE,
    "unit_test_fixed_point_parse",
    HLS,
    "unit_test_ix_ws_client_shutdown",
    "unit_test_ix_ws_client_stop_latency",
    "unit_test_order_serialization_bitget",
    "unit_test_order_serialization_bybit",
    "unit_test_order_serialization_hyperliquid",
    PCF,
    "unit_test_polymarket_delta",
    PST,
    "unit_test_polymarket_timestamps",
]

# Which library objects a mutated file feeds. A header is listed against every
# .cpp that includes it, because the library object is what the test links.
CONNECTOR_SOURCES = [
    "bybit_exchange_connector.cpp",
    "bitget_exchange_connector.cpp",
    "hyperliquid_exchange_connector.cpp",
    "polymarket_exchange_connector.cpp",
]

SOURCES_OF = {
    BYBIT_CONN: ["bybit_exchange_connector.cpp"],
    BITGET_CONN: ["bitget_exchange_connector.cpp"],
    HL_CONN: ["hyperliquid_exchange_connector.cpp"],
    POLY_CONN: ["polymarket_exchange_connector.cpp"],
    HL_SIGNER: ["hl_signer.cpp"],
    BASE_HDR: CONNECTOR_SOURCES,
    POLY_CFG: ["polymarket_exchange_connector.cpp"],
    HL_HDR: ["hyperliquid_exchange_connector.cpp"],
    SIGNERD: [],
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
    # Further (old, new) edits applied with the same rules as old/new. Used
    # where one behavioural change cannot be expressed as a single hunk --
    # feeding the checksum parsed levels instead of the venue's own strings
    # needs somewhere to keep the re-rendered text alive.
    also: list[tuple[str, str]] = field(default_factory=list)
    # "cxx" builds and runs gtest binaries; "python" runs pytest on the file
    # named by `target` and skips the C++ sweep (no C++ binary compiles a .py).
    kind: str = "cxx"
    # A mutant proven to compute the same thing as the original on every
    # input. Surviving is then the correct outcome and not a hole: no test can
    # kill it, and one that appeared to would be asserting something other
    # than behaviour. It stays in the list so that the day the code changes
    # shape and the two stop being equivalent, the survival stops being
    # expected -- whoever sees EQUIVALENT here is pointed at the argument in
    # `why`.
    equivalent: bool = False


MUTATIONS: list[Mutation] = [
    # ---- the transport close nobody used to report -------------------------
    Mutation(
        name="bybit-disconnect-not-reported",
        why="Bybit's close handler goes back to logging only, so a supervisor never hears it",
        file=BYBIT_CONN,
        old="  emitDisconnect(detail);",
        new="  (void)detail;",
        target=DISC,
        test="ConnectorHealthDisconnect.BybitReportsATransportClose",
    ),
    Mutation(
        name="bitget-disconnect-not-reported",
        why="Bitget's close handler goes back to logging only",
        file=BITGET_CONN,
        old="  emitDisconnect(detail);",
        new="  (void)detail;",
        target=DISC,
        test="ConnectorHealthDisconnect.BitgetReportsATransportClose",
    ),
    Mutation(
        name="hyperliquid-disconnect-not-reported",
        why="Hyperliquid's close handler goes back to logging only",
        file=HL_CONN,
        old="  emitDisconnect(detail);",
        new="  (void)detail;",
        target=DISC,
        test="ConnectorHealthDisconnect.HyperliquidReportsATransportClose",
    ),
    Mutation(
        name="polymarket-disconnect-not-reported",
        why="Polymarket's close handler goes back to logging only",
        file=POLY_CONN,
        old="  emitDisconnect(detail);",
        new="  (void)detail;",
        target=DISC,
        test="ConnectorHealthDisconnect.PolymarketReportsATransportClose",
    ),
    Mutation(
        name="bitget-private-close-not-reported",
        why=("the private stream -- the one carrying order and execution reports -- stops "
             "reporting its close and goes back to a warn line, so fills can stop arriving "
             "with nothing on the health channel to say so"),
        file=BITGET_CONN,
        old="""          // The private stream carries order and execution reports: losing it
          // stops fills reaching the engine, so it is the same class of event
          // as losing the public book.
          handleDisconnect(code, std::string("private stream: ").append(reason));""",
        new="""          _logger->warn("[Bitget] Private WS closed: code=" + std::to_string(code) +
                        " reason=" + std::string(reason));""",
        target=DISC,
    ),
    Mutation(
        name="adv-bybit-private-close-not-reported",
        why="same on Bybit's private stream: the close is logged and never reported",
        file=BYBIT_CONN,
        old="""          // The private stream carries order and execution reports: losing it
          // stops fills reaching the engine, so it is the same class of event
          // as losing the public book.
          handleDisconnect(code, std::string("private stream: ").append(reason));""",
        new="""          _logger->info("[Bybit] Private WS closed: code=" + std::to_string(code) +
                        ", reason=" + std::string(reason));""",
        target=DISC,
    ),
    Mutation(
        name="adv-bybit-disconnect-reason-empty",
        why=("the event fires but carries nothing: a supervisor is told a feed went away and "
             "not which code or reason, which is the whole content of the report"),
        file=BYBIT_CONN,
        old="  emitDisconnect(detail);",
        new="  (void)detail;\n  emitDisconnect(\"\");",
        target=DISC,
        test="ConnectorHealthDisconnect.BybitReportsATransportClose",
    ),
    Mutation(
        name="adv-bybit-disconnect-emitted-twice",
        why=("one close produces two events, so a supervisor counting disconnects to decide "
             "whether to fail a venue over doubles every transport blip"),
        file=BYBIT_CONN,
        old="  emitDisconnect(detail);",
        new="  emitDisconnect(detail);\n  emitDisconnect(detail);",
        target=DISC,
        test="ConnectorHealthDisconnect.BybitReportsATransportClose",
    ),

    # ---- Bybit's book gap as an event --------------------------------------
    Mutation(
        name="bybit-gap-not-emitted",
        why="the gap is detected, dropped and logged again, and never reaches emitSequenceGap",
        file=BYBIT_CONN,
        old="          emitSequenceGap(expected, static_cast<uint64_t>(updateId));",
        new="          (void)expected;",
        target=BYH,
        test="BybitFeedHealth.BookGapEmitsTheSequenceGapEvent",
    ),
    Mutation(
        name="bybit-baseline-less-gap-expects-one",
        why=("a delta with no baseline reports expected=1 instead of 0: there was no id to "
             "have expected, and 1 is a real update id, so the consumer is told a specific "
             "frame was skipped when none was"),
        file=BYBIT_CONN,
        old="              seqState.lastUpdateId < 0 ? 0u : static_cast<uint64_t>(seqState.lastUpdateId + 1);",
        new="              seqState.lastUpdateId < 0 ? 1u : static_cast<uint64_t>(seqState.lastUpdateId + 1);",
        target=BYH,
        test="BybitFeedHealth.DeltaWithNoBaselineEmitsTheSequenceGapEvent",
    ),
    Mutation(
        name="adv-bybit-gap-arguments-swapped",
        why=("expected and received change places, so the consumer reads the hole backwards "
             "and any 'how many ids did we lose' arithmetic on it underflows"),
        file=BYBIT_CONN,
        old="          emitSequenceGap(expected, static_cast<uint64_t>(updateId));",
        new="          emitSequenceGap(static_cast<uint64_t>(updateId), expected);",
        target=BYH,
        test="BybitFeedHealth.BookGapEmitsTheSequenceGapEvent",
    ),

    # ---- the quiet feed ----------------------------------------------------
    Mutation(
        name="stale-zero-window-does-not-disable",
        why=("a window of 0 stops meaning 'off': every connector ships 0 by default, so this "
             "turns the check on for every deployment that never configured one"),
        file=BASE_HDR,
        old="    if (timeoutMs <= 0)",
        new="    if (timeoutMs < 0)",
        target=STALE,
        test="ConnectorHealthStale.ZeroWindowDisablesTheCheck",
    ),
    Mutation(
        name="stale-reported-every-poll",
        why=("the once-per-episode latch is dropped on the read side, so a supervisor polling "
             "at 1 Hz gets one event per second per dead feed instead of one per episode"),
        file=BASE_HDR,
        old="        if (state.reported || now.raw() <= state.lastUpdate.raw() ||",
        new="        if (now.raw() <= state.lastUpdate.raw() ||",
        target=STALE,
    ),
    Mutation(
        name="stale-fresh-data-does-not-rearm",
        why=("fresh data no longer re-arms the report, so a feed that died, was reported, and "
             "came back is never reported again the next time it dies"),
        file=BASE_HDR,
        old="    state.reported = false;  // fresh data re-arms the report",
        new="",
        target=STALE,
    ),
    Mutation(
        name="adv-stale-window-silently-longer",
        why=("the configured window is multiplied by 1.9 on the way in, so a desk that asked "
             "for 1s of silence is told at 1.9s -- the check still fires, just too late"),
        file=BASE_HDR,
        old="    const uint64_t windowNs = static_cast<uint64_t>(timeoutMs) * 1'000'000ULL;",
        new="    const uint64_t windowNs = static_cast<uint64_t>(timeoutMs) * 1'900'000ULL;",
        target=STALE,
    ),
    Mutation(
        name="adv-stale-reports-nanoseconds-not-millis",
        why=("the callback's lastUpdateMs argument is handed raw nanoseconds, so anything "
             "computing an age from it is off by a factor of a million"),
        file=BASE_HDR,
        old="        stale.emplace_back(symbol, state.lastUpdate.raw() / 1'000'000ULL);",
        new="        stale.emplace_back(symbol, state.lastUpdate.raw());",
        target=STALE,
    ),
    Mutation(
        name="adv-stale-reports-only-the-first-symbol",
        why=("the scan stops after the first symbol it reports, so a venue that loses ten "
             "feeds at once reports one and the other nine stay silent"),
        file=BASE_HDR,
        old="""        state.reported = true;
        stale.emplace_back(symbol, state.lastUpdate.raw() / 1'000'000ULL);""",
        new="""        state.reported = true;
        stale.emplace_back(symbol, state.lastUpdate.raw() / 1'000'000ULL);
        break;""",
        target=STALE,
    ),
    Mutation(
        name="bybit-start-does-not-seed-subscribed-symbols",
        why=("start() stops stamping the subscribed symbols, so a feed that never delivers a "
             "single frame has no entry to age out from and is never reported stale -- the "
             "loudest of the two failures this check exists for"),
        file=BYBIT_CONN,
        old="""  const MonoNanos startedAt = nowMonoNanos();
  for (const auto& entry : _config.symbols)
  {
    markFeedActivity(resolveSymbolId(entry.name), startedAt);
  }
""",
        new="",
        target=STALE,
    ),
    Mutation(
        name="bitget-stamp-taken-after-parsing",
        why=("the arrival stamp is taken at the end of the book branch rather than on entry, "
             "so the feed always looks fresher than it is by the cost of the parse"),
        file=BITGET_CONN,
        old="\n      markFeedActivity(sid, arrivedAt);",
        new="\n      markFeedActivity(sid, nowMonoNanos());",
        target=STALE,
    ),
    Mutation(
        name="adv-polymarket-price-change-not-stamped",
        why=("a Polymarket token whose book frame arrived once and which then only ever "
             "receives price_change deltas is reported stale forever, because only the "
             "snapshot path stamps activity"),
        file=POLY_CONN,
        old="""    const SymbolId sym = resolveSymbolId(tokenId);
    markFeedActivity(sym, MonoNanos::fromRaw(recvNs));
    changes.push_back({sym, isBid, *priceOpt, *qtyOpt});""",
        new="""    const SymbolId sym = resolveSymbolId(tokenId);
    changes.push_back({sym, isBid, *priceOpt, *qtyOpt});""",
        target=STALE,
    ),

    # ---- Bitget book integrity ---------------------------------------------
    Mutation(
        name="bitget-seq-gap-not-counted",
        why="a sequence break stops incrementing bookGapCount, so the hole leaves no trace",
        file=BITGET_CONN,
        old="    _bookGapCount.fetch_add(1, std::memory_order_relaxed);",
        new="",
        target=BGI,
        test="BitgetBookIntegrity.SequenceGapIsDetectedAndReported",
    ),
    Mutation(
        name="bitget-seq-gap-not-emitted",
        why="the sequence break is counted and dropped but never reaches emitSequenceGap",
        file=BITGET_CONN,
        old="    emitSequenceGap(expected, static_cast<uint64_t>(seq));",
        new="    (void)expected;",
        target=BGI,
        test="BitgetBookIntegrity.SequenceGapIsDetectedAndReported",
    ),
    Mutation(
        name="bitget-checksum-failure-does-not-invalidate",
        why=("a book that failed its checksum is not marked invalid, so the deltas that follow "
             "are applied on top of levels the venue already told us are wrong"),
        file=BITGET_CONN,
        old="""        state.lastSeq = -1;
        state.invalid = true;
        resubscribeBook(instId);""",
        new="""        state.lastSeq = -1;
        resubscribeBook(instId);""",
        target=BGI,
        test="BitgetBookIntegrity.InvalidBookSuppressesDeltasUntilAFreshSnapshot",
    ),
    Mutation(
        name="bitget-checksum-over-parsed-levels",
        why=("the checksum is computed over the connector's own rendering of the parsed "
             "fixed-point values instead of the venue's strings, so every book whose text does "
             "not round-trip through the parser fails a checksum that was in fact correct"),
        file=BITGET_CONN,
        old="""      std::vector<RawLevel> rawBids;
      std::vector<RawLevel> rawAsks;""",
        new="""      std::vector<RawLevel> rawBids;
      std::vector<RawLevel> rawAsks;
      std::vector<std::pair<std::string, std::string>> parsedStore;
      parsedStore.reserve(512);""",
        also=[
            ("""            rawBids.emplace_back(p, q);""",
             """            parsedStore.emplace_back(priceOpt->toString(), qtyOpt->toString());
            rawBids.emplace_back(parsedStore.back().first, parsedStore.back().second);"""),
            ("""            rawAsks.emplace_back(p, q);""",
             """            parsedStore.emplace_back(priceOpt->toString(), qtyOpt->toString());
            rawAsks.emplace_back(parsedStore.back().first, parsedStore.back().second);"""),
        ],
        target=BGI,
        test="BitgetBookIntegrity.MatchingChecksumIsPublished",
    ),
    Mutation(
        name="bitget-checksum-over-20-levels",
        why=("the checksum covers the first 20 levels instead of the 25 the venue defines, so "
             "every books-channel frame deeper than 20 levels fails a correct checksum and "
             "the connector resyncs forever"),
        file=BITGET_CONN,
        old="static constexpr size_t BITGET_CHECKSUM_LEVELS = 25;",
        new="static constexpr size_t BITGET_CHECKSUM_LEVELS = 20;",
        target=BGI,
    ),
    Mutation(
        name="bitget-checksum-interleaving-swapped",
        why=("the two sides are interleaved ask-first, which is not the order the venue hashes, "
             "so a sound book fails its checksum"),
        file=BITGET_CONN,
        old="    for (const auto* side : {&bids, &asks})",
        new="    for (const auto* side : {&asks, &bids})",
        target=BGI,
        test="BitgetBookIntegrity.MatchingChecksumIsPublished",
    ),
    Mutation(
        name="bitget-checksum-failure-does-not-resubscribe",
        why=("the book is invalidated but never re-subscribed, so no fresh snapshot is ever "
             "asked for and the symbol stays dark until the next full reconnect"),
        file=BITGET_CONN,
        old="""        state.invalid = true;
        resubscribeBook(instId);""",
        new="""        state.invalid = true;""",
        target=BGI,
    ),
    Mutation(
        name="bitget-deltas-checked-on-checksum",
        why=("deltas are failed against their own checksum, which covers the merged book this "
             "connector never builds, so every delta that carries one invalidates a healthy "
             "book and forces a resync loop"),
        file=BITGET_CONN,
        old="""  if (state.invalid)
  {
    // Already reported when the book was invalidated; deltas racing the""",
        new="""  if (venueChecksum)
  {
    const uint32_t computed = bitgetBookChecksum(bids, asks);
    if (computed != *venueChecksum)
    {
      _bookChecksumFailureCount.fetch_add(1, std::memory_order_relaxed);
      state.lastSeq = -1;
      state.invalid = true;
      emitSequenceGap(computed, *venueChecksum);
      return false;
    }
  }

  if (state.invalid)
  {
    // Already reported when the book was invalidated; deltas racing the""",
        target=BGI,
    ),
    Mutation(
        name="adv-bitget-gap-arguments-swapped",
        why=("expected and received change places on Bitget's sequence gap, so the consumer "
             "reads the hole backwards"),
        file=BITGET_CONN,
        old="    emitSequenceGap(expected, static_cast<uint64_t>(seq));",
        new="    emitSequenceGap(static_cast<uint64_t>(seq), expected);",
        target=BGI,
    ),
    Mutation(
        name="adv-bitget-checksum-sign-cast-dropped",
        why=("the venue's signed checksum is narrowed straight to uint32_t instead of through "
             "int32_t. EQUIVALENT: both conversions are reduction modulo 2^32 of the same "
             "int64 value, so the 32 bits compared are identical for every value the venue "
             "can send; the int32 hop only documents that the field is signed on the wire. "
             "The day the comparison stops being on the low 32 bits this stops being "
             "equivalent and the survival stops being expected"),
        file=BITGET_CONN,
        old="            venueChecksum = static_cast<uint32_t>(static_cast<int32_t>(v));",
        new="            venueChecksum = static_cast<uint32_t>(v);",
        target=BGI,
        equivalent=True,
    ),

    # ---- the Polymarket snapshot type --------------------------------------
    Mutation(
        name="polymarket-snapshot-type-left-to-the-slot",
        why=("the type is left to whatever the pooled event last carried, so a snapshot that "
             "lands on a slot used by a price_change is published as DELTA and merged into "
             "the book it was meant to replace"),
        file=POLY_CONN,
        old="  ev->update.type = BookUpdateType::SNAPSHOT;",
        new="",
        target=PST,
        test="PolymarketSnapshotType.SnapshotOnARecycledSlotIsStillTypedSnapshot",
    ),

    # ---- the config fields nothing reads -----------------------------------
    Mutation(
        name="polymarket-privatekey-restored",
        why=("the dead privateKey field comes back: a plaintext credential in the public API "
             "that no code path consumes"),
        file=POLY_CFG,
        old='  std::string wsEndpoint{"wss://ws-subscriptions-clob.polymarket.com/ws/market"};',
        new=('  std::string wsEndpoint{"wss://ws-subscriptions-clob.polymarket.com/ws/market"};\n'
             "  std::string privateKey;"),
        target=PCF,
        test="PolymarketConfigFields.DeadCredentialFieldsAreGone",
    ),
    Mutation(
        name="polymarket-funderwallet-restored",
        why="the dead funderWallet field comes back",
        file=POLY_CFG,
        old='  std::string wsEndpoint{"wss://ws-subscriptions-clob.polymarket.com/ws/market"};',
        new=('  std::string wsEndpoint{"wss://ws-subscriptions-clob.polymarket.com/ws/market"};\n'
             "  std::string funderWallet;"),
        target=PCF,
        test="PolymarketConfigFields.DeadCredentialFieldsAreGone",
    ),
    Mutation(
        name="polymarket-restendpoint-restored",
        why="the dead restEndpoint field comes back on a WS-only connector",
        file=POLY_CFG,
        old='  std::string wsEndpoint{"wss://ws-subscriptions-clob.polymarket.com/ws/market"};',
        new=('  std::string wsEndpoint{"wss://ws-subscriptions-clob.polymarket.com/ws/market"};\n'
             '  std::string restEndpoint{"https://clob.polymarket.com"};'),
        target=PCF,
        test="PolymarketConfigFields.DeadRestEndpointIsGone",
    ),
    Mutation(
        name="hyperliquid-restendpoint-restored",
        why="the dead restEndpoint field comes back on a WS-only connector",
        file=HL_HDR,
        old='  std::string wsEndpoint{"wss://api.hyperliquid.xyz/ws"};',
        new=('  std::string wsEndpoint{"wss://api.hyperliquid.xyz/ws"};\n'
             '  std::string restEndpoint{"https://api.hyperliquid.xyz/exchange"};'),
        target=PCF,
        test="HyperliquidConfigFields.DeadRestEndpointIsGone",
    ),

    # ---- the signer transport ----------------------------------------------
    Mutation(
        name="hl-signer-tcp-fallback-restored",
        why=("the loopback TCP fallback comes back, so with no trusted Unix socket the raw "
             "private key is written to whoever bound 127.0.0.1:19847 first"),
        file=HL_SIGNER,
        old="""static socket_t connect_unix(const char* path, int timeout_ms = 50)
{""",
        new="""static socket_t connect_tcp(uint16_t port, int timeout_ms = 50)
{
  socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == INVALID_SOCK)
  {
    return INVALID_SOCK;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
  {
    close_socket(fd);
    return INVALID_SOCK;
  }
  return fd;
}

static socket_t connect_unix(const char* path, int timeout_ms = 50)
{""",
        also=[
            ("""  if (!is_private_signer_socket(path.c_str()))
  {
    return std::nullopt;
  }

  // Built only once the transport is known to be trustworthy: no reason to
  // have the key in a buffer we might never be allowed to send.
  const std::string req = build_request_json(p);

  socket_t fd = connect_unix(path.c_str(), /*timeout_ms=*/50);
  if (fd == INVALID_SOCK)
  {""",
             """  const std::string req = build_request_json(p);

  socket_t fd = INVALID_SOCK;
  if (is_private_signer_socket(path.c_str()))
  {
    fd = connect_unix(path.c_str(), /*timeout_ms=*/50);
  }
  if (fd == INVALID_SOCK)
  {
    fd = connect_tcp(19847, /*timeout_ms=*/50);
  }
  if (fd == INVALID_SOCK)
  {"""),
        ],
        target=HLS,
        test="HlSignerTransport.NeverSendsThePrivateKeyOverLoopbackTcp",
    ),
    Mutation(
        name="hl-signer-lstat-replaced-by-stat",
        why=("the path check follows symlinks, so a symlink this user owns pointing at a "
             "socket someone else controls passes every test below it"),
        file=HL_SIGNER,
        old="  if (::lstat(path, &st) != 0)",
        new="  if (::stat(path, &st) != 0)",
        target=HLS,
    ),
    Mutation(
        name="hl-signer-group-bit-allowed",
        why=("group access is accepted, so any member of the socket's group can open the "
             "signer and harvest the key"),
        file=HL_SIGNER,
        old="  if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0)",
        new="  if ((st.st_mode & S_IRWXO) != 0)",
        target=HLS,
    ),
    Mutation(
        name="hl-signer-owner-check-removed",
        why=("the socket's owner is no longer checked, so a 0600 socket belonging to another "
             "user is trusted with the key"),
        file=HL_SIGNER,
        old="""  if (st.st_uid != ::geteuid())
  {
    FLOX_LOG_ERROR("[HL] signer socket is owned by another user, refusing: " << path);
    return false;
  }
""",
        new="",
        target=HLS,
    ),
    Mutation(
        name="hl-signer-reply-bound-raised",
        why=("the ceiling on the peer-declared reply length is raised to half a gigabyte, "
             "which is the unbounded resize in a thin disguise"),
        file=HL_SIGNER,
        old="static constexpr uint32_t HL_SIGNER_MAX_RESPONSE = 4096;",
        new="static constexpr uint32_t HL_SIGNER_MAX_RESPONSE = 512u * 1024u * 1024u;",
        target=HLS,
        test="HlSignerTransport.RefusesAResponseLengthAboveTheBound",
    ),
    Mutation(
        name="hl-signer-request-built-before-transport-check",
        why=("the request -- private key and all -- is serialised before the transport is "
             "known to be trustworthy, so the key sits in a heap buffer on every call, "
             "including the ones that refuse to send"),
        file=HL_SIGNER,
        old="""  if (!is_private_signer_socket(path.c_str()))
  {
    return std::nullopt;
  }

  // Built only once the transport is known to be trustworthy: no reason to
  // have the key in a buffer we might never be allowed to send.
  const std::string req = build_request_json(p);""",
        new="""  const std::string req = build_request_json(p);

  if (!is_private_signer_socket(path.c_str()))
  {
    return std::nullopt;
  }""",
        target=HLS,
    ),
    Mutation(
        name="adv-hl-signer-accepts-a-root-owned-socket",
        why=("a socket owned by root is trusted as well as one owned by this user, so any "
             "process that can drop a 0600 socket at the configured path as root -- or any "
             "root-owned daemon that is not the signer -- receives the key"),
        file=HL_SIGNER,
        old="  if (st.st_uid != ::geteuid())",
        new="  if (st.st_uid != ::geteuid() && st.st_uid != 0)",
        target=HLS,
    ),

    # ---- the signing daemon ------------------------------------------------
    Mutation(
        name="hl-signerd-umask-removed",
        why=("bind() goes back to creating the socket node under the ambient umask with the "
             "chmod afterwards, which leaves a window in which the node exists on the "
             "filesystem world-writable"),
        file=SIGNERD,
        old="""    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    old_umask = os.umask(0o177)
    try:
        srv.bind(path)
    finally:
        os.umask(old_umask)
    os.chmod(path, 0o600)""",
        new="""    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    os.chmod(path, 0o600)""",
        target=PYTEST_TARGET,
        kind="python",
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


def apply_mutation(text: str, m: Mutation) -> str:
    out = replace_occurrence(text, m.old, m.new, m.occurrence, m.expected_occurrences)
    for old, new in m.also:
        out = replace_occurrence(out, old, new, 1, 1)
    return out


def find_objects(pattern_path: str, name: str | None = None) -> list[Path]:
    cmd = ["find", str(BUILD), "-type", "f", "-name", name or "*.o", "-path", pattern_path]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.split()
    return [Path(p) for p in out]


def target_objects(target: str) -> list[Path]:
    """The test target's own object files, located the way `find` would."""
    return find_objects(f"*{target}.dir*")


def library_objects(source_file: str) -> list[Path]:
    """The mutated source's objects inside the flox-connectors library.

    Connector sources do not compile into the test binary: they compile into
    libflox-connectors.a, so the library object is what has to go. A header is
    mapped to every .cpp that includes it.
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


def run_pytest(path: str) -> tuple[int, str]:
    try:
        result = subprocess.run(
            [sys.executable, "-m", "pytest", path, "-q"],
            capture_output=True, text=True, timeout=TEST_TIMEOUT, cwd=REPO,
        )
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {TEST_TIMEOUT}s"
    return result.returncode, result.stdout + result.stderr


def control(targets: list[str], pytest_targets: list[str]) -> bool:
    ok = True
    if targets:
        for target in targets:
            # Deleted first so the control binary is compiled from the source
            # as it stands right now, not served from whatever the last run
            # left behind.
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
    for path in pytest_targets:
        code, out = run_pytest(path)
        state = "green" if code == 0 else "RED"
        summary = out.strip().splitlines()[-1] if out.strip() else ""
        print(f"  control {path:<42} {state}   {summary}")
        ok = ok and code == 0
    return ok


def run_python_mutation(m: Mutation) -> tuple[str, str]:
    code, out = run_pytest(m.target)
    print(f"  pytest {m.target} -> exit {code} ({'RED' if code else 'green'})")
    if code != 0:
        return "RED", f"killed by {m.target}"
    # No C++ sweep: nothing in the tree compiles the daemon, so no connector
    # binary could notice a change to it. The whole pytest file above is the
    # sweep.
    print("  no C++ binary compiles the daemon, so the pytest file above is the whole sweep")
    return "EQUIVALENT" if m.equivalent else "GREEN", "survived the daemon's own pytest file"


def run_mutation(m: Mutation) -> tuple[str, str]:
    """Returns (verdict, detail). Verdict is RED, GREEN, EQUIVALENT or NOCOMPILE."""
    path = REPO / m.file
    original = path.read_text()
    before = sha256(path)
    primary_targets = [] if m.kind == "python" else [m.target, *m.extra_targets]

    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    print(f"  file    {m.file}")
    print(f"  sha256  before  {before}")

    mutated = apply_mutation(original, m)
    if mutated == original:
        raise SystemExit("mutation changed nothing")
    path.write_text(mutated)
    print(f"  sha256  mutated {sha256(path)}")

    verdict = "GREEN"
    detail = ""
    try:
        if m.kind == "python":
            verdict, detail = run_python_mutation(m)
            return verdict, detail

        removed = delete_objects(m.file, primary_targets)
        print(f"  removed {removed} object file(s): the library object(s) for the mutated "
              f"source and the objects of {', '.join(primary_targets)}")

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
            # Nothing its own target noticed. Every other offline connector
            # test gets a turn before this is called a hole in the suite.
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
        if m.kind != "python":
            delete_objects(m.file, primary_targets)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.name:<48} {m.target:<42} {m.test or '(whole binary)'}")
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
    control_targets = sorted({t for m in selected if m.kind == "cxx"
                              for t in [m.target, *m.extra_targets]})
    control_pytests = sorted({m.target for m in selected if m.kind == "python"})

    print("control run before the mutations")
    if not control(control_targets, control_pytests):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, *run_mutation(m)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(control_targets, control_pytests)

    print("\nsummary")
    print(f"  {'verdict':<10} {'mutation':<48} {'target':<42} detail")
    for m, verdict, detail in results:
        print(f"  {verdict:<10} {m.name:<48} {m.target:<42} {detail}")

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
