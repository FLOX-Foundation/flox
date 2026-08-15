#!/usr/bin/env python3
"""Exhaustively check the checkpoint/recovery protocol over a bounded model.

Recovery is where tests are weakest: the interesting states are crash points
between file operations, and a test can only visit the ones someone imagined.
This enumerates every reachable state of a bounded model instead -- every
interleaving of checkpoint, publish failure, corruption and pruning, up to a
small number of generations -- and checks one property on all of them.

THE PROPERTY. If recovery reports success, the state it rebuilt covers the
whole committed history. Silently rebuilding a truncated history is the worst
possible outcome: the venue starts, serves traffic, and is wrong.

THE MODEL mirrors sequenced_shard.h:

  Checkpoint at ts   rotate the journal onto segment(ts) FIRST, then publish
                     snapshot(ts) in the background; on success, prune.
  Publish            may fail (a failed rename is logged and tolerated), which
                     leaves a segment with no snapshot of its own.
  Recovery           newest snapshot that validates end-to-end, plus every
                     segment at or after it. With no valid snapshot: the
                     legacy pre-checkpoint file plus every segment.
  Pruning            keeps `retain` snapshot generations, deletes older
                     snapshots AND their segments, and deletes the legacy file
                     once `retain` generations exist.

Coverage is the whole point of the model: a snapshot at ts stands for the
history before ts, a segment at ts for the history from ts to the next
checkpoint. Recovery is complete when the pieces it picked tile the history
from 0 with no gap.

Usage:
    python3 scripts/check_recovery_model.py
    python3 scripts/check_recovery_model.py --generations 5 --retain 2
"""
from __future__ import annotations

import argparse
import itertools
import sys
from dataclasses import dataclass


@dataclass(frozen=True)
class State:
    # Checkpoint timestamps taken so far, ascending. Also the segment boundaries.
    taken: tuple[int, ...]
    # Snapshot files present on disk, by ts.
    snapshots: frozenset[int]
    # Of those, the ones that still validate end-to-end.
    valid: frozenset[int]
    # Segment files present on disk, by ts.
    segments: frozenset[int]
    # The pre-checkpoint single journal file, holding history before taken[0].
    legacy: bool


def prune(st: State, retain: int) -> State:
    """Delete generations beyond `retain`, exactly as pruneGenerations does."""
    snaps = sorted(st.snapshots)
    snapshots, segments, legacy = st.snapshots, st.segments, st.legacy
    if len(snaps) > retain:
        keep_from = snaps[len(snaps) - retain]
        snapshots = frozenset(t for t in snapshots if t >= keep_from)
        segments = frozenset(t for t in segments if t >= keep_from)
    if len(snaps) >= retain:
        legacy = False
    return State(st.taken, snapshots, st.valid & snapshots, segments, legacy)


def successors(st: State, retain: int, max_gen: int):
    """Every next state: a checkpoint (publish ok or failed), or a corruption."""
    if len(st.taken) < max_gen:
        ts = (st.taken[-1] + 1) if st.taken else 1
        taken = st.taken + (ts,)
        # The journal rotates onto the new segment before the snapshot is
        # published -- a crash in that window leaves the segment without one.
        rotated = State(taken, st.snapshots, st.valid, st.segments | {ts}, st.legacy)
        yield ("checkpoint-publish-failed", rotated)
        published = State(taken, rotated.snapshots | {ts}, rotated.valid | {ts},
                          rotated.segments, rotated.legacy)
        yield ("checkpoint-published", prune(published, retain))

    # A snapshot stops validating: a torn write, or a format/state-hash change
    # that invalidates what is on disk. Systematic causes hit every generation,
    # which is why more than one may go at once.
    for ts in sorted(st.valid):
        yield (f"snapshot-{ts}-no-longer-validates",
               State(st.taken, st.snapshots, st.valid - {ts}, st.segments, st.legacy))


def recover(st: State):
    """Model of recoverAll. Returns (started, covered_from_zero)."""
    chosen = max(st.valid) if st.valid else None

    if chosen is not None:
        # Snapshot covers [0, chosen); segments must tile [chosen, now).
        needed = [t for t in st.taken if t >= chosen]
        complete = all(t in st.segments for t in needed)
        return True, complete

    # No valid snapshot: the legacy file covers [0, taken[0]) and every segment
    # must be present to tile the rest.
    complete = st.legacy and all(t in st.segments for t in st.taken)
    return True, complete


def recover_guarded(st: State):
    """Recovery with the guard this check exists to require.

    Falling back a generation is bounded by what pruning kept. Once the
    retained snapshots are exhausted, the remaining segments do not reach back
    to the start of history, and replaying them yields a plausible-looking but
    truncated state. Refusing to start is the only safe answer.
    """
    chosen = max(st.valid) if st.valid else None
    if chosen is not None:
        needed = [t for t in st.taken if t >= chosen]
        if not all(t in st.segments for t in needed):
            return False, False
        return True, True

    if not st.legacy and st.taken:
        return False, False  # history was pruned; it cannot be replayed in full
    complete = st.legacy and all(t in st.segments for t in st.taken)
    return complete, complete


def explore(retain: int, max_gen: int, recovery):
    start = State((), frozenset(), frozenset(), frozenset(), True)
    seen = {start: None}
    queue = [start]
    while queue:
        st = queue.pop()
        for label, nxt in successors(st, retain, max_gen):
            if nxt not in seen:
                seen[nxt] = (st, label)
                queue.append(nxt)

    violations = []
    for st in seen:
        started, complete = recovery(st)
        if started and not complete:
            trace, cur = [], st
            while seen.get(cur):
                prev, label = seen[cur]
                trace.append(label)
                cur = prev
            violations.append((st, list(reversed(trace))))
    return len(seen), violations


def describe(st: State) -> str:
    return (f"checkpoints={list(st.taken)} snapshots={sorted(st.snapshots)} "
            f"valid={sorted(st.valid)} segments={sorted(st.segments)} legacy={st.legacy}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--generations", type=int, default=4)
    ap.add_argument("--retain", type=int, default=2)
    ap.add_argument("--unguarded", action="store_true",
                    help="model recovery WITHOUT the exhaustion guard (should fail)")
    args = ap.parse_args()

    recovery = recover if args.unguarded else recover_guarded
    states, violations = explore(args.retain, args.generations, recovery)

    if states < 10:
        print(f"model explored only {states} states -- the bound is wrong, this proves nothing",
              file=sys.stderr)
        return 2

    if violations:
        print(f"recovery model: {len(violations)} state(s) start on a truncated history")
        st, trace = violations[0]
        print(f"  shortest witness: {describe(st)}")
        for step in trace:
            print(f"    {step}")
        print("\nRecovery must refuse to start when the retained generations cannot")
        print("reconstruct history back to zero.")
        return 1

    print(f"OK: {states} reachable states, retain={args.retain}, "
          f"{args.generations} generations. Recovery never starts on a truncated history.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
