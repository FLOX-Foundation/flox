#!/usr/bin/env python3
"""Mutation harness for the venue reachability fix (allocation rule on the
config, the three account-scoped verbs, InstrumentRegistry::apply wiring, and
the zero-tick peg refusal).

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks the reachability fix one piece
at a time, in the source, and checks that the tests written for it -- plus a
curated set of pre-existing tests that touch the same code -- go red. An
unmutated tree has to go green before and after.

Four groups of target binaries answer for the mutations, chosen by what the
mutated line can actually be observed through; a mutation is killed if ANY
binary in its group goes red:

    MATCH_POLICY  test_venue_match_policy_reach, test_venue_journal_window,
                  test_venue_golden_replay
    CONTROL       test_venue_control_position_verbs, test_venue_control_methods,
                  test_venue_control_plane, test_venue_golden_replay
    REGISTRY      test_venue_registry_runtime_wiring, test_venue_golden_replay
    PEG           test_venue_peg_zero_tick, test_venue_reject_reasons,
                  test_venue_golden_replay

test_venue_golden_replay is in every group on request: a mutation that
survives its own group but breaks the golden hashes is still worth knowing
about, and one that leaves them untouched says the golden suite records
nothing about this fix either.

Every run is honest about the build: the mutated file's hash is printed before
and after, every target's object files are deleted so nothing can be served
from cache, the rebuild output has to contain "Building CXX" or the run is
refused, and each binary runs under a timeout. A mutation that does not
compile is not a mutation and is reported as such.

A mutation may carry `equivalent`, a short reason. That marks a mutation the
harness runs and reports honestly, but which is not evidence of a missing
assertion: it changes the source text without changing any observable output
of the binaries in its group, so nothing could kill it and nothing should be
asked to. The summary calls these EQUIV rather than ALIVE and they do not
turn the run's exit code non-zero.

Usage:

    python3 scripts/mutations/venue_reachability.py            # control, mutations, control
    python3 scripts/mutations/venue_reachability.py --list
    python3 scripts/mutations/venue_reachability.py --only peg-refusal-only-for-buys

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

CONTROL_API = "venue/include/flox-venue/control_api.h"
CONTROL_PLANE = "venue/include/flox-venue/control_plane.h"
SEQUENCED_SHARD = "venue/include/flox-venue/sequenced_shard.h"
SYMBOL_ROUTER = "venue/include/flox-venue/symbol_router.h"
JOURNAL_WINDOW = "venue/include/flox-venue/journal_window.h"
PEGS = "venue/include/flox-venue/engine/pegs.h"
VALIDATE = "venue/include/flox-venue/engine/validate.inl"

MATCH = "test_venue_match_policy_reach"
JWIN = "test_venue_journal_window"
GOLDEN = "test_venue_golden_replay"
VERBS = "test_venue_control_position_verbs"
METHODS = "test_venue_control_methods"
PLANE = "test_venue_control_plane"
REGWIRE = "test_venue_registry_runtime_wiring"
PEGZERO = "test_venue_peg_zero_tick"
REASONS = "test_venue_reject_reasons"

MATCH_POLICY = [MATCH, JWIN, GOLDEN]
CONTROL = [VERBS, METHODS, PLANE, GOLDEN]
REGISTRY = [REGWIRE, GOLDEN]
PEG = [PEGZERO, REASONS, GOLDEN]


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
    targets: list[str] = field(default_factory=list)
    gtest_filter: str | None = None
    occurrence: int = 1
    expected_occurrences: int = 1
    # A short reason, set only when the mutation is known to change no
    # observable output of the binaries in its group -- see the module
    # docstring. Left None for every mutation that is a real hole candidate.
    equivalent: str | None = None

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
    # ---- the allocation rule travels on the config (finding 13) -----------
    Mutation(
        name="shard-ignores-match-policy",
        why="the live engine SequencedShard builds drops cfg.matchPolicy, so the constructor "
            "default (price-time) is what a pro-rata instrument actually runs under",
        file=SEQUENCED_SHARD,
        old="out_.publish(std::move(msg)); }, std::move(book), cfg.matchPolicy)",
        new="out_.publish(std::move(msg)); }, std::move(book))",
        targets=MATCH_POLICY,
    ),
    Mutation(
        name="router-ignores-match-policy",
        why="SymbolRouter::addSymbol drops cfg.matchPolicy the same way -- the other deployment "
            "route into an engine, ignoring the field this fix put on the config for it",
        file=SYMBOL_ROUTER,
        old="auto eng = std::make_unique<MatchingEngine<Book>>(cfg, std::move(sink), std::move(book),\n"
            "                                                      cfg.matchPolicy);",
        new="auto eng = std::make_unique<MatchingEngine<Book>>(cfg, std::move(sink), std::move(book));",
        targets=MATCH_POLICY,
    ),
    Mutation(
        name="validate-snapshot-probe-ignores-match-policy",
        why="the scratch engine validateSnapshot builds to check a resumed generation matches "
            "under price-time regardless of the shard's own policy, so it would refuse every "
            "pro-rata snapshot this shard ever publishes",
        file=SEQUENCED_SHARD,
        old="MatchingEngine<Book> probe(cfg_, [](const OutboundEvent&) {}, Book{bookProto_}, cfg_.matchPolicy);",
        new="MatchingEngine<Book> probe(cfg_, [](const OutboundEvent&) {}, Book{bookProto_});",
        targets=MATCH_POLICY,
    ),
    Mutation(
        name="replay-window-ignores-match-policy",
        why="journal_window.h's replayWindow builds its scratch engine under the constructor "
            "default instead of cfg.matchPolicy, so a window replayed over a pro-rata segment "
            "resolves fills under the wrong rule -- and no existing test replays a pro-rata "
            "window, acceptance or otherwise",
        file=JOURNAL_WINDOW,
        old="out.events.push_back(WindowEvent{eventTs, e}); }, Book{}, cfg.matchPolicy);",
        new="out.events.push_back(WindowEvent{eventTs, e}); }, Book{});",
        targets=MATCH_POLICY,
    ),
    # ---- the three verbs (finding 23) --------------------------------------
    Mutation(
        name="kbuiltins-drops-set-account-risk-limits",
        why="setAccountRiskLimits answers in handle() but is missing from kBuiltinMethods, so a "
            "deployment can registerMethod over it and shadow the built-in for some requests",
        file=CONTROL_API,
        old='"list", "setAccountRiskLimits", "adjustPosition", "forceClosePosition"};',
        new='"list", "adjustPosition", "forceClosePosition"};',
        targets=CONTROL,
    ),
    Mutation(
        name="kbuiltins-drops-adjust-position",
        why="the same hole for adjustPosition",
        file=CONTROL_API,
        old='"list", "setAccountRiskLimits", "adjustPosition", "forceClosePosition"};',
        new='"list", "setAccountRiskLimits", "forceClosePosition"};',
        targets=CONTROL,
    ),
    Mutation(
        name="kbuiltins-drops-force-close-position",
        why="the same hole for forceClosePosition",
        file=CONTROL_API,
        old='"list", "setAccountRiskLimits", "adjustPosition", "forceClosePosition"};',
        new='"list", "setAccountRiskLimits", "adjustPosition"};',
        targets=CONTROL,
    ),
    Mutation(
        name="adjust-reason-unknown-text-falls-back-to-manual",
        why="an unrecognized reason string maps to Manual instead of being refused -- an "
            "operator typo silently becomes 'operator judgement' in a record nothing can "
            "distinguish from a real manual correction",
        file=CONTROL_API,
        old='    if (text == "manual")\n'
            "    {\n"
            "      out = AdjustReason::Manual;\n"
            "      return true;\n"
            "    }\n"
            "    return false;\n"
            "  }\n"
            "\n"
            "  void forward(const InboundCommand& cmd)",
        new='    if (text == "manual")\n'
            "    {\n"
            "      out = AdjustReason::Manual;\n"
            "      return true;\n"
            "    }\n"
            "    out = AdjustReason::Manual;\n"
            "    return true;\n"
            "  }\n"
            "\n"
            "  void forward(const InboundCommand& cmd)",
        targets=CONTROL,
    ),
    Mutation(
        name="adjust-position-absent-entry-explicit-zero",
        why="an absent entry is parsed (and defaulted to raw 0) instead of being left untouched "
            "-- the finding's own wording. The record has no separate unset bit, so this is "
            "unkillable by design: entryRaw==0 already IS the sentinel for both cases",
        file=CONTROL_API,
        old='      if (req.present("entry"))\n'
            "      {\n"
            "        Price entry{};\n"
            '        if (!req.decimalField("entry", entry) || entry.raw() < 0)\n'
            "        {\n"
            '          return err("bad_field");\n'
            "        }\n"
            "        a.entryRaw = entry.raw();\n"
            "      }",
        new="      {\n"
            "        Price entry{};\n"
            '        (void)req.decimalField("entry", entry);\n'
            "        if (entry.raw() < 0)\n"
            "        {\n"
            '          return err("bad_field");\n'
            "        }\n"
            "        a.entryRaw = entry.raw();\n"
            "      }",
        targets=CONTROL,
        equivalent="AdjustPosition::entryRaw has no separate 'present' bit on the wire -- 0 IS "
                   "the unset sentinel (see engine/clearing.h:225-234). Whether the guard "
                   "leaves a.entryRaw untouched (default 0) or an unconditional parse assigns "
                   "it explicitly, every request produces the identical record. No assertion "
                   "could kill this without the wire format growing a presence bit.",
    ),
    Mutation(
        name="force-close-qty-accepts-a-sign",
        why="the qty field is documented as a SIZE ('a signed request here would let an operator "
            "name one that contradicts [the position]'), but the negative-value guard is the "
            "only thing enforcing that -- and no acceptance test ever sends a negative qty",
        file=CONTROL_API,
        old='        if (!req.decimalField("qty", q) || q.raw() < 0)\n'
            "        {\n"
            '          return err("bad_field");\n'
            "        }",
        new='        if (!req.decimalField("qty", q))\n'
            "        {\n"
            '          return err("bad_field");\n'
            "        }",
        targets=CONTROL,
    ),
    Mutation(
        name="set-account-risk-limits-empty-mask-accepted",
        why="a request naming none of the three limit groups is forwarded and journaled as a "
            "no-op SetAccountRiskLimits record instead of being refused the way its symbol-wide "
            "sibling refuses the same shape of request",
        file=CONTROL_API,
        old="        r.fields |= AccountRiskLimitField::AccountRiskMaxPosition;\n"
            "      }\n"
            "      if (r.fields == 0)\n"
            "      {\n"
            '        return err("no_limits_named");\n'
            "      }\n"
            "      forward(InboundCommand{r});",
        new="        r.fields |= AccountRiskLimitField::AccountRiskMaxPosition;\n"
            "      }\n"
            "      forward(InboundCommand{r});",
        targets=CONTROL,
    ),
    # ---- InstrumentRegistry wiring (finding 30) ----------------------------
    Mutation(
        name="apply-drops-set-account-risk-limits-branch",
        why="the configuration OR-chain in apply() loses the SetAccountRiskLimits arm, so the "
            "one record type this fix exists for goes back to answering 'not configuration'",
        file=CONTROL_PLANE,
        old="    if (std::get_if<SetStpGroup>(&cmd) != nullptr ||\n"
            "        std::get_if<SetFundingSchedule>(&cmd) != nullptr ||\n"
            "        std::get_if<SetAdmissionProfile>(&cmd) != nullptr ||\n"
            "        std::get_if<SetRiskLimits>(&cmd) != nullptr ||\n"
            "        // Routed by symbol and journaled exactly like the three above. It was\n"
            '        // answered "not a configuration command" only because it was added\n'
            "        // last, which made a replay of the WAL stop being a faithful replay\n"
            "        // at the first per-account limit.\n"
            "        std::get_if<SetAccountRiskLimits>(&cmd) != nullptr)",
        new="    if (std::get_if<SetStpGroup>(&cmd) != nullptr ||\n"
            "        std::get_if<SetFundingSchedule>(&cmd) != nullptr ||\n"
            "        std::get_if<SetAdmissionProfile>(&cmd) != nullptr ||\n"
            "        std::get_if<SetRiskLimits>(&cmd) != nullptr)",
        targets=REGISTRY,
    ),
    Mutation(
        name="apply-snapshot-does-not-offer-the-registry",
        why="the snapshot recovery loop stops offering its records to the registry -- an "
            "instrument that only lives in the snapshot's config section (older than the "
            "surviving segments) comes back unknown to the registry after a checkpoint restart. "
            "No acceptance test ever calls checkpointNow() before restarting, so this path is "
            "unreachable by the suite this fix shipped",
        file=SEQUENCED_SHARD,
        old="        engine_.applySnapshotRecord(cmd, ts);\n"
            "        // The snapshot's config section is where an instrument listed before\n"
            "        // the oldest surviving segment still lives, so recovery through a\n"
            "        // checkpoint rebuilds the registry the same way a full replay does.\n"
            "        offerToRegistry(cmd);\n"
            "        if (ts > lastTs_)",
        new="        engine_.applySnapshotRecord(cmd, ts);\n"
            "        if (ts > lastTs_)",
        targets=REGISTRY,
    ),
    Mutation(
        name="live-command-path-does-not-offer-the-registry",
        why="the per-command live path stops offering to the registry -- a registry wired to a "
            "RUNNING shard only ever sees the replay at start() and goes stale the moment a new "
            "command is sequenced. No acceptance test submits a live command to a shard with a "
            "registry attached and checks the registry afterwards",
        file=SEQUENCED_SHARD,
        old="        journal_.append(ev.cmd, ts);  // write-ahead, before applying\n"
            "        engine_.submit(ev.cmd, ts);   // the SAME timestamp the journal holds\n"
            "        offerToRegistry(ev.cmd);      // the same record a restart would replay",
        new="        journal_.append(ev.cmd, ts);  // write-ahead, before applying\n"
            "        engine_.submit(ev.cmd, ts);   // the SAME timestamp the journal holds",
        targets=REGISTRY,
    ),
    # ---- pegs at tick 0 (finding 25) ---------------------------------------
    Mutation(
        name="peg-requires-tick-replaced-by-tick-size-violation",
        why="the finding's own honest-vs-prototype note: TickSizeViolation says 'your price is "
            "off the grid' and sends the client back with a rounded price, which cannot help "
            "here -- the instrument, not the order, is what is missing something. The zero-tick "
            "test only asks for reason != None, not for the specific reason",
        file=VALIDATE,
        old="    return RejectReason::PegRequiresTick;\n"
            "  }\n"
            "  if (!cfg_.minQty.isZero()",
        new="    return RejectReason::TickSizeViolation;\n"
            "  }\n"
            "  if (!cfg_.minQty.isZero()",
        targets=PEG,
    ),
    Mutation(
        name="peg-admission-refusal-removed",
        why="the whole admission-time refusal is gone, leaving only the clamp inside "
            "PegBook::targetRaw as the sole defense against a locked book",
        file=VALIDATE,
        old="  // A peg needs a tick for the same kind of reason. The never-cross clamp\n"
            "  // keeps a tracking order strictly inside the price it tracks, and the tick\n"
            '  // is the only distance it has to step back by; tickSize 0 means "unchecked"\n'
            "  // everywhere else in the config, but here it makes the clamp land on the\n"
            "  // opposite touch itself -- and repeg() re-rests the order through\n"
            "  // addResting, which runs no matching pass, so the instrument quotes\n"
            "  // bid == ask and nobody can trade out of it. Refused at admission, where a\n"
            "  // refusal still costs nothing and the owner gets a reason it can act on.\n"
            "  if (o.peg != PegRef::None && cfg_.tickSize.isZero())\n"
            "  {\n"
            "    return RejectReason::PegRequiresTick;\n"
            "  }\n",
        new="",
        targets=PEG,
    ),
    Mutation(
        name="peg-clamp-step-back-reverts-to-tick-raw",
        why="the fallback that steps the clamp back by one raw unit when there is no tick is "
            "gone, restoring the original bug at the second line of defense -- unreachable by "
            "this suite because the admission refusal above already turns every zero-tick peg "
            "away before it can rest and be repegged",
        file=PEGS,
        old="    const int64_t stepRaw = m.tickRaw > 0 ? m.tickRaw : 1;",
        new="    const int64_t stepRaw = m.tickRaw;",
        targets=PEG,
    ),
    Mutation(
        name="peg-alignment-truncates-toward-zero-again",
        why="the floor-division fix for a negative target is reverted to plain truncating "
            "division -- the original defect the finding's second line closes. No peg test, new "
            "or old, prices a target below zero, so this is unreachable by the curated group",
        file=PEGS,
        old="      int64_t levels = target / m.tickRaw;\n"
            "      if (target % m.tickRaw != 0 && target < 0)\n"
            "      {\n"
            "        --levels;\n"
            "      }\n"
            "      target = levels * m.tickRaw;",
        new="      target = target / m.tickRaw * m.tickRaw;  // align down to tick",
        targets=PEG,
    ),
    # ---- adversarial, not on the required list -----------------------------
    Mutation(
        name="live-apply-before-journal",
        why="the live command path applies to the engine and offers the registry BEFORE the "
            "journal write instead of after: 'write-ahead, before applying' stops being true. "
            "No test crashes the process between the two statements, so nothing in the suite "
            "can tell the order changed -- a real crash between them now applies a command "
            "(and can publish its events) that the journal never durably recorded",
        file=SEQUENCED_SHARD,
        old="        journal_.append(ev.cmd, ts);  // write-ahead, before applying\n"
            "        engine_.submit(ev.cmd, ts);   // the SAME timestamp the journal holds\n"
            "        offerToRegistry(ev.cmd);      // the same record a restart would replay",
        new="        engine_.submit(ev.cmd, ts);   // the SAME timestamp the journal holds\n"
            "        journal_.append(ev.cmd, ts);  // write-ahead, before applying\n"
            "        offerToRegistry(ev.cmd);      // the same record a restart would replay",
        targets=sorted(set(CONTROL + REGISTRY)),
    ),
    Mutation(
        name="registry-offered-in-reverse-stream-order",
        why="recover() still applies every record to the engine in stream order, but offers "
            "them to the registry in REVERSE order. Each of the four control-plane records the "
            "acceptance test journals is an absolute set, not an increment, so replaying them "
            "backward into the registry produces a different final configuration than the one "
            "the WAL actually describes",
        file=SEQUENCED_SHARD,
        old="      const auto records = Journal::loadTimed(path);\n"
            "      replaying_ = true;\n"
            "      for (const auto& [ts, cmd] : records)\n"
            "      {\n"
            "        engine_.submit(cmd, ts);\n"
            "        offerToRegistry(cmd);\n"
            "        if (ts > lastTs_)\n"
            "        {\n"
            "          lastTs_ = ts;\n"
            "        }\n"
            "      }\n"
            "      replaying_ = false;\n"
            "      return records.size();\n"
            "    }\n"
            "\n"
            "    // Apply a snapshot file",
        new="      const auto records = Journal::loadTimed(path);\n"
            "      replaying_ = true;\n"
            "      for (const auto& [ts, cmd] : records)\n"
            "      {\n"
            "        engine_.submit(cmd, ts);\n"
            "        if (ts > lastTs_)\n"
            "        {\n"
            "          lastTs_ = ts;\n"
            "        }\n"
            "      }\n"
            "      for (auto it = records.rbegin(); it != records.rend(); ++it)\n"
            "      {\n"
            "        offerToRegistry(it->second);\n"
            "      }\n"
            "      replaying_ = false;\n"
            "      return records.size();\n"
            "    }\n"
            "\n"
            "    // Apply a snapshot file",
        targets=REGISTRY,
    ),
    Mutation(
        name="peg-refusal-only-for-buys",
        why="the admission refusal gains '&& o.side == Side::BUY'. The zero-tick suite only ever "
            "arranges a BUY-side peg (arrangeAskThenRepeg), so a SELL peg on a tick-less "
            "instrument sails through admission and reaches the same locked-book path the fix "
            "closes -- with nothing in the suite ever pricing that side",
        file=VALIDATE,
        old="  if (o.peg != PegRef::None && cfg_.tickSize.isZero())",
        new="  if (o.peg != PegRef::None && cfg_.tickSize.isZero() && o.side == Side::BUY)",
        targets=PEG,
    ),
    Mutation(
        name="set-account-risk-limits-max-open-orders-off-by-one",
        why="the upper bound on maxOpenOrders is tightened from > to >=, so the one value at the "
            "edge of the field's own range (UINT32_MAX) is refused as bad_field instead of "
            "accepted -- no test in the suite sends that boundary value",
        file=CONTROL_API,
        old="            maxOpenOrders > (std::numeric_limits<uint32_t>::max)())\n"
            "        {\n"
            '          return err("bad_field");\n'
            "        }\n"
            "        r.fields |= AccountRiskLimitField::AccountRiskMaxOpenOrders;",
        new="            maxOpenOrders >= (std::numeric_limits<uint32_t>::max)())\n"
            "        {\n"
            '          return err("bad_field");\n'
            "        }\n"
            "        r.fields |= AccountRiskLimitField::AccountRiskMaxOpenOrders;",
        targets=CONTROL,
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
    """'killed', 'alive', 'equivalent' or 'no-compile'."""
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
        if verdict == "alive" and m.equivalent:
            verdict = "equivalent"
            print(f"  GREEN, but marked equivalent: {m.equivalent}")
        elif verdict == "alive":
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
    label = {"killed": "RED  ", "alive": "ALIVE", "equivalent": "EQUIV", "no-compile": "NOBLD"}
    for m, verdict in results:
        print(f"  {label[verdict]}  {m.name:<50} {', '.join(m.files())}")
    survived = [m.name for m, v in results if v == "alive"]
    equiv = [m.name for m, v in results if v == "equivalent"]
    nobuild = [m.name for m, v in results if v == "no-compile"]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived: {', '.join(survived)}")
    if equiv:
        print(f"{len(equiv)} mutation(s) equivalent (see --list for reasons): {', '.join(equiv)}")
    if nobuild:
        print(f"{len(nobuild)} mutation(s) did not compile: {', '.join(nobuild)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
