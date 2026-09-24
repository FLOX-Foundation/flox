#!/usr/bin/env python3
"""Mutation harness for the FIX codec strictness fix (TimeInForce, the
ClOrdID/OrigClOrdID/Account/Symbol string-id fields, GTD's ExpireTime, and the
quoting order-id block).

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks that fix one piece at a time,
in the source, and checks whether the tests written for it go red -- or stay
green, which is a hole in what they cover.

Four test binaries answer for every mutation, and a mutation is killed if ANY
of them goes red:

    test_venue_fix_codec_strictness   the acceptance tests
    test_venue_fix_field_parse        the code agent's own parser unit tests
    test_venue_fix_codec_contract     the code agent's own codec-edge tests
    test_venue_fix_mass_quote         pre-existing MassQuote/QuoteCancel tests

Every run is honest about the build: the mutated file's hash is printed before
and after, every target's object files are deleted so nothing can be served
from cache, the rebuild output has to contain "Building CXX" or the run is
refused, and each binary runs under a timeout. A mutation that does not
compile is not a mutation and is reported as such.

Usage:

    python3 scripts/mutations/fix_codec.py            # control, mutations, control
    python3 scripts/mutations/fix_codec.py --list
    python3 scripts/mutations/fix_codec.py --only tif-unknown-mapped-to-gtc-again

The build directory is expected to be configured already:

    cmake --preset venue-lite
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
BUILD = REPO / "build-venue-lite"
BIN_DIR = BUILD / "venue"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 900
TEST_TIMEOUT = 120

CODEC = "venue/include/flox-venue/fix_codec.h"
FIELD_PARSE = "venue/include/flox-venue/fix_field_parse.h"
SESSION = "venue/include/flox-venue/fix_session.h"

STRICT = "test_venue_fix_codec_strictness"
FIELDS = "test_venue_fix_field_parse"
CONTRACT = "test_venue_fix_codec_contract"
MASSQ = "test_venue_fix_mass_quote"
ALL4 = [STRICT, FIELDS, CONTRACT, MASSQ]


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
    file: str = ""
    old: str = ""
    new: str = ""
    edits: list[Edit] = field(default_factory=list)
    targets: list[str] = field(default_factory=lambda: list(ALL4))
    gtest_filter: str | None = None
    occurrence: int = 1
    expected_occurrences: int = 1

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
    # ---- fix_field_parse.h: parseU64/parseU32 -----------------------------
    Mutation(
        name="parseu64-accepts-leading-plus",
        why="a leading '+' is stripped before the digit scan, the way strtoull's sign "
            "handling used to let it through",
        file=FIELD_PARSE,
        old="""inline bool parseU64(std::string_view t, uint64_t& out) noexcept
{
  if (t.empty() || (t.size() > 1 && t[0] == '0'))
  {
    return false;
  }
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();""",
        new="""inline bool parseU64(std::string_view t, uint64_t& out) noexcept
{
  if (t.empty() || (t.size() > 1 && t[0] == '0'))
  {
    return false;
  }
  if (!t.empty() && t[0] == '+')
  {
    t.remove_prefix(1);
  }
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();""",
    ),
    Mutation(
        name="parseu64-accepts-leading-space",
        why="a single leading space is stripped before the digit scan, the way strtoull "
            "skips whitespace",
        file=FIELD_PARSE,
        old="""inline bool parseU64(std::string_view t, uint64_t& out) noexcept
{
  if (t.empty() || (t.size() > 1 && t[0] == '0'))
  {
    return false;
  }
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();""",
        new="""inline bool parseU64(std::string_view t, uint64_t& out) noexcept
{
  if (t.empty() || (t.size() > 1 && t[0] == '0'))
  {
    return false;
  }
  if (!t.empty() && t[0] == ' ')
  {
    t.remove_prefix(1);
  }
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();""",
    ),
    Mutation(
        name="parseu64-accepts-trailing-non-digit",
        why="the scan stops at the first non-digit instead of refusing, so \"123ABC\" "
            "parses as 123 the way strtoull stops and reports nothing",
        file=FIELD_PARSE,
        old="""    if (c < '0' || c > '9')
    {
      return false;
    }
    const uint64_t d = static_cast<uint64_t>(c - '0');""",
        new="""    if (c < '0' || c > '9')
    {
      break;
    }
    const uint64_t d = static_cast<uint64_t>(c - '0');""",
    ),
    Mutation(
        name="parseu64-accepts-empty-as-zero",
        why="the empty-string guard is dropped, so an absent value with zero digits scanned "
            "returns v=0 as if the sender wrote \"0\"",
        file=FIELD_PARSE,
        old="""  if (t.empty() || (t.size() > 1 && t[0] == '0'))
  {
    return false;
  }""",
        new="""  if (t.size() > 1 && t[0] == '0')
  {
    return false;
  }""",
    ),
    Mutation(
        name="parseu64-overflow-wraps",
        why="the pre-multiply overflow guard is removed, so a 20-digit value wraps through "
            "normal unsigned overflow instead of being refused",
        file=FIELD_PARSE,
        old="""    const uint64_t d = static_cast<uint64_t>(c - '0');
    if (v > (kMax - d) / 10)
    {
      return false;  // the next multiply would wrap
    }
    v = v * 10 + d;""",
        new="""    const uint64_t d = static_cast<uint64_t>(c - '0');
    v = v * 10 + d;""",
    ),
    Mutation(
        name="parseu32-truncates-instead-of-refusing",
        why="the width guard above UINT32_MAX is dropped, so a Symbol like 4294967296 "
            "truncates onto 0 -- someone else's book -- instead of being refused",
        file=FIELD_PARSE,
        old="""  uint64_t v = 0;
  if (!parseU64(t, v) || v > std::numeric_limits<uint32_t>::max())
  {
    return false;
  }
  out = static_cast<uint32_t>(v);""",
        new="""  uint64_t v = 0;
  if (!parseU64(t, v))
  {
    return false;
  }
  out = static_cast<uint32_t>(v);""",
    ),
    Mutation(
        name="parseu64-leading-zero-accepted",
        why="the leading-zero guard is dropped, so \"0042\" and \"42\" fold onto the same id "
            "-- the collision this parse exists to prevent, from the padding side",
        file=FIELD_PARSE,
        old="""  if (t.empty() || (t.size() > 1 && t[0] == '0'))
  {
    return false;
  }""",
        new="""  if (t.empty())
  {
    return false;
  }""",
    ),
    # ---- fix_field_parse.h: parseUtcTimestampNs ----------------------------
    Mutation(
        name="expiretime-month-off-by-one",
        why="the parsed month is passed to the civil-to-days arithmetic minus one, treating "
            "it as zero-indexed the way a common calendar-library bug does",
        file=FIELD_PARSE,
        old="  const int64_t days = detail::daysFromCivil(year, month, day);",
        new="  const int64_t days = detail::daysFromCivil(year, month - 1, day);",
    ),
    Mutation(
        name="expiretime-milliseconds-dropped",
        why="the parsed .sss milliseconds are computed but never added into the nanosecond "
            "result, so a GTD expiry loses sub-second precision silently",
        file=FIELD_PARSE,
        old="  outNs = secs * 1'000'000'000 + static_cast<int64_t>(millis) * 1'000'000;",
        new="  outNs = secs * 1'000'000'000;",
    ),
    Mutation(
        name="expiretime-fixed-local-offset",
        why="a constant one-hour offset is folded into the result, the mktime/localtime "
            "failure mode this parser exists to rule out, pinned to one zone instead of "
            "reading the host's",
        file=FIELD_PARSE,
        old="  outNs = secs * 1'000'000'000 + static_cast<int64_t>(millis) * 1'000'000;\n  return true;",
        new="  outNs = secs * 1'000'000'000 + static_cast<int64_t>(millis) * 1'000'000 + "
            "3600LL * 1'000'000'000;\n  return true;",
    ),
    # ---- fix_codec.h: TimeInForce -------------------------------------------
    Mutation(
        name="tif-unknown-mapped-to-gtc-again",
        why="the final refusal branch is reverted to the pre-fix mapping: anything not 1/3/4/6 "
            "rests as GTC instead of being refused",
        file=CODEC,
        old="""        else
        {
          return refuse("TimeInForce", 59,
                        "names no time in force this venue runs: 1 GTC, 3 IOC, 4 FOK, 6 GTD");
        }""",
        new="""        else
        {
          o.tif = TimeInForce::GTC;
        }""",
    ),
    Mutation(
        name="tif-59-2-accepted",
        why="59=2 (AtTheOpening) alone is folded back onto GTC, the literal review finding, "
            "leaving 59=7/9/X and the rest still refused",
        file=CODEC,
        old="""        if (tif == "1")
        {
          o.tif = TimeInForce::GTC;
        }""",
        new="""        if (tif == "1" || tif == "2")
        {
          o.tif = TimeInForce::GTC;
        }""",
    ),
    Mutation(
        name="gtd-without-expiretime-accepted-as-zero",
        why="the required-126 gate is dropped: 59=6 with no ExpireTime decodes as GTD with "
            "expiryNs left at its default 0 instead of being refused",
        file=CODEC,
        old="""          o.tif = TimeInForce::GTD;
          if (!has(126))
          {
            return refuse("ExpireTime", 126, "required by TimeInForce 6 (GTD)");
          }
          int64_t expiry = 0;
          if (!fixfield::parseUtcTimestampNs(s(126), expiry))
          {
            return refuse("ExpireTime", 126,
                          "not a UTC FIX UTCTimestamp, YYYYMMDD-HH:MM:SS with optional .sss");
          }
          // The one legitimate crossing into sequencer time: SeqNanos is
          // captured from the wall clock at ingestion, and an expiry the
          // client wrote as a UTC instant is a wall-clock instant until the
          // sequencer stamps it.
          o.expiryNs = SeqNanos::fromRaw(expiry);""",
        new="""          o.tif = TimeInForce::GTD;
          if (has(126))
          {
            int64_t expiry = 0;
            if (!fixfield::parseUtcTimestampNs(s(126), expiry))
            {
              return refuse("ExpireTime", 126,
                            "not a UTC FIX UTCTimestamp, YYYYMMDD-HH:MM:SS with optional .sss");
            }
            o.expiryNs = SeqNanos::fromRaw(expiry);
          }""",
    ),
    # ---- fix_codec.h: reason quality -----------------------------------------
    Mutation(
        name="reason-empty-on-side-refusal",
        why="the Side(54) refusal drops straight to nullopt without going through refuse(), "
            "leaving *reason untouched -- Side is never exercised by these four binaries with "
            "an invalid value, so an empty reason here is invisible to them",
        file=CODEC,
        old="""      else
      {
        return refuse("Side", 54, "required, and must be 1 (Buy) or 2 (Sell)");
      }""",
        new="""      else
      {
        return std::nullopt;
      }""",
    ),
    Mutation(
        name="reason-names-the-wrong-tag",
        why="the ClOrdID refusal names tag 41 (OrigClOrdID's tag) instead of 11",
        file=CODEC,
        old="""      if (!has(11) || !u64(11, o.id))
      {
        return refuse("ClOrdID", 11, "required, and must be a decimal integer below 2^64");
      }""",
        new="""      if (!has(11) || !u64(11, o.id))
      {
        return refuse("ClOrdID", 41, "required, and must be a decimal integer below 2^64");
      }""",
    ),
    # ---- fix_codec.h: the quoting id block -----------------------------------
    Mutation(
        name="id-fold-account-symbol-swapped",
        why="account and symbol trade places in the fold -- symbol in the high bits, account "
            "in the low 32 -- which is still injective but not what the block-adjacency "
            "contract promises",
        file=CODEC,
        old="""    return static_cast<OrderId>(
        kMarker + (((accountId << kSymbolBits) | static_cast<uint64_t>(symbol)) * kBlock));""",
        new="""    return static_cast<OrderId>(
        kMarker + (((static_cast<uint64_t>(symbol) << kSymbolBits) | accountId) * kBlock));""",
    ),
    Mutation(
        name="account-limit-raised-past-documented-value",
        why="kQuoteAccountLimit moves from 2^24 (16,777,216 accounts, the number "
            "docs/venue/fix-quoting.md states) to 2^27 -- still inside the range the "
            "static_assert allows without wrapping, so nothing overflows, but the documented "
            "bound is now just wrong and nothing pins the literal",
        file=CODEC,
        old="  static constexpr uint64_t kQuoteAccountLimit = 1ULL << 24;",
        new="  static constexpr uint64_t kQuoteAccountLimit = 1ULL << 27;",
    ),
    Mutation(
        name="block-width-off-by-one-narrower",
        why="the block is one id narrower than 2*kQuoteLadderLevels, so adjacent symbols' "
            "blocks overlap by one leg",
        file=CODEC,
        old="    constexpr uint64_t kBlock = 2ULL * kQuoteLadderLevels;",
        new="    constexpr uint64_t kBlock = 2ULL * kQuoteLadderLevels - 1;",
    ),
    Mutation(
        name="out-of-range-account-refusal-removed-on-quotecancel-only",
        why="the range gate stays on MassQuote (35=i) but is deleted on QuoteCancel (35=Z), "
            "so an out-of-range account can still reach quoteLadderIdBase through a cancel",
        file=CODEC,
        old="""      if (!quoteIdBlockInRange(l.accountId))
      {
        return refuse("Account", 1,
                      "above the range a quoting id block is derived for (below 2^24)");
      }""",
        new="",
        occurrence=2,
        expected_occurrences=2,
    ),
    # ---- fix_session.h: the reason forwarded to Text(58) ---------------------
    Mutation(
        name="session-does-not-forward-the-codec-reason",
        why="the QuoteStatusReport Text always reads \"malformed\", the codec's specific "
            "reason is computed and then thrown away -- tag 58 is still present so a test "
            "that only checks f.count(58) cannot tell",
        file=SESSION,
        old="""      sendQuoteStatus(false, quoteId, sym,
                      "MassQuote/QuoteCancel rejected: " +
                          (reason.empty() ? std::string("malformed") : reason),
                      nowNs);""",
        new="""      sendQuoteStatus(false, quoteId, sym, "MassQuote/QuoteCancel rejected: malformed", nowNs);""",
    ),
    # ---- adversarial: ours -----------------------------------------------------
    Mutation(
        name="adv-one-arg-decode-diverges-on-tif",
        why="the TIF refusal only fires when a reason pointer was supplied: the one-argument "
            "decode() (reason==nullptr) silently falls back to GTC for any unknown "
            "TimeInForce while the two-argument overload the acceptance/contract suites call "
            "stays strict -- test_venue_fix_mass_quote calls only the one-argument decode() "
            "and never sends tag 59, so the divergence is invisible to all four binaries",
        file=CODEC,
        old="""        else
        {
          return refuse("TimeInForce", 59,
                        "names no time in force this venue runs: 1 GTC, 3 IOC, 4 FOK, 6 GTD");
        }""",
        new="""        else if (reason != nullptr)
        {
          return refuse("TimeInForce", 59,
                        "names no time in force this venue runs: 1 GTC, 3 IOC, 4 FOK, 6 GTD");
        }
        else
        {
          o.tif = TimeInForce::GTC;
        }""",
    ),
    Mutation(
        name="adv-modify-order-account-symbol-swapped",
        why="OrderCancelReplaceRequest (35=G) reads Account(1) into m.symbol and Symbol(55) "
            "into m.accountId -- none of the four binaries ever assert m->symbol or "
            "m->accountId on a ModifyOrder, only m->id, so a swap here is invisible",
        file=CODEC,
        old="""      if (has(55) && !sym(55, m.symbol))
      {
        return refuse("Symbol", 55, "must be a decimal integer below 2^32");
      }""",
        new="""      if (has(1) && !sym(1, m.symbol))
      {
        return refuse("Symbol", 55, "must be a decimal integer below 2^32");
      }""",
    ),
    Mutation(
        name="adv-modify-order-malformed-price-ignored",
        why="a malformed Price(44) on OrderCancelReplaceRequest is silently dropped (the "
            "amend keeps the resting price) instead of refused -- no test sends 35=G with a "
            "bad 44, cancelReplace() always sends a valid decimal",
        file=CODEC,
        old="""      if (has(44))
      {
        int64_t pr;
        if (!fix(44, pr))
        {
          return refuse("Price", 44, "not a plain decimal price");
        }
        m.newPrice = Price::fromRaw(pr);
      }""",
        new="""      if (has(44))
      {
        int64_t pr;
        if (fix(44, pr))
        {
          m.newPrice = Price::fromRaw(pr);
        }
      }""",
    ),
    Mutation(
        name="adv-cancel-order-malformed-symbol-ignored",
        why="a malformed Symbol(55) on OrderCancelRequest (35=F) is silently accepted as "
            "symbol 0 instead of refused -- cancelRequest() always sends a valid numeric "
            "symbol, so the refusal branch is never exercised for F specifically",
        file=CODEC,
        old="""      if (has(55) && !sym(55, c.symbol))
      {
        return refuse("Symbol", 55, "must be a decimal integer below 2^32");
      }""",
        new="""      if (has(55))
      {
        sym(55, c.symbol);
      }""",
    ),
    Mutation(
        name="adv-symbol-uint32-max-misfolded",
        why="the fold narrows to 31 bits for the symbol half instead of the full 32 SymbolId "
            "has, so a Symbol with its top bit set overlaps Account's low bit -- a sanity "
            "check that the boundary values in the acceptance/contract suites actually catch "
            "this rather than a real hole",
        file=CODEC,
        old="""    constexpr uint64_t kMarker = 0x51'00000000ULL;  // 'Q'
    constexpr uint64_t kSymbolBits = 32;
    static_assert((((kQuoteAccountLimit - 1) << kSymbolBits) | 0xFFFFFFFFULL) <=
                      (std::numeric_limits<uint64_t>::max() - kMarker) / kBlock,
                  "the widest pair in range must still fit above the marker without wrapping");""",
        new="""    constexpr uint64_t kMarker = 0x51'00000000ULL;  // 'Q'
    constexpr uint64_t kSymbolBits = 31;
    static_assert((((kQuoteAccountLimit - 1) << kSymbolBits) | 0x7FFFFFFFULL) <=
                      (std::numeric_limits<uint64_t>::max() - kMarker) / kBlock,
                  "the widest pair in range must still fit above the marker without wrapping");""",
    ),
    Mutation(
        name="adv-clordid-uint64-max-spuriously-refused",
        why="a fabricated post-parse rule refuses a ClOrdID of exactly UINT64_MAX -- a sanity "
            "check that the round-trip control (NumericValuesRoundTripExactly, which asserts "
            "18446744073709551615 decodes successfully) actually catches an over-eager "
            "refusal at the legal boundary rather than a real hole",
        file=CODEC,
        old="""      o.clientOrderId = o.id;""",
        new="""      if (o.id == std::numeric_limits<uint64_t>::max())
      {
        return refuse("ClOrdID", 11, "the maximum value is reserved");
      }
      o.clientOrderId = o.id;""",
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


def control(targets: list[str]) -> bool:
    ok = True
    for target in targets:
        for obj in object_files(target):
            obj.unlink()
        rebuild(target)
        code, output = run_test(target, None)
        state = "green" if code == 0 else "RED"
        summary = next((line for line in output.splitlines() if line.startswith("[==========] ")
                        and " ran." in line), "")
        print(f"  control {target:<34} {state}   {summary.strip()}")
        ok = ok and code == 0
    return ok


def run_mutation(m: Mutation) -> str:
    """'killed', 'alive' or 'no-compile'."""
    edits = m.editList()
    paths = {e.file: REPO / e.file for e in edits}
    originals = {f: p.read_text() for f, p in paths.items()}
    before = {f: sha256(p) for f, p in paths.items()}
    print(f"\n[{m.name}]")
    print(f"  {m.why}")
    for f in m.files():
        print(f"  file    {f}")
        print(f"  sha256  before  {before[f]}")

    texts = dict(originals)
    for e in edits:
        texts[e.file] = replace_occurrence(texts[e.file], e.old, e.new, e.occurrence,
                                           e.expected_occurrences)
    for f, p in paths.items():
        if texts[f] == originals[f]:
            raise SystemExit(f"mutation changed nothing in {f}")
        p.write_text(texts[f])
        print(f"  sha256  mutated {sha256(p)}  {f}")

    verdict = "alive"
    try:
        for target in m.targets:
            removed = object_files(target)
            for obj in removed:
                obj.unlink()
            print(f"  removed {len(removed)} object file(s) for {target}")

        try:
            for target in m.targets:
                output = rebuild(target)
                compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
                print(f"  rebuilt {target}: {compiled} 'Building CXX' line(s)")
        except BuildFailed as e:
            print(f"  {e.target} DID NOT COMPILE -- not a mutation")
            print("\n".join(e.output.splitlines()[-20:]))
            return "no-compile"

        for target in m.targets:
            code, test_output = run_test(target, m.gtest_filter)
            red = code != 0
            flt = f" --gtest_filter={m.gtest_filter}" if m.gtest_filter else ""
            print(f"  {target}{flt} -> exit {code} "
                  f"({'RED, mutation killed' if red else 'green'})")
            if red:
                verdict = "killed"
                failed = [line for line in test_output.splitlines()
                          if line.startswith("[  FAILED  ]")]
                for line in failed[:6]:
                    print(f"    {line}")
        if verdict == "alive":
            print("  GREEN, MUTATION SURVIVED every binary asked")
    finally:
        for f, p in paths.items():
            p.write_text(originals[f])
            after = sha256(p)
            print(f"  sha256  after   {after}  {f}")
            if after != before[f]:
                raise SystemExit(f"restore failed: {f} does not hash back to its original")
        for target in m.targets:
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
            print(f"{m.name:<50} {', '.join(m.files())}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(f"{BUILD} is not configured; run\n  cmake --preset venue-lite")

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")
    targets = sorted({t for m in selected for t in m.targets})

    print("control run before the mutations")
    if not control(targets):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, run_mutation(m)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(targets)

    print("\nsummary")
    label = {"killed": "RED  ", "alive": "ALIVE", "no-compile": "NOBLD"}
    for m, verdict in results:
        print(f"  {label[verdict]}  {m.name:<50} {', '.join(m.files())}")
    survived = [m.name for m, v in results if v == "alive"]
    nobuild = [m.name for m, v in results if v == "no-compile"]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived: {', '.join(survived)}")
    if nobuild:
        print(f"{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
