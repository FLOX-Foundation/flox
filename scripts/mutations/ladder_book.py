#!/usr/bin/env python3
"""Mutation harness for the LadderBook refusal contract and its id index.

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks the LadderBook fix one piece at
a time, in the source, and checks that the tests written for it go red -- and
that an unmutated tree goes green before and after.

Two test binaries answer for every mutation, and a mutation is killed if EITHER
of them goes red:

    test_venue_ladder_book_contract   the acceptance tests
    test_venue_ladder_book_index      the id-index tests that came with the fix

Every run is honest about the build: the mutated file's hash is printed before
and after, every target's object files are deleted so nothing can be served
from cache, the rebuild output has to contain "Building CXX" or the run is
refused, and each binary runs under a timeout. A mutation that does not compile
is not a mutation and is reported as such.

Usage:

    python3 scripts/mutations/ladder_book.py            # control, mutations, control
    python3 scripts/mutations/ladder_book.py --list
    python3 scripts/mutations/ladder_book.py --only erase-writes-a-hole-instead-of-shifting-back

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

BOOK = "include/flox/book/ladder_book.h"
VALIDATE = "venue/include/flox-venue/engine/validate.inl"
ORDERS = "venue/include/flox-venue/engine/orders.inl"
PEGS = "venue/include/flox-venue/engine/expiry_pegs.inl"
LASTLOOK = "venue/include/flox-venue/engine/last_look.h"
RESTORE = "venue/include/flox-venue/engine/checkpoint_restore.inl"
METRICS = "venue/include/flox-venue/metrics.h"

CONTRACT = "test_venue_ladder_book_contract"
INDEX = "test_venue_ladder_book_index"
REASONS = "test_venue_reject_reasons"  # pre-existing, not part of this task
BOTH = [CONTRACT, INDEX]


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
    # A mutation is usually one edit in one file (the four fields above). Some
    # holes only open when two gates go at once -- the engine asks the same
    # question twice, before and after it lifts an order off the book -- so a
    # mutation may carry a list of edits instead.
    edits: list[Edit] = field(default_factory=list)
    targets: list[str] = field(default_factory=lambda: list(BOTH))
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
    # ---- the id index -----------------------------------------------------
    Mutation(
        name="erase-writes-a-hole-instead-of-shifting-back",
        why="deletion stops closing its probe chain and just blanks the slot, the way the "
            "tombstone version did: every entry the hole cut off is unreachable",
        file=BOOK,
        old="""    size_t j = (hole + 1) & mask;
    for (size_t steps = 0; steps + 1 < idxCap_ && idx_[j].occupied != 0; ++steps, j = (j + 1) & mask)
    {
      const size_t home = hash(idx_[j].id);
      const bool reachablePastHole =
          (hole <= j) ? (hole < home && home <= j) : (hole < home || home <= j);
      if (!reachablePastHole)
      {
        idx_[hole] = idx_[j];
        hole = j;
      }
    }
    idx_[hole] = Slot{};""",
        new="    idx_[hole] = Slot{};",
    ),
    Mutation(
        name="erase-shift-scan-truncated",
        why="the backward-shift scan looks at one slot instead of walking to the end of the "
            "chain, so anything further down the chain is orphaned",
        file=BOOK,
        old="for (size_t steps = 0; steps + 1 < idxCap_ && idx_[j].occupied != 0; ++steps, j = (j + 1) & mask)",
        new="for (size_t steps = 0; steps < 1 && idx_[j].occupied != 0; ++steps, j = (j + 1) & mask)",
    ),
    Mutation(
        name="erase-shift-scan-unbounded",
        why="the bound that stops the shift scan from circling a saturated table forever is "
            "removed; the comment says it is there so a bad table answers wrong rather than hangs",
        file=BOOK,
        old="for (size_t steps = 0; steps + 1 < idxCap_ && idx_[j].occupied != 0; ++steps, j = (j + 1) & mask)",
        new="for (size_t steps = 0; idx_[j].occupied != 0; ++steps, j = (j + 1) & mask)",
    ),
    Mutation(
        name="erase-reachability-drops-the-wrap-case",
        why="the (hole > j) half of the reachability test -- the chain that wraps the end of "
            "the table -- is dropped, so a wrapping chain is cut on every deletion",
        file=BOOK,
        old="""      const bool reachablePastHole =
          (hole <= j) ? (hole < home && home <= j) : (hole < home || home <= j);""",
        new="""      const bool reachablePastHole = (hole < home && home <= j);""",
    ),
    Mutation(
        name="insert-slot-claims-success-without-writing",
        why="insertSlot answers true when its probe ran off the end of the table without "
            "writing: the order goes on a level with no index entry and can never be cancelled",
        file=BOOK,
        old="""      if (idx_[k].occupied == 0)
      {
        idx_[k] = Slot{id, node, 1};
        return true;
      }
    }
    return false;
  }""",
        new="""      if (idx_[k].occupied == 0)
      {
        idx_[k] = Slot{id, node, 1};
        return true;
      }
    }
    return true;
  }""",
    ),
    Mutation(
        name="index-table-sized-to-the-node-pool",
        why="drops the load bound: the table is sized to the pool instead of past twice it, so "
            "occupancy can reach 1 and a probe need never meet an empty slot",
        file=BOOK,
        old="while (idxCap_ < static_cast<size_t>(c.maxOrders) * 2 + 1)",
        new="while (idxCap_ < static_cast<size_t>(c.maxOrders))",
    ),
    # ---- addResting -------------------------------------------------------
    Mutation(
        name="add-resting-accepts-an-exhausted-pool",
        why="an exhausted node pool answers Accepted, which is the silent drop the fix exists "
            "to remove -- the order is on no book and its owner is told it is working",
        file=BOOK,
        old="""    const int32_t n = allocNode();
    if (n < 0)
    {
      return BookAddResult::PoolExhausted;
    }""",
        new="""    const int32_t n = allocNode();
    if (n < 0)
    {
      return BookAddResult::Accepted;
    }""",
    ),
    Mutation(
        name="add-resting-ignores-a-failed-index-insert",
        why="the answer insertSlot gives is thrown away, so an order the index would not take "
            "is still linked into its level: it trades and cannot be cancelled",
        file=BOOK,
        old="""    if (!insertSlot(o.id, n))
    {
      freeNode(n);
      return BookAddResult::PoolExhausted;
    }""",
        new="    (void)insertSlot(o.id, n);",
    ),
    Mutation(
        name="add-resting-indexes-after-the-level-link",
        why="restores the original ordering -- level first, index second -- so a failed index "
            "insert leaves a half-inserted order on the level and refuses it at the same time",
        file=BOOK,
        old="""    if (!insertSlot(o.id, n))
    {
      freeNode(n);
      return BookAddResult::PoolExhausted;
    }
    Node& node = nodes_[static_cast<size_t>(n)];
    node.order = o;
    node.level = lvl;
    node.side = side;
    node.prev = -1;
    node.next = -1;

    Level& level = levelRef(side, lvl);
    if (level.head < 0)
    {
      level.head = level.tail = n;
      setBit(bitsRef(side), lvl);
      onLevelOccupied(side, lvl);
    }
    else
    {
      nodes_[static_cast<size_t>(level.tail)].next = n;
      node.prev = level.tail;
      level.tail = n;
    }
    level.totalQty += o.leaves + o.hidden;  // hidden reserve is real liquidity
    return BookAddResult::Accepted;         // count_ is maintained by allocNode/freeNode""",
        new="""    Node& node = nodes_[static_cast<size_t>(n)];
    node.order = o;
    node.level = lvl;
    node.side = side;
    node.prev = -1;
    node.next = -1;

    Level& level = levelRef(side, lvl);
    if (level.head < 0)
    {
      level.head = level.tail = n;
      setBit(bitsRef(side), lvl);
      onLevelOccupied(side, lvl);
    }
    else
    {
      nodes_[static_cast<size_t>(level.tail)].next = n;
      node.prev = level.tail;
      level.tail = n;
    }
    level.totalQty += o.leaves + o.hidden;
    if (!insertSlot(o.id, n))
    {
      return BookAddResult::PoolExhausted;
    }
    return BookAddResult::Accepted;""",
    ),
    # ---- the price ladder -------------------------------------------------
    Mutation(
        name="can-rest-accepts-a-price-below-the-base",
        why="canRest stops checking the bottom of the ladder, so the gate the engine asks "
            "before it commits an order to matching passes a price with no level",
        file=BOOK,
        old="""    const int32_t lvl = levelOf(p);
    return lvl >= 0 && lvl < numLevels_;""",
        new="""    const int32_t lvl = levelOf(p);
    return lvl < numLevels_;""",
    ),
    Mutation(
        name="level-of-truncates-toward-zero",
        why="the original bug: the window (base - tick, base) divides out to 0, so a price "
            "under the ladder is filed on the base level and quoted at the level's price",
        file=BOOK,
        old="    const int64_t l = floorDiv(p.raw() - base_, tick_);",
        new="    const int64_t l = (p.raw() - base_) / tick_;",
        occurrence=1,
        expected_occurrences=2,
    ),
    Mutation(
        name="clamp-level-truncates-toward-zero",
        why="the same truncation on the taker's side: a limit under the base clamps to level 0 "
            "and sweeps the depth resting above it",
        file=BOOK,
        old="    const int64_t l = floorDiv(p.raw() - base_, tick_);",
        new="    const int64_t l = (p.raw() - base_) / tick_;",
        occurrence=2,
        expected_occurrences=2,
    ),
    # ---- the one door onto the book ---------------------------------------
    Mutation(
        name="rest-on-book-skips-full",
        why="the last-node gate the book's own comment always asked for is not consulted",
        file=VALIDATE,
        old="""  if (book_.full())
  {
    return RejectReason::BookCapacityExceeded;
  }
""",
        new="",
    ),
    Mutation(
        name="rest-on-book-reasons-swapped",
        why="an out-of-band price answers BookCapacityExceeded and an exhausted pool answers "
            "InvalidPrice: the client is told to change the one thing that is not wrong",
        file=VALIDATE,
        old="""  if (!book_.canRest(ro.price))
  {
    return RejectReason::InvalidPrice;
  }
  if (book_.full())
  {
    return RejectReason::BookCapacityExceeded;
  }""",
        new="""  if (!book_.canRest(ro.price))
  {
    return RejectReason::BookCapacityExceeded;
  }
  if (book_.full())
  {
    return RejectReason::InvalidPrice;
  }""",
    ),
    Mutation(
        name="rest-on-book-switch-reasons-swapped",
        why="the same swap one level down, on the answers addResting itself gives",
        file=VALIDATE,
        old="""    case BookAddResult::PriceOutOfBand:
      // The client's price, and nothing the venue can do about it -- the same
      // answer the collar gives for a price it will not take.
      return RejectReason::InvalidPrice;
    case BookAddResult::PoolExhausted:
      break;
  }
  return RejectReason::BookCapacityExceeded;""",
        new="""    case BookAddResult::PriceOutOfBand:
      return RejectReason::BookCapacityExceeded;
    case BookAddResult::PoolExhausted:
      break;
  }
  return RejectReason::InvalidPrice;""",
    ),
    Mutation(
        name="validate-does-not-ask-can-rest",
        why="the pre-commit price gate is gone, so an unrepresentable price is discovered only "
            "after the order has matched -- it prints, and then it is canceled, not rejected",
        file=VALIDATE,
        old="""    // The book's own band, which the collar above knows nothing about: the
    // collar is optional config, the ladder's geometry is not. Asked here,
    // before the order is committed to matching, so a price the book cannot
    // represent is refused while refusing still costs nothing -- by the time
    // addResting sees it the order may already have printed.
    if (!book_.canRest(o.price))
    {
      return RejectReason::InvalidPrice;
    }
""",
        new="",
    ),
    Mutation(
        name="modify-does-not-ask-can-rest",
        why="an amend to a price the book has no level for is no longer refused up front: it "
            "lifts the order off the book and then cannot put it back",
        file=ORDERS,
        old="""  // The book's own band, for the same reason submit asks: an amend to a price
  // the book has no level for would take the order off the book and then fail
  // to put it back. Refused here, the original order is still resting.
  if (!book_.canRest(newPrice))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidPrice, acct, true});
    return;
  }
""",
        new="",
    ),
    # ---- the engine's re-entry paths --------------------------------------
    Mutation(
        name="triggered-stop-ignores-the-refusal",
        why="a triggered stop's residual is acked as working whatever the book answered",
        file=ORDERS,
        old="""      if (const RejectReason why = restOnBook(agg->side, rro); why != RejectReason::None)
      {
        // Same rule as the submit path: a residual the book refused is a
        // reject when the triggered order printed nothing, and a cancel once
        // it has, because a reject cannot follow its own executions.
        if (out.filled.isZero())
        {
          releaseReservation(agg->id);
          sink_(OrderRejected{agg->id, cfg_.id, why, agg->accountId, agg->clientOrderId});
        }
        else
        {
          releaseReservationExceptHeld(agg->id);
          sink_(OrderCanceled{agg->id, cfg_.id, CancelReason::BookRefused, agg->accountId,
                              agg->clientOrderId, out.leaves, out.filled});
        }
      }
      else
      {
        trackResting(agg->id, agg->accountId, agg->stp);
        sink_(OrderAccepted{agg->id, cfg_.id, agg->side, agg->price, out.leaves, true, Quantity{},
                            agg->accountId, agg->clientOrderId, out.filled});
      }""",
        new="""      (void)restOnBook(agg->side, rro);
      trackResting(agg->id, agg->accountId, agg->stp);
      sink_(OrderAccepted{agg->id, cfg_.id, agg->side, agg->price, out.leaves, true, Quantity{},
                          agg->accountId, agg->clientOrderId, out.filled});""",
    ),
    Mutation(
        name="amend-reenter-ignores-the-refusal",
        why="an amend that the book will not take back sends OrderModified for an order that "
            "is on no book: the amend already lifted it off",
        file=ORDERS,
        old="""    if (restOnBook(side, mro) != RejectReason::None)
    {
      // The amend already lifted the order off the book, so there is no
      // original left to keep: the order is gone and its owner is told so,
      // rather than being sent an OrderModified about an order on no book.
      releaseReservationExceptHeld(m.id);
      forgetOrder(m.id);
      sink_(OrderCanceled{m.id, m.symbol, CancelReason::BookRefused, acct, curClientOrderId,
                          out.leaves, curCumQty + out.filled});
      processTriggers();
      return;
    }""",
        new="    (void)restOnBook(side, mro);",
    ),
    Mutation(
        name="peg-putback-ignores-the-refusal",
        why="a peg whose price did not move is put back without checking, and is silently lost "
            "if the book refuses",
        file=PEGS,
        old="""      if (restOnBook(ro->side, *ro) != RejectReason::None)
      {
        releaseReservation(id);
        forgetOrder(id);
        pegs_.erase(id);
        sink_(OrderCanceled{id, cfg_.id, CancelReason::BookRefused, ro->accountId,
                            ro->clientOrderId, ro->leaves + ro->hidden, ro->cumQty});
      }
      continue;""",
        new="""      (void)restOnBook(ro->side, *ro);
      continue;""",
    ),
    Mutation(
        name="peg-reprice-ignores-the-refusal",
        why="a peg target computed from a touch that ran past the band is a price with no "
            "level; the reprice reports OrderModified for an order the book dropped",
        file=PEGS,
        old="""    if (restOnBook(nr.side, nr) != RejectReason::None)
    {
      releaseReservation(id);
      forgetOrder(id);
      pegs_.erase(id);
      sink_(OrderCanceled{id, cfg_.id, CancelReason::BookRefused, nr.accountId, nr.clientOrderId,
                          nr.leaves + nr.hidden, nr.cumQty});
      continue;
    }""",
        new="    (void)restOnBook(nr.side, nr);",
    ),
    Mutation(
        name="last-look-maker-restore-ignores-the-refusal",
        why="the maker's held quantity is put back without checking, and OrderModified is sent "
            "for an order the book did not take",
        file=LASTLOOK,
        old="""      if (!host.reinsertTail(ro->side, *ro))
      {
        host.releaseHeldLeg(h.maker, h.qty);
        host.publish(OrderCanceled{h.maker, symbol, CancelReason::BookRefused, h.makerAccount,
                                   ro->clientOrderId, ro->leaves, ro->cumQty});
        return;
      }""",
        new="      (void)host.reinsertTail(ro->side, *ro);",
    ),
    Mutation(
        name="last-look-taker-restore-ignores-the-refusal",
        why="the rebuilt taker is adopted and acked as working whatever the book answered -- "
            "the pool can have filled while the hold was open",
        file=LASTLOOK,
        old="""        if (!host.reinsertTail(h.takerSide, rebuilt))
        {
          // Nothing of this taker rests, so there is no accept to send: it
          // ends the way its TIF would have ended it, with its held buying
          // power released.
          host.releaseHeldLeg(h.taker, h.qty);
          host.publish(OrderCanceled{h.taker, symbol, CancelReason::BookRefused, h.takerAccount,
                                     h.takerClientOrderId, h.qty, rebuilt.cumQty});
          return;
        }""",
        new="        (void)host.reinsertTail(h.takerSide, rebuilt);",
    ),
    Mutation(
        name="checkpoint-restore-accepts-a-refused-record",
        why="a snapshot record the book will not take is counted as restored, so the generation "
            "comes back one order short of the state whose hash it claims",
        file=RESTORE,
        old="""  if (book_.addResting(r.side, ro) != BookAddResult::Accepted)
  {
    return false;
  }""",
        new="  (void)book_.addResting(r.side, ro);",
    ),
    # ---- what the owner is told -------------------------------------------
    Mutation(
        name="refused-residual-cancel-reason-replaced",
        why="a residual the book refused after the order printed is reported as Expired, so the "
            "owner cannot tell a venue-side refusal from its own time in force running out",
        file=VALIDATE,
        old="""        sink_(OrderCanceled{o.id, o.symbol, CancelReason::BookRefused, o.accountId,
                            o.clientOrderId, out.leaves, out.filled});""",
        new="""        sink_(OrderCanceled{o.id, o.symbol, CancelReason::Expired, o.accountId,
                            o.clientOrderId, out.leaves, out.filled});""",
    ),
    Mutation(
        name="last-look-full-maker-restore-ignores-the-refusal",
        why="the maker whose whole displayed size was held is put back without checking: its "
            "node went to the free list when the hold took it and may be gone",
        file=LASTLOOK,
        old="""      if (!host.reinsertTail(makerSide, rebuilt))
      {
        // This hold took the maker's whole displayed size, so the order left
        // the book and its node with it: the pool can have filled up while the
        // hold was open. Nothing to modify, so the owner is told the order is
        // gone.
        host.releaseHeldLeg(h.maker, h.qty);
        host.publish(OrderCanceled{h.maker, symbol, CancelReason::BookRefused, h.makerAccount,
                                   h.makerClientOrderId, h.qty, rebuilt.cumQty});
        return;
      }""",
        new="      (void)host.reinsertTail(makerSide, rebuilt);",
    ),
    Mutation(
        name="last-look-taker-residual-restore-ignores-the-refusal",
        why="the taker's still-resting remainder takes its held quantity back without checking, "
            "and OrderModified goes out for an order the book did not take",
        file=LASTLOOK,
        old="""        if (!host.reinsertTail(ro->side, *ro))
        {
          host.releaseHeldLeg(h.taker, h.qty);
          host.publish(OrderCanceled{h.taker, symbol, CancelReason::BookRefused, h.takerAccount,
                                     ro->clientOrderId, ro->leaves, ro->cumQty});
          return;
        }""",
        new="        (void)host.reinsertTail(ro->side, *ro);",
    ),
    Mutation(
        name="amend-asks-neither-gate",
        why="both halves of the amend contract at once -- no pre-lift price gate and no check "
            "on the way back -- which is the state the order is actually lost in",
        edits=[
            Edit(
                file=ORDERS,
                old="""  // The book's own band, for the same reason submit asks: an amend to a price
  // the book has no level for would take the order off the book and then fail
  // to put it back. Refused here, the original order is still resting.
  if (!book_.canRest(newPrice))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidPrice, acct, true});
    return;
  }
""",
                new="",
            ),
            Edit(
                file=ORDERS,
                old="""    if (restOnBook(side, mro) != RejectReason::None)
    {
      // The amend already lifted the order off the book, so there is no
      // original left to keep: the order is gone and its owner is told so,
      // rather than being sent an OrderModified about an order on no book.
      releaseReservationExceptHeld(m.id);
      forgetOrder(m.id);
      sink_(OrderCanceled{m.id, m.symbol, CancelReason::BookRefused, acct, curClientOrderId,
                          out.leaves, curCumQty + out.filled});
      processTriggers();
      return;
    }""",
                new="    (void)restOnBook(side, mro);",
            ),
        ],
    ),
    Mutation(
        name="index-table-sized-to-the-node-pool-and-unbounded-shift",
        why="the two index mutations that survive alone, together: at an occupancy the load "
            "bound used to forbid, the deletion scan has no empty slot to stop on",
        edits=[
            Edit(
                file=BOOK,
                old="while (idxCap_ < static_cast<size_t>(c.maxOrders) * 2 + 1)",
                new="while (idxCap_ < static_cast<size_t>(c.maxOrders))",
            ),
            Edit(
                file=BOOK,
                old="for (size_t steps = 0; steps + 1 < idxCap_ && idx_[j].occupied != 0; ++steps, j = (j + 1) & mask)",
                new="for (size_t steps = 0; idx_[j].occupied != 0; ++steps, j = (j + 1) & mask)",
            ),
        ],
    ),
    Mutation(
        name="metrics-reject-array-one-short",
        why="the per-reason counter array is not grown for the new reason, so counting a "
            "capacity reject writes one past the end of the array",
        file=METRICS,
        old="static_cast<size_t>(RejectReason::PegRequiresTick) + 1;",
        new="static_cast<size_t>(RejectReason::NewOrderNotPermitted) + 1;",
        # The third binary is not part of this task's tests: it is the enum/counter
        # pairing test that already lived in the tree, asked here so the verdict
        # says who actually covers this.
        targets=[CONTRACT, INDEX, REASONS],
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
        # Deleted first so the control binary is compiled from the source as it
        # stands right now, not served from whatever the last run left behind.
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
            print(f"{m.name:<46} {', '.join(m.files())}")
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
        print(f"  {label[verdict]}  {m.name:<46} {', '.join(m.files())}")
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
