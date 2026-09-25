#!/usr/bin/env python3
"""Mutation run for the venue snapshot round-trip fixes.

Every fix in this task is worth exactly what the test that pins it is worth,
and a test is only worth something if it fails when the fix is taken away.
This script takes each fix away, one at a time, and insists the test goes red.

Per mutation: hash the file, apply an exact single-occurrence replacement,
delete the target test's object files (found on disk, not guessed), rebuild
the target and prove a compile actually happened ("Building CXX" in ninja's
output -- a mutation the build skipped is not a mutation), run the test under
a timeout, restore the file and prove the hash came back.

A mutation that does not compile is not a mutation either: it is reported as
NO-COMPILE and counts as a failure of the run.

Run from the repository root with a configured build-venue-lite:
    cmake --preset venue-lite && python3 scripts/mutations/venue_snapshot_round_trip.py
"""

import hashlib
import os
import subprocess
import sys

BUILD = "build-venue-lite"
JOBS = "4"
TEST_TIMEOUT = 300


class Mutation:
    def __init__(self, name, path, old, new, target, gtest_filter):
        self.name = name
        self.path = path
        self.old = old
        self.new = new
        self.target = target
        self.filter = gtest_filter


MUTATIONS = [
    Mutation(
        "clordid: the rotation moment is not restored",
        "venue/include/flox-venue/engine/checkpoint_restore.inl",
        "clOrdIds_.restore(r->account, r->generation, r->ids, r->count, r->rotatedAtNs);",
        "clOrdIds_.restore(r->account, r->generation, r->ids, r->count, 0);",
        "test_venue_clordid_window",
        "ClOrdIdWindow.*Snapshot*",
    ),
    Mutation(
        "clordid: the rotation moment is not written",
        "venue/include/flox-venue/engine/clordid_window.h",
        "        batch.rotatedAtNs = seen.rotatedAtNs;\n        for (uint64_t id : sortedIds(seen, g))",
        "        batch.rotatedAtNs = 0;\n        for (uint64_t id : sortedIds(seen, g))",
        "test_venue_clordid_window",
        "ClOrdIdWindow.*Snapshot*",
    ),
    Mutation(
        "clearing: a flat position record is corruption again",
        "venue/include/flox-venue/engine/clearing.h",
        "    if (positions_.count(r.account) != 0)",
        "    if (r.qtyRaw == 0 || positions_.count(r.account) != 0)",
        "test_venue_position_correction",
        "PositionCorrection.*",
    ),
    Mutation(
        "restore: a GTD conditional never reaches the expiry book",
        "venue/include/flox-venue/engine/checkpoint_restore.inl",
        "    expiry_.set(r.order.id, r.order.expiryNs);",
        "    (void)r.order.expiryNs;",
        "test_venue_checkpoint",
        "VenueCheckpoint.ARestoredGtdStopStillExpires",
    ),
    Mutation(
        "last look: the refused taker loses its reduce-only flag",
        "venue/include/flox-venue/engine/last_look.h",
        "        rebuilt.reduceOnly = h.takerReduceOnly;",
        "        rebuilt.reduceOnly = false;",
        "test_venue_engine_last_look",
        "VenueEngineLastLook.RejectRebuildsATakerHeldWhollyOutOfTheBookWithItsFlags",
    ),
    Mutation(
        "last look: the hold's reference price leaves the digest",
        "venue/include/flox-venue/engine/last_look.h",
        "        h = mix(h, static_cast<uint64_t>(x.refAtHoldRaw));",
        "        (void)x.refAtHoldRaw;",
        "test_venue_checkpoint",
        "VenueCheckpoint.AHoldsReferencePriceIsPartOfTheState",
    ),
    Mutation(
        "tags: the MMP fill windows collide with the closed marker again",
        "venue/include/flox-venue/engine/state_hash_tags.h",
        "inline constexpr uint64_t kMmpFills = 0xB012;",
        "inline constexpr uint64_t kMmpFills = 0xB00A;",
        "test_venue_state_hash_tags",
        "StateHashTags.*",
    ),
    Mutation(
        "tags: the firm STP groups collide with funding again",
        "venue/include/flox-venue/engine/state_hash_tags.h",
        "inline constexpr uint64_t kStpGroup = 0xB011;",
        "inline constexpr uint64_t kStpGroup = 0xB00B;",
        "test_venue_state_hash_tags",
        "StateHashTags.*",
    ),
    Mutation(
        "tags: the account caps collide with the delisted marker again",
        "venue/include/flox-venue/engine/state_hash_tags.h",
        "inline constexpr uint64_t kAccountRiskLimits = 0xB010;",
        "inline constexpr uint64_t kAccountRiskLimits = 0xB00E;",
        "test_venue_state_hash_tags",
        "StateHashTags.*",
    ),
]


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def objects_of(target):
    """The target's object files, found on disk rather than guessed."""
    out = subprocess.run(
        ["find", BUILD, "-name", "*.o", "-path", "*%s.dir*" % target],
        capture_output=True, text=True, check=True).stdout
    return [line for line in out.splitlines() if line]


def build(target):
    r = subprocess.run(
        ["cmake", "--build", BUILD, "--target", target, "-j", JOBS],
        capture_output=True, text=True)
    return r.returncode == 0, r.stdout + r.stderr


def run_test(target, gtest_filter):
    binary = os.path.join(BUILD, "venue", target)
    try:
        r = subprocess.run([binary, "--gtest_filter=" + gtest_filter],
                           capture_output=True, text=True, timeout=TEST_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None
    return r.returncode


def control():
    print("control: building and running every target unmutated")
    rows = []
    ok = True
    for m in MUTATIONS:
        built, _ = build(m.target)
        if not built:
            rows.append((m.target, m.filter, "BUILD FAILED"))
            ok = False
            continue
        rc = run_test(m.target, m.filter)
        state = "green" if rc == 0 else ("TIMEOUT" if rc is None else "RED (rc=%s)" % rc)
        rows.append((m.target, m.filter, state))
        ok = ok and rc == 0
    for target, flt, state in rows:
        print("  %-34s %-62s %s" % (target, flt, state))
    return ok


def apply(m):
    original = read(m.path)
    before = sha256(m.path)
    count = original.count(m.old)
    if count != 1:
        return ("NOT APPLIED (%d matches)" % count), before, None

    write(m.path, original.replace(m.old, m.new))
    try:
        removed = objects_of(m.target)
        for obj in removed:
            os.remove(obj)
        built, log = build(m.target)
        if not built:
            return "NO-COMPILE", before, sha256(m.path)
        if "Building CXX" not in log:
            return "NOT REBUILT", before, sha256(m.path)
        rc = run_test(m.target, m.filter)
        mutated_hash = sha256(m.path)
        if rc is None:
            return "TIMEOUT", before, mutated_hash
        return ("RED" if rc != 0 else "SURVIVED"), before, mutated_hash
    finally:
        write(m.path, original)


def main():
    if not os.path.isdir(BUILD):
        print("no %s -- run: cmake --preset venue-lite" % BUILD)
        return 2

    if not control():
        print("\ncontrol is not green; the mutations below would mean nothing")
        return 1

    print()
    results = []
    for m in MUTATIONS:
        verdict, before, mutated = apply(m)
        after = sha256(m.path)
        restored = after == before
        # The file is back, so the next mutation starts from the shipped tree.
        build(m.target)
        results.append((m, verdict, mutated is not None and mutated != before, restored))

    print("%-58s %-34s %-9s %-8s %s" % ("mutation", "target", "verdict", "changed", "restored"))
    print("-" * 122)
    for m, verdict, changed, restored in results:
        print("%-58s %-34s %-9s %-8s %s"
              % (m.name[:58], m.target, verdict, "yes" if changed else "NO",
                 "yes" if restored else "NO"))

    bad = [r for r in results if r[1] != "RED" or not r[2] or not r[3]]
    print()
    if bad:
        print("%d of %d mutations did not go red on a file that was really changed and "
              "really restored" % (len(bad), len(results)))
        return 1
    print("all %d mutations red, control green" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
