#!/usr/bin/env python3
"""Mutation harness for the matcher/venue atomicity fix (OCO commit boundary,
clientOrderId on matcher-initiated cancels, onModify/applyQuote ordering,
dispatch symbol guards).

A test that passes proves nothing on its own; what it has to do is fail when
the code it covers is wrong. This script breaks each piece of the fix one at
a time, in the source, and checks that the relevant test goes red -- and that
an unmutated tree goes green before and after.

Every run is honest about the build: the mutated file's hash is printed before
and after, the target's object files are deleted so nothing can be served from
cache, the rebuild output has to contain "Building CXX" or the run is refused,
and the test binary runs under a timeout.

Usage:

    python3 scripts/mutations/matcher_atomicity.py            # control, all mutations, control
    python3 scripts/mutations/matcher_atomicity.py --list
    python3 scripts/mutations/matcher_atomicity.py --only oco-unlink-luld-removed

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
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build-venue-lite"
CMAKE = os.environ.get("CMAKE", shutil.which("cmake") or "/opt/homebrew/bin/cmake")
BUILD_JOBS = os.environ.get("MUTATION_JOBS", "4")
BUILD_TIMEOUT = 900
TEST_TIMEOUT = 120

VALIDATE = "venue/include/flox-venue/engine/validate.inl"
ORDERS = "venue/include/flox-venue/engine/orders.inl"
QUOTE = "venue/include/flox-venue/engine/quote_mmp.inl"
DISPATCH = "venue/include/flox-venue/engine/dispatch.inl"
MATCHER = "venue/include/flox-venue/matcher.h"
OCOBOOK = "venue/include/flox-venue/engine/oco.h"

OCO_TEST = "test_venue_oco_commit_boundary"
ATOM_TEST = "test_venue_reject_atomicity"


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


MUTATIONS: list[Mutation] = [
    # ---- three oco_.unlink() calls added past the commit point, removed one
    # at a time -------------------------------------------------------------
    Mutation(
        name="oco-unlink-luld-removed",
        why="drops the unlink on the LULD-breach refusal; the leg stays in its group",
        file=VALIDATE,
        old="""      releaseReservation(o.id);
      oco_.unlink(o.id);  // refused, so it never joined the group it was linked into
      sink_(OrderRejected{o.id, o.symbol, RejectReason::LuldBreach, o.accountId, o.clientOrderId});""",
        new="""      releaseReservation(o.id);
      sink_(OrderRejected{o.id, o.symbol, RejectReason::LuldBreach, o.accountId, o.clientOrderId});""",
        target=OCO_TEST,
        test="OcoCommitBoundary.ALuldRefusalLeavesNoDeadIdInTheGroup",
    ),
    Mutation(
        name="oco-unlink-post-only-removed",
        why="drops the unlink on out.reject (post-only-would-cross / FOK-unfulfillable)",
        file=VALIDATE,
        old="""    releaseReservation(o.id);  // post-only-would-cross / FOK-unfulfillable: free the reserve
    oco_.unlink(o.id);         // and it never joined the group it was linked into
    sink_(OrderRejected{o.id, o.symbol, out.reject, o.accountId, o.clientOrderId});""",
        new="""    releaseReservation(o.id);  // post-only-would-cross / FOK-unfulfillable: free the reserve
    sink_(OrderRejected{o.id, o.symbol, out.reject, o.accountId, o.clientOrderId});""",
        target=OCO_TEST,
        test="OcoCommitBoundary.APostOnlyRefusalLeavesNoDeadIdInTheGroup",
    ),
    Mutation(
        name="oco-unlink-residual-canceled-removed",
        why="drops the unlink on the zero-fill residual cancel (raw-sink IOC/FOK path)",
        file=VALIDATE,
        old="""    releaseReservationExceptHeld(o.id);
    // The order is gone and was never tracked, so no forgetOrder will ever run
    // for it: this is its only chance to leave the group. Whether it filled
    // first is decided already -- the print is in oco_'s pending list and
    // processOco still cancels the siblings, this id simply is not one of them.
    oco_.unlink(o.id);
    sink_(OrderCanceled{o.id, o.symbol, out.residualCancelReason, o.accountId, o.clientOrderId,""",
        new="""    releaseReservationExceptHeld(o.id);
    sink_(OrderCanceled{o.id, o.symbol, out.residualCancelReason, o.accountId, o.clientOrderId,""",
        target=OCO_TEST,
        test="OcoCommitBoundary.AResidualCancelLeavesNoDeadIdInTheGroup",
    ),
    Mutation(
        name="oco-unlink-wrong-id",
        why="unlinks order 0 (never a real id in these tests) instead of the refused order -- same as not unlinking at all",
        file=VALIDATE,
        old="""      releaseReservation(o.id);
      oco_.unlink(o.id);  // refused, so it never joined the group it was linked into
      sink_(OrderRejected{o.id, o.symbol, RejectReason::LuldBreach, o.accountId, o.clientOrderId});""",
        new="""      releaseReservation(o.id);
      oco_.unlink(0);  // refused, so it never joined the group it was linked into
      sink_(OrderRejected{o.id, o.symbol, RejectReason::LuldBreach, o.accountId, o.clientOrderId});""",
        target=OCO_TEST,
        test="OcoCommitBoundary.ALuldRefusalLeavesNoDeadIdInTheGroup",
    ),

    # ---- four matcher-initiated OrderCanceled reports, clientOrderId back to
    # hardcoded 0 -------------------------------------------------------------
    Mutation(
        name="matcher-cancel-oldest-clord-zero",
        why="STP CancelOldest cancel report drops the resting order's clientOrderId",
        file=MATCHER,
        old="""      case STPMode::CancelOldest:
        sink(OrderCanceled{m.id, order.symbol, CancelReason::SelfTradePrevention, m.accountId,
                           m.clientOrderId, m.leaves + m.hidden, m.cumQty});
        book.cancel(m.id);
        return StpOutcome::RePeek;""",
        new="""      case STPMode::CancelOldest:
        sink(OrderCanceled{m.id, order.symbol, CancelReason::SelfTradePrevention, m.accountId,
                           0, m.leaves + m.hidden, m.cumQty});
        book.cancel(m.id);
        return StpOutcome::RePeek;""",
        target=ATOM_TEST,
        test="RejectAtomicity.AnStpCancelCarriesTheRestingClientOrderId",
    ),
    Mutation(
        name="matcher-cancel-both-clord-zero",
        why="STP CancelBoth maker-side cancel report drops the resting order's clientOrderId",
        file=MATCHER,
        old="""      case STPMode::CancelBoth:
        sink(OrderCanceled{m.id, order.symbol, CancelReason::SelfTradePrevention, m.accountId,
                           m.clientOrderId, m.leaves + m.hidden, m.cumQty});
        book.cancel(m.id);
        return StpOutcome::CancelTaker;""",
        new="""      case STPMode::CancelBoth:
        sink(OrderCanceled{m.id, order.symbol, CancelReason::SelfTradePrevention, m.accountId,
                           0, m.leaves + m.hidden, m.cumQty});
        book.cancel(m.id);
        return StpOutcome::CancelTaker;""",
        target=ATOM_TEST,
        test="RejectAtomicity.AnStpCancelBothCarriesBothClientOrderIds",
    ),
    Mutation(
        name="matcher-cancel-decrement-clord-zero",
        why="STP Decrement full-cancel report (resting <= incoming) drops the resting order's clientOrderId",
        file=MATCHER,
        old="""          sink(OrderCanceled{m.id, order.symbol, CancelReason::SelfTradePrevention, m.accountId,
                             m.clientOrderId, m.leaves + m.hidden, m.cumQty});
          book.cancel(m.id);
        }
        else  // incoming smaller: reduce resting, incoming fully decremented""",
        new="""          sink(OrderCanceled{m.id, order.symbol, CancelReason::SelfTradePrevention, m.accountId,
                             0, m.leaves + m.hidden, m.cumQty});
          book.cancel(m.id);
        }
        else  // incoming smaller: reduce resting, incoming fully decremented""",
        target=ATOM_TEST,
        test="RejectAtomicity.AnStpDecrementCancelCarriesTheRestingClientOrderId",
    ),
    Mutation(
        name="matcher-fill-time-risk-clord-zero",
        why="fill-time perp risk block cancel report drops the blocked maker's clientOrderId",
        file=MATCHER,
        old="""            sink(OrderCanceled{blockedId, order.symbol, lim.reason, blockedAcct, blockedClOrd,
                               m->leaves + m->hidden, m->cumQty});""",
        new="""            sink(OrderCanceled{blockedId, order.symbol, lim.reason, blockedAcct, 0,
                               m->leaves + m->hidden, m->cumQty});""",
        target=ATOM_TEST,
        test="RejectAtomicity.AFillTimeRiskCancelCarriesTheRestingClientOrderId",
    ),

    # ---- risk-block clientOrderId captured after the resting-hold hook -----
    Mutation(
        name="matcher-risk-block-clord-captured-after-hook",
        why=("moves blockedClOrd's capture from before the onRestingHolds_ hook to after it -- "
             "the comment says the capture exists BECAUSE the hook may reshape the book, so "
             "reading m-> after it ran is the bug the comment warns against"),
        file=MATCHER,
        old="""            const OrderId blockedId = m->id;
            const uint64_t blockedAcct = m->accountId;
            const uint64_t blockedClOrd = m->clientOrderId;
            if (onRestingHolds_ && onRestingHolds_(blockedId))
            {
              continue;  // hook may have reshaped the book -- re-peek
            }
            sink(OrderCanceled{blockedId, order.symbol, lim.reason, blockedAcct, blockedClOrd,""",
        new="""            const OrderId blockedId = m->id;
            const uint64_t blockedAcct = m->accountId;
            if (onRestingHolds_ && onRestingHolds_(blockedId))
            {
              continue;  // hook may have reshaped the book -- re-peek
            }
            const uint64_t blockedClOrd = m->clientOrderId;
            sink(OrderCanceled{blockedId, order.symbol, lim.reason, blockedAcct, blockedClOrd,""",
        target=ATOM_TEST,
        test="RejectAtomicity.AFillTimeRiskCancelCarriesTheRestingClientOrderId",
    ),

    # ---- onModify: rejectHoldsFor() moved back above one check at a time ---
    Mutation(
        name="modify-holds-before-existence",
        why="resolves the order's holds before even checking the order exists (pub_.tracked)",
        file=ORDERS,
        old="""  if (!pub_.tracked(m.id))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownOrder, m.accountId, true});
    return;
  }
  const uint64_t owner = ownerOf(m.id);
  if (m.newQty.raw() <= 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidQuantity, m.accountId, true});
    return;
  }
  const bool amendNamesAPrice = m.newPrice.raw() != 0;
  if (amendNamesAPrice)
  {
    if (const RejectReason r = priceRefusal(m.newPrice); r != RejectReason::None)
    {
      sink_(CancelRejected{m.id, m.symbol, r, owner, true});
      return;
    }
  }
  if (!cfg_.lotSize.isZero() && (m.newQty.raw() % cfg_.lotSize.raw()) != 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::LotSizeViolation, owner, true});
    return;
  }

  // Accepted. Resolve (reject) any last-look holds referencing this order
  // before reshaping it: both modify paths re-shape the order and its
  // reservation, and a hold left behind would later settle against a
  // reservation that no longer covers it. This restores the held slice to the
  // book, so the resting record is read only now.
  rejectHoldsFor(m.id);
  const RestingOrder* cur = book_.find(m.id);""",
        new="""  rejectHoldsFor(m.id);
  if (!pub_.tracked(m.id))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownOrder, m.accountId, true});
    return;
  }
  const uint64_t owner = ownerOf(m.id);
  if (m.newQty.raw() <= 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidQuantity, m.accountId, true});
    return;
  }
  const bool amendNamesAPrice = m.newPrice.raw() != 0;
  if (amendNamesAPrice)
  {
    if (const RejectReason r = priceRefusal(m.newPrice); r != RejectReason::None)
    {
      sink_(CancelRejected{m.id, m.symbol, r, owner, true});
      return;
    }
  }
  if (!cfg_.lotSize.isZero() && (m.newQty.raw() % cfg_.lotSize.raw()) != 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::LotSizeViolation, owner, true});
    return;
  }

  const RestingOrder* cur = book_.find(m.id);""",
        target=ATOM_TEST,
        test=None,  # no acceptance test names an unknown-id-with-holds case; run the whole binary
    ),
    Mutation(
        name="modify-holds-before-newqty",
        why="resolves the order's holds before the newQty == 0 check",
        file=ORDERS,
        old="""  if (!pub_.tracked(m.id))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownOrder, m.accountId, true});
    return;
  }
  const uint64_t owner = ownerOf(m.id);
  if (m.newQty.raw() <= 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidQuantity, m.accountId, true});
    return;
  }
  const bool amendNamesAPrice = m.newPrice.raw() != 0;
  if (amendNamesAPrice)
  {
    if (const RejectReason r = priceRefusal(m.newPrice); r != RejectReason::None)
    {
      sink_(CancelRejected{m.id, m.symbol, r, owner, true});
      return;
    }
  }
  if (!cfg_.lotSize.isZero() && (m.newQty.raw() % cfg_.lotSize.raw()) != 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::LotSizeViolation, owner, true});
    return;
  }

  // Accepted. Resolve (reject) any last-look holds referencing this order
  // before reshaping it: both modify paths re-shape the order and its
  // reservation, and a hold left behind would later settle against a
  // reservation that no longer covers it. This restores the held slice to the
  // book, so the resting record is read only now.
  rejectHoldsFor(m.id);
  const RestingOrder* cur = book_.find(m.id);""",
        new="""  if (!pub_.tracked(m.id))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownOrder, m.accountId, true});
    return;
  }
  const uint64_t owner = ownerOf(m.id);
  rejectHoldsFor(m.id);
  if (m.newQty.raw() <= 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidQuantity, m.accountId, true});
    return;
  }
  const bool amendNamesAPrice = m.newPrice.raw() != 0;
  if (amendNamesAPrice)
  {
    if (const RejectReason r = priceRefusal(m.newPrice); r != RejectReason::None)
    {
      sink_(CancelRejected{m.id, m.symbol, r, owner, true});
      return;
    }
  }
  if (!cfg_.lotSize.isZero() && (m.newQty.raw() % cfg_.lotSize.raw()) != 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::LotSizeViolation, owner, true});
    return;
  }

  const RestingOrder* cur = book_.find(m.id);""",
        target=ATOM_TEST,
        test="RejectAtomicity.AModifyWithAnInvalidQuantityLeavesTheHoldsIntact",
    ),
    Mutation(
        name="modify-holds-before-tick",
        why="resolves the order's holds before the tick-size check (shared code path with the band check)",
        file=ORDERS,
        old="""  if (!pub_.tracked(m.id))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownOrder, m.accountId, true});
    return;
  }
  const uint64_t owner = ownerOf(m.id);
  if (m.newQty.raw() <= 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidQuantity, m.accountId, true});
    return;
  }
  const bool amendNamesAPrice = m.newPrice.raw() != 0;
  if (amendNamesAPrice)
  {
    if (const RejectReason r = priceRefusal(m.newPrice); r != RejectReason::None)
    {
      sink_(CancelRejected{m.id, m.symbol, r, owner, true});
      return;
    }
  }
  if (!cfg_.lotSize.isZero() && (m.newQty.raw() % cfg_.lotSize.raw()) != 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::LotSizeViolation, owner, true});
    return;
  }

  // Accepted. Resolve (reject) any last-look holds referencing this order
  // before reshaping it: both modify paths re-shape the order and its
  // reservation, and a hold left behind would later settle against a
  // reservation that no longer covers it. This restores the held slice to the
  // book, so the resting record is read only now.
  rejectHoldsFor(m.id);
  const RestingOrder* cur = book_.find(m.id);""",
        new="""  if (!pub_.tracked(m.id))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownOrder, m.accountId, true});
    return;
  }
  const uint64_t owner = ownerOf(m.id);
  if (m.newQty.raw() <= 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidQuantity, m.accountId, true});
    return;
  }
  rejectHoldsFor(m.id);
  const bool amendNamesAPrice = m.newPrice.raw() != 0;
  if (amendNamesAPrice)
  {
    if (const RejectReason r = priceRefusal(m.newPrice); r != RejectReason::None)
    {
      sink_(CancelRejected{m.id, m.symbol, r, owner, true});
      return;
    }
  }
  if (!cfg_.lotSize.isZero() && (m.newQty.raw() % cfg_.lotSize.raw()) != 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::LotSizeViolation, owner, true});
    return;
  }

  const RestingOrder* cur = book_.find(m.id);""",
        target=ATOM_TEST,
        test="RejectAtomicity.AModifyOffTickLeavesTheHoldsIntact",
    ),
    Mutation(
        name="modify-holds-before-band",
        why="resolves the order's holds before the band check -- same code change as modify-holds-before-tick, checked against the band test",
        file=ORDERS,
        old="""  if (!pub_.tracked(m.id))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownOrder, m.accountId, true});
    return;
  }
  const uint64_t owner = ownerOf(m.id);
  if (m.newQty.raw() <= 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidQuantity, m.accountId, true});
    return;
  }
  const bool amendNamesAPrice = m.newPrice.raw() != 0;
  if (amendNamesAPrice)
  {
    if (const RejectReason r = priceRefusal(m.newPrice); r != RejectReason::None)
    {
      sink_(CancelRejected{m.id, m.symbol, r, owner, true});
      return;
    }
  }
  if (!cfg_.lotSize.isZero() && (m.newQty.raw() % cfg_.lotSize.raw()) != 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::LotSizeViolation, owner, true});
    return;
  }

  // Accepted. Resolve (reject) any last-look holds referencing this order
  // before reshaping it: both modify paths re-shape the order and its
  // reservation, and a hold left behind would later settle against a
  // reservation that no longer covers it. This restores the held slice to the
  // book, so the resting record is read only now.
  rejectHoldsFor(m.id);
  const RestingOrder* cur = book_.find(m.id);""",
        new="""  if (!pub_.tracked(m.id))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownOrder, m.accountId, true});
    return;
  }
  const uint64_t owner = ownerOf(m.id);
  if (m.newQty.raw() <= 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidQuantity, m.accountId, true});
    return;
  }
  rejectHoldsFor(m.id);
  const bool amendNamesAPrice = m.newPrice.raw() != 0;
  if (amendNamesAPrice)
  {
    if (const RejectReason r = priceRefusal(m.newPrice); r != RejectReason::None)
    {
      sink_(CancelRejected{m.id, m.symbol, r, owner, true});
      return;
    }
  }
  if (!cfg_.lotSize.isZero() && (m.newQty.raw() % cfg_.lotSize.raw()) != 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::LotSizeViolation, owner, true});
    return;
  }

  const RestingOrder* cur = book_.find(m.id);""",
        target=ATOM_TEST,
        test="RejectAtomicity.AModifyOutsideTheBandLeavesTheHoldsIntact",
    ),

    # ---- onModify: existence check reverted to reading the book directly ---
    Mutation(
        name="modify-existence-back-to-book-find",
        why=("existence goes back to book_.find(m.id) == nullptr instead of pub_.tracked(m.id) -- "
             "wrong for a maker whose whole size is held out of the book (T005 finding 21's own "
             "motivating case). No acceptance test builds that fully-held-out state: HeldMaker "
             "only holds 3 of 5, leaving 2 resting, so book_.find still succeeds either way"),
        file=ORDERS,
        old="""  if (!pub_.tracked(m.id))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownOrder, m.accountId, true});
    return;
  }""",
        new="""  if (book_.find(m.id) == nullptr)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownOrder, m.accountId, true});
    return;
  }""",
        target=ATOM_TEST,
        test=None,  # run the whole binary; no test builds a fully-held-out maker
    ),

    # ---- applyQuote: instrument-state gate --------------------------------
    Mutation(
        name="quote-instrument-state-not-consulted",
        why="applyQuote no longer asks instrumentStateRefusal() at all before cancelling the two legs",
        file=QUOTE,
        old="""  if (const RejectReason r = instrumentStateRefusal(); r != RejectReason::None)
  {
    sink_(OrderRejected{q.bidId, q.symbol, r, q.accountId, q.clientOrderId});
    return;
  }
  rejectHoldsFor(q.bidId);  // a replaced quote may carry open holds""",
        new="""  rejectHoldsFor(q.bidId);  // a replaced quote may carry open holds""",
        target=ATOM_TEST,
        test="RejectAtomicity.AQuoteIntoAHaltedInstrumentLeavesBothLegsResting:"
             "RejectAtomicity.AQuoteIntoAClosedInstrumentLeavesBothLegsResting",
    ),
    Mutation(
        name="quote-instrument-state-checked-after-cancels",
        why="applyQuote still asks instrumentStateRefusal(), but after both legs are already cancelled -- too late to save them",
        file=QUOTE,
        old="""  if (const RejectReason r = instrumentStateRefusal(); r != RejectReason::None)
  {
    sink_(OrderRejected{q.bidId, q.symbol, r, q.accountId, q.clientOrderId});
    return;
  }
  rejectHoldsFor(q.bidId);  // a replaced quote may carry open holds
  rejectHoldsFor(q.askId);
  if (auto ro = book_.cancel(q.bidId))
  {
    const uint64_t acct = ownerOf(q.bidId);
    releaseReservation(q.bidId);
    forgetOrder(q.bidId);
    sink_(OrderCanceled{q.bidId, cfg_.id, CancelReason::UserRequested, acct, ro->clientOrderId,
                        ro->leaves + ro->hidden, ro->cumQty});
  }
  if (auto ro = book_.cancel(q.askId))
  {
    const uint64_t acct = ownerOf(q.askId);
    releaseReservation(q.askId);
    forgetOrder(q.askId);
    sink_(OrderCanceled{q.askId, cfg_.id, CancelReason::UserRequested, acct, ro->clientOrderId,
                        ro->leaves + ro->hidden, ro->cumQty});
  }""",
        new="""  rejectHoldsFor(q.bidId);  // a replaced quote may carry open holds
  rejectHoldsFor(q.askId);
  if (auto ro = book_.cancel(q.bidId))
  {
    const uint64_t acct = ownerOf(q.bidId);
    releaseReservation(q.bidId);
    forgetOrder(q.bidId);
    sink_(OrderCanceled{q.bidId, cfg_.id, CancelReason::UserRequested, acct, ro->clientOrderId,
                        ro->leaves + ro->hidden, ro->cumQty});
  }
  if (auto ro = book_.cancel(q.askId))
  {
    const uint64_t acct = ownerOf(q.askId);
    releaseReservation(q.askId);
    forgetOrder(q.askId);
    sink_(OrderCanceled{q.askId, cfg_.id, CancelReason::UserRequested, acct, ro->clientOrderId,
                        ro->leaves + ro->hidden, ro->cumQty});
  }
  if (const RejectReason r = instrumentStateRefusal(); r != RejectReason::None)
  {
    sink_(OrderRejected{q.bidId, q.symbol, r, q.accountId, q.clientOrderId});
    return;
  }""",
        target=ATOM_TEST,
        test="RejectAtomicity.AQuoteIntoAHaltedInstrumentLeavesBothLegsResting:"
             "RejectAtomicity.AQuoteIntoAClosedInstrumentLeavesBothLegsResting",
    ),

    # ---- validate(): instrument-state gate dropped for NewOrder ------------
    Mutation(
        name="validate-no-longer-calls-instrument-state-refusal",
        why=("validate(NewOrder) stops asking instrumentStateRefusal() -- a plain NewOrder into a "
             "halted/closed/delisted instrument would no longer be refused for that reason. No test "
             "in this file submits a bare NewOrder into a halted/closed instrument; the halted/closed "
             "coverage here goes only through applyQuote, which asks the gate itself"),
        file=VALIDATE,
        old="""  if (o.symbol != cfg_.id)
  {
    return RejectReason::UnknownSymbol;
  }
  if (const RejectReason r = instrumentStateRefusal(); r != RejectReason::None)
  {
    return r;
  }
  if (o.quantity.raw() <= 0)""",
        new="""  if (o.symbol != cfg_.id)
  {
    return RejectReason::UnknownSymbol;
  }
  if (o.quantity.raw() <= 0)""",
        target=ATOM_TEST,
        test=None,  # run the whole binary
    ),

    # ---- dispatch.inl: symbol guards removed, one at a time ----------------
    Mutation(
        name="dispatch-setrisklimits-guard-removed",
        why="SetRiskLimits applies unconditionally again, with no symbol check",
        file=DISPATCH,
        old="""    if (rl->symbol == cfg_.id)
    {
      applyRiskLimits(*rl);  // sequenced -> journaled -> replayed
    }""",
        new="""    applyRiskLimits(*rl);  // sequenced -> journaled -> replayed""",
        target=ATOM_TEST,
        test="RejectAtomicity.SetRiskLimitsForAForeignSymbolIsIgnored",
    ),
    Mutation(
        name="dispatch-setadmissionprofile-guard-removed",
        why="SetAdmissionProfile applies unconditionally again, with no symbol check",
        file=DISPATCH,
        old="""    if (ap->symbol == cfg_.id)
    {
      setAdmissionProfile(ap->account, ap->profile);  // sequenced -> journaled -> replayed
    }""",
        new="""    setAdmissionProfile(ap->account, ap->profile);  // sequenced -> journaled -> replayed""",
        target=ATOM_TEST,
        test="RejectAtomicity.SetAdmissionProfileForAForeignSymbolIsIgnored",
    ),
    Mutation(
        name="dispatch-setaccountrisklimits-guard-removed",
        why="SetAccountRiskLimits applies unconditionally again, with no symbol check",
        file=DISPATCH,
        old="""    if (al->symbol == cfg_.id)
    {
      credit_.setAccountLimits(*al);  // sequenced -> journaled -> replayed (W26-T064)
    }""",
        new="""    credit_.setAccountLimits(*al);  // sequenced -> journaled -> replayed (W26-T064)""",
        target=ATOM_TEST,
        test="RejectAtomicity.SetAccountRiskLimitsForAForeignSymbolIsIgnored",
    ),
    Mutation(
        name="dispatch-setrisklimits-guard-wrong-field",
        why="SetRiskLimits guard compares symbol against cfg_.maxOpenOrders instead of cfg_.id -- always false in these tests, so the command never applies even on its own symbol",
        file=DISPATCH,
        old="""    if (rl->symbol == cfg_.id)
    {
      applyRiskLimits(*rl);  // sequenced -> journaled -> replayed
    }""",
        new="""    if (rl->symbol == cfg_.maxOpenOrders)
    {
      applyRiskLimits(*rl);  // sequenced -> journaled -> replayed
    }""",
        target=ATOM_TEST,
        test="RejectAtomicity.ControlTheThreeLimitCommandsStillApplyOnTheOwnSymbol",
    ),

    # ---- adversarial: our own -----------------------------------------------
    Mutation(
        name="adv-oco-unlink-skips-multi-member-groups",
        why=("OcoBook::unlinkGrouped becomes a no-op whenever the group currently has more than one "
             "member -- a plausible 'optimization' that happens to break every real OCO pair, since "
             "a leg refused past the commit point is refused while its sibling is still resting "
             "(group size 2)"),
        file=OCOBOOK,
        old="""  void unlinkGrouped(OrderId id)
  {
    auto it = group_.find(id);
    if (it == group_.end())
    {
      return;
    }
    if (auto gm = members_.find(it->second); gm != members_.end())
    {
      auto& v = gm->second;
      v.erase(std::remove(v.begin(), v.end(), id), v.end());
      if (v.empty())
      {
        members_.erase(gm);
      }
    }
    group_.erase(it);
  }""",
        new="""  void unlinkGrouped(OrderId id)
  {
    auto it = group_.find(id);
    if (it == group_.end())
    {
      return;
    }
    if (auto gm = members_.find(it->second); gm != members_.end())
    {
      auto& v = gm->second;
      if (v.size() > 1)
      {
        return;
      }
      v.erase(std::remove(v.begin(), v.end(), id), v.end());
      if (v.empty())
      {
        members_.erase(gm);
      }
    }
    group_.erase(it);
  }""",
        target=OCO_TEST,
        test=None,  # expected to break all three commit-boundary tests
    ),
    Mutation(
        name="adv-stp-cancel-oldest-account-zeroed",
        why="STP CancelOldest carries the right clientOrderId but reports account 0 instead of the maker's own account",
        file=MATCHER,
        old="""      case STPMode::CancelOldest:
        sink(OrderCanceled{m.id, order.symbol, CancelReason::SelfTradePrevention, m.accountId,
                           m.clientOrderId, m.leaves + m.hidden, m.cumQty});
        book.cancel(m.id);
        return StpOutcome::RePeek;""",
        new="""      case STPMode::CancelOldest:
        sink(OrderCanceled{m.id, order.symbol, CancelReason::SelfTradePrevention, 0,
                           m.clientOrderId, m.leaves + m.hidden, m.cumQty});
        book.cancel(m.id);
        return StpOutcome::RePeek;""",
        target=ATOM_TEST,
        test="RejectAtomicity.AnStpCancelCarriesTheRestingClientOrderId",
    ),
    Mutation(
        name="adv-quote-state-checked-before-dedup",
        why=("swaps the order of the dedup check and the instrument-state check in applyQuote: state "
             "is now asked FIRST, so a quote refused for Halted/MarketClosed never registers its "
             "clientOrderId in the dedup index at all. No acceptance test resubmits the same "
             "clientOrderId after a state refusal to notice the slot was never consumed"),
        file=QUOTE,
        old="""  if (!clOrdIdChecked && clOrdIdDuplicate(q.accountId, q.clientOrderId))
  {
    sink_(OrderRejected{q.bidId, q.symbol, RejectReason::DuplicateClientOrderId, q.accountId,
                        q.clientOrderId});
    return;
  }
  // The instrument's own state, asked BEFORE anything is pulled. A quote is a
  // replace: it cancels what the two ids name and submits two new legs, and
  // onNew refuses those legs on exactly this gate. Reading it only there left
  // the maker with an empty book on a halted or closed instrument -- its
  // quotes pulled, both replacements refused, and no way to put them back
  // until the instrument trades again. Refused whole, like every other
  // whole-quote refusal above.
  //
  // Delisting is not one of these in practice: that transition cancels the
  // whole book by design (SessionEffect::CancelBookAfter), so there is nothing
  // left for the quote to protect. It is refused here anyway, for the same
  // reason the other two are -- the legs would be refused below.
  if (const RejectReason r = instrumentStateRefusal(); r != RejectReason::None)
  {
    sink_(OrderRejected{q.bidId, q.symbol, r, q.accountId, q.clientOrderId});
    return;
  }""",
        new="""  if (const RejectReason r = instrumentStateRefusal(); r != RejectReason::None)
  {
    sink_(OrderRejected{q.bidId, q.symbol, r, q.accountId, q.clientOrderId});
    return;
  }
  if (!clOrdIdChecked && clOrdIdDuplicate(q.accountId, q.clientOrderId))
  {
    sink_(OrderRejected{q.bidId, q.symbol, RejectReason::DuplicateClientOrderId, q.accountId,
                        q.clientOrderId});
    return;
  }""",
        target=ATOM_TEST,
        test="RejectAtomicity.AQuoteIntoAHaltedInstrumentLeavesBothLegsResting:"
             "RejectAtomicity.AQuoteIntoAClosedInstrumentLeavesBothLegsResting",
    ),
    Mutation(
        name="adv-dispatch-setadmissionprofile-guard-refuses-with-event",
        why=("the SetAdmissionProfile symbol guard, instead of silently ignoring a foreign-symbol "
             "record like every other guarded branch, emits a CancelRejected for it. The acceptance "
             "test only checks the profile was not applied and that a later order is unaffected -- it "
             "never asserts the ignored command produced no event of its own"),
        file=DISPATCH,
        old="""    if (ap->symbol == cfg_.id)
    {
      setAdmissionProfile(ap->account, ap->profile);  // sequenced -> journaled -> replayed
    }""",
        new="""    if (ap->symbol == cfg_.id)
    {
      setAdmissionProfile(ap->account, ap->profile);  // sequenced -> journaled -> replayed
    }
    else
    {
      sink_(CancelRejected{0, ap->symbol, RejectReason::UnknownSymbol, ap->account, true});
    }""",
        target=ATOM_TEST,
        test="RejectAtomicity.SetAdmissionProfileForAForeignSymbolIsIgnored",
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


def rebuild(target: str) -> str:
    result = subprocess.run(
        [CMAKE, "--build", str(BUILD), "--target", target, "-j", BUILD_JOBS],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT, cwd=REPO,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise SystemExit(f"rebuild of {target} failed:\n{output[-4000:]}")
    if "Building CXX" not in output:
        raise SystemExit(
            f"rebuild of {target} compiled nothing -- the result would have been "
            f"a stale binary, so the run is refused:\n{output[-2000:]}"
        )
    return output


def run_test(target: str, gtest_filter: str | None) -> tuple[int, str]:
    binary = BUILD / "venue" / target
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
        print(f"  control {target:<32} {state}   {summary.strip()}")
        ok = ok and code == 0
    return ok


def run_mutation(m: Mutation) -> bool:
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

    try:
        removed = object_files(m.target)
        for obj in removed:
            obj.unlink()
        print(f"  removed {len(removed)} object file(s) for {m.target}")

        output = rebuild(m.target)
        compiled = sum(1 for line in output.splitlines() if "Building CXX" in line)
        print(f"  rebuilt {m.target}: {compiled} 'Building CXX' line(s)")

        code, test_output = run_test(m.target, m.test)
        killed = code != 0
        shown_filter = m.test if m.test else "(whole binary)"
        print(f"  {m.target} --gtest_filter={shown_filter} -> exit {code} "
              f"({'RED, mutation killed' if killed else 'GREEN, MUTATION SURVIVED'})")
        if not killed:
            print("  ----- surviving mutation, test output -----")
            print("\n".join(test_output.splitlines()[-25:]))
    finally:
        path.write_text(original)
        after = sha256(path)
        print(f"  sha256  after   {after}")
        if after != before:
            raise SystemExit("restore failed: the file does not hash back to its original")
        for obj in object_files(m.target):
            obj.unlink()

    return killed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="print the mutations and exit")
    parser.add_argument("--only", action="append", default=[], help="run only these mutations")
    args = parser.parse_args()

    if args.list:
        for m in MUTATIONS:
            print(f"{m.name:<48} {m.target:<30} {m.test or '(whole binary)'}")
        return 0

    if not (BUILD / "CMakeCache.txt").is_file():
        raise SystemExit(
            f"{BUILD} is not configured; run\n"
            "  cmake --preset venue-lite"
        )

    selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
    if not selected:
        raise SystemExit(f"no mutation matches {args.only}")
    targets = sorted({m.target for m in selected})

    print("control run before the mutations")
    if not control(targets):
        raise SystemExit("the unmutated tree is not green; nothing below would mean anything")

    results = [(m, run_mutation(m)) for m in selected]

    print("\ncontrol run after the mutations")
    restored = control(targets)

    print("\nsummary")
    for m, killed in results:
        print(f"  {'RED  ' if killed else 'ALIVE'}  {m.name:<48} {m.target}")
    survived = [m.name for m, killed in results if not killed]
    if survived:
        print(f"\n{len(survived)} mutation(s) survived: {', '.join(survived)}")
    if not restored:
        print("\nthe tree did not come back green after the run")
    return 0 if not survived and restored else 1


if __name__ == "__main__":
    sys.exit(main())
