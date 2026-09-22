# Quoting over FIX: MassQuote / QuoteCancel / QuoteStatusReport

`FixCodec` (`venue/include/flox-venue/fix_codec.h`) and `FixConnection`
(`venue/include/flox-venue/fix_session.h`) decode a maker's whole ladder on
one symbol from a single FIX message, the same `QuoteLadder` command a
non-FIX caller builds directly (`venue/include/flox-venue/messages.h`,
`venue/include/flox-venue/engine/quote_mmp.inl`). Before T063 the FIX
perimeter accepted `D`/`F`/`G` (order entry) only -- a market maker speaking
FIX could not quote at all.

## MassQuote (35=i) in

| Tag | Field | Maps to |
|---|---|---|
| 1 | Account | `QuoteLadder::accountId` -- **required**, see below |
| 117 | QuoteID | `QuoteLadder::clientOrderId` -- the ladder's own name; deduplicated once for the whole ladder, the same rule a `Quote` already applies to its two legs |
| 299 | QuoteEntryID | starts one level; levels are read in the order the entries arrive, up to `kQuoteLadderLevels` |
| 55 | Symbol | `QuoteLadder::symbol`, once per entry; every entry of one MassQuote must name the same symbol |
| 132 | BidPx | `QuoteLadderLevel::bidPrice` |
| 133 | OfferPx | `QuoteLadderLevel::askPrice` |
| 134 | BidSize | `QuoteLadderLevel::bidQty` |
| 135 | OfferSize | `QuoteLadderLevel::askQty` |

`NoQuoteSets` (296), `QuoteSetID` (302) and `NoQuoteEntries` (295) are the FIX
4.4 group counts; the decoder does not need them -- entries are read as they
arrive, delimited by `QuoteEntryID`, the way `FixMdCodec` reads repeating
market-data entries.

The decode is refused (no command is built, no engine call happens) when:

- **Account (1) or QuoteID (117) is missing.**
- **More than `kQuoteLadderLevels` entries are named.** `QuoteLadder::levels`
  above that is *clamped* by the engine (a live ladder never shrinks the
  wire format), so this is the codec's own check -- a client that named nine
  levels asked for something the ladder cannot represent and is told so,
  rather than having its ninth level silently dropped.
- **The levels are not ordered as received: bid strictly descending, ask
  strictly ascending.** The shape a ladder walking away from the mid always
  has. A MassQuote that does not honour it is refused rather than silently
  sorted -- sorting would submit a ladder the sender never asked for under
  its own QuoteID.
- **A level is missing one of BidPx/OfferPx/BidSize/OfferSize**, or a price
  or size does not parse as a clean fixed-point decimal (the same strict
  `decwire` parse every other FIX numeric field gets: no doubles, no
  exponents, no silent coercion).
- **An entry names a different symbol than the first one.** One MassQuote is
  one `QuoteLadder`, and a `QuoteLadder` is one symbol.

### The order-id block

FIX `MassQuote` carries no per-leg order id -- 117 QuoteID is the *ladder's*
own name, not an id base for its `kQuoteLadderLevels` legs a side. The venue
therefore derives `bidIdBase`/`askIdBase` deterministically from
`(accountId, symbol)` (`FixCodec::quoteLadderIdBase`): every MassQuote or
QuoteCancel from one account on one symbol addresses the same
`2*kQuoteLadderLevels`-id block, bid block then ask block. That is what lets
a QuoteCancel -- which names no ids of its own -- take down exactly the legs
the last MassQuote on that account/symbol put up, and what lets a later
MassQuote *replace* them rather than add a second set beside them, which is
the "replaced atomically, same ids" contract `QuoteLadder` already
documents.

This is also why Account (1) is required rather than left to the session to
stamp after decode, unlike `NewOrderSingle`: the id block is derived from
`accountId` *inside* `decode()`, before any session-level stamping could run,
so a decode that tolerated a missing Account would leave the ladder's own
`accountId` field disagreeing with the id block it was built from.

## QuoteCancel (35=Z) in

| Tag | Field | Maps to |
|---|---|---|
| 1 | Account | `QuoteLadder::accountId` -- required |
| 55 | Symbol | `QuoteLadder::symbol` -- required |
| 298 | QuoteCancelType | not read: every engine shard already handles exactly one symbol, so "cancel every symbol" (4) and "cancel this symbol" are the same operation from here |

Decodes to a `QuoteLadder` with `levels = 0` targeting the same id block a
MassQuote from this account on this symbol would -- the engine's existing
"a shorter ladder takes its surplus levels down" rule does the rest; a
zero-level ladder takes ALL of them down.

## QuoteStatusReport (35=AI) out

Two different things answer with this message, both intentionally:

1. **The immediate acknowledgment of the MassQuote/QuoteCancel frame itself**,
   sent by `FixConnection::onFrame` before the frame falls through to the
   ordinary decode/admission/submit pipeline -- the same session layer that
   answers Logon, Heartbeat and an unknown MsgType directly, not an engine
   event. `297` QuoteStatus is `0` (Accepted) for anything this venue could
   turn into a `QuoteLadder`, `5` (Rejected) with `58` Text for anything it
   could not (see the refusal list above). `117` echoes the request's
   QuoteID when it had one.
2. **An engine-side refusal of a ladder that passed that check** --
   `RejectReason::QuoteNotPermitted` (the admission profile denies quoting,
   `AdmissionDeny::DenyQuote`) -- answers through the *same* report shape,
   from `FixCodec::encode`'s own `QuoteNotPermitted` case, over the ordinary
   `OutboundEvent` -> exec-report path. FIX has no honest `ExecType` for "your
   ladder never reached the book", and switching to an `ExecutionReport`
   partway through one MassQuote's story would be a second message format
   for the same conversation.

Every other engine-side event a ladder's legs produce (`OrderAccepted`,
`OrderCanceled`, `OrderExecuted`, ...) is unaffected and still travels as an
ordinary `ExecutionReport` per leg, exactly as it does for a `QuoteLadder`
submitted through any other transport -- `QuoteStatusReport` answers the
MassQuote/QuoteCancel *request*, not the legs it produces.

The immediate acknowledgment is sent the same way Logon/Heartbeat/session
Reject are (sequenced, not logged for resend): a `ResendRequest` whose range
covers it answers with `SequenceReset-GapFill` over that seq, same as every
other session-layer reply.

## Quotes-only admission

A market maker that only ever sends `MassQuote`/`QuoteCancel` should not be
able to place or cancel a plain order through the same session --
`AdmissionProfile::deny` carries `DenyNewOrder` (refuses `NewOrder`,
`RejectReason::NewOrderNotPermitted`) alongside the existing `DenyCancel`
(refuses `CancelOrder`) for exactly this profile, with `DenyQuote` left
unset. See `docs/venue/matching.md` for the full `AdmissionDeny` table.

## What does not change

The engine (`MatchingEngine::onQuoteLadder`, `engine::QuoteLadderLegs`) is
untouched: a `QuoteLadder` decoded from a MassQuote runs through the exact
same path, id for id and event for event, as one submitted through any other
transport. SBE's own inbound `QuoteLadder` (schema version 8, template 7,
see `docs/venue/perimeter.md`) is unaffected. The golden replay corpus does
not change -- the perimeter gained a decoder, the engine gained nothing new
to replay.
