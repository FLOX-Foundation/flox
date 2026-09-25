# Quoting over FIX: MassQuote / QuoteCancel / QuoteStatusReport

`FixCodec` (`venue/include/flox-venue/fix_codec.h`) and `FixConnection`
(`venue/include/flox-venue/fix_session.h`) decode a maker's whole ladder on
one symbol from a single FIX message, the same `QuoteLadder` command a
non-FIX caller builds directly (`venue/include/flox-venue/messages.h`,
`venue/include/flox-venue/engine/quote_mmp.inl`). Before MassQuote/QuoteCancel support was added, the FIX
perimeter accepted `D`/`F`/`G` (order entry) only -- a market maker speaking
FIX could not quote at all.

## What the inbound codec accepts and refuses

`FixCodec::decode` is the same function for `D`/`F`/`G` and for
`i`/`Z`, so the strictness below is the same on all of them. Every refusal
fills in the reason of the two-argument
`decode(const std::string&, std::string*)`, formatted as
`<FixFieldName>(<tag>): <what was wrong>` -- `ClOrdID(11)`, `Account(1)`,
`Symbol(55)`. `FixConnection` puts that string in the `Text` (58) of the
`QuoteStatusReport` it answers a refused MassQuote/QuoteCancel with, which is
the only place the sender ever sees it. The one-argument `decode` remains the
decoder hook the gateways install.

### The id fields

FIX types `ClOrdID` (11), `OrigClOrdID` (41), `Account` (1) and `Symbol` (55)
as String; this venue carries them as integers. A value it cannot carry is
**refused naming the field**, never coerced -- see
`venue/include/flox-venue/fix_field_parse.h`. Accepted: decimal digits only,
and nothing else.

| Wire value | Answer |
|---|---|
| `42`, `0`, `18446744073709551615` | accepted (the last is the largest id this venue can carry) |
| `ORD-A1`, `BTC-USD`, `123ABC`, `4.2`, `0x10`, `4e2` | refused -- `strtoull` stopped at the first junk character and handed the engine what it had read so far, so two clients with alphanumeric names both became order id 0 |
| `` (empty) | refused |
| ` 42`, `42 ` | refused -- a leading space is not part of a decimal integer |
| `+42`, `-1` | refused -- no sign; `-1` used to wrap to `UINT64_MAX`, the id a client that legitimately named `UINT64_MAX` gets |
| `0042` | refused -- `0042` and `42` are two different ClOrdIDs, and accepting both would hand them one order id |
| `99999999999999999999` | refused -- above `UINT64_MAX`; `strtoull` saturated it onto `UINT64_MAX` and set `ERANGE`, which nobody read |
| `4294967296` in 55 | refused -- above `UINT32_MAX`; the cast to `SymbolId` truncated it to 0, another instrument's book |

`ClOrdID` (11) is required on `D` and `OrigClOrdID` (41) on `F`/`G`, as FIX
4.4 requires them. `Account` (1) and `Symbol` (55) stay optional on `D`/`F`/`G`
-- the session stamps the account and a shard already knows its symbol -- but a
value that *is* present has to parse. On `i` and `Z` both are required, for
the reason the id block section below gives.

### TimeInForce (59) and GTD

`TimeInForce` used to map 3 and 4 and send everything else to GTC, including
values that name a deadline this venue cannot keep. It now accepts exactly the
four the engine runs, and refuses the rest naming `TimeInForce(59)`.

| 59 | FIX 4.4 | Answer |
|---|---|---|
| *absent* | -- | GTC, the default |
| 1 | GoodTillCancel | `TimeInForce::GTC` |
| 3 | ImmediateOrCancel | `TimeInForce::IOC` |
| 4 | FillOrKill | `TimeInForce::FOK` |
| 6 | GoodTillDate | `TimeInForce::GTD`, with `ExpireTime` (126) below |
| 0, 2, 5, 7, 8 | Day, AtTheOpening, GoodTillCrossing, AtTheClose, AtCrossing | refused -- each ends at a session boundary or an auction this venue does not run, and resting one as GTC turns an order the sender gave a deadline into one that never expires |
| anything else | -- | refused |

There is no `TimeInForce` for post-only: that arrives as `ExecInst` (18) `6`
ParticipateDoNotInitiate, which is how FIX spells it, and `TimeInForce::POST_ONLY`
is reachable only through the non-FIX transports.

`ExpireTime` (126) is **required** by `59=6` and refused unless it is a FIX
`UTCTimestamp` -- `YYYYMMDD-HH:MM:SS` with optional `.sss` milliseconds,
`20260925-12:00:00.250`. It is read as UTC by civil-date arithmetic
(`fix_field_parse.h`), never through `mktime`/`strptime`: those read the host's
`TZ` and locale, and an expiry that moves with the venue host's
`/etc/localtime` is not an expiry. The milliseconds are kept
(`.250` -> `...250000000` in `NewOrder::expiryNs`), and the parse is the exact
inverse of the `SendingTime` (52) printer both ends of a session already share.
A month outside 1-12, a day the month does not have, an hour above 23, a year
before 1970, after 2261 (nanoseconds since the epoch no longer fit an int64) or any other shape is refused naming `ExpireTime(126)`. On a
`TimeInForce` other than GTD, tag 126 carries no FIX meaning and is ignored.

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

"Exactly the legs the last MassQuote on *that* account/symbol put up" is a
claim in two directions: one pair always reaches its own block, and no other
pair ever reaches it. The original fold, `account * 4099 + symbol`, only
delivered the first -- `(1, 4099)` and `(2, 0)` both fold to 8198 and were
handed the same sixteen ids, so either maker's QuoteCancel took the other's
ladder down. A bigger multiplier moves which pairs collide; it does not
remove the collision.

**The range.** The fold is positional, so each field has a width:

| Field | Width | Range |
|---|---|---|
| `Symbol` (55) | 32 bits | `0 .. 4294967295` -- the whole `SymbolId` type |
| `Account` (1) | 24 bits | `0 .. 16777215` (`FixCodec::kQuoteAccountLimit - 1`) |

`bidIdBase = kMarker + ((account << 32) | symbol) * 2 * kQuoteLadderLevels`,
`askIdBase = bidIdBase + kQuoteLadderLevels`. Injective over that whole range
by construction, and the blocks tile it: adjacent pairs are exactly one block
width apart, so no leg of one pair can land in another pair's block.

An `Account` above the range is **refused** naming `Account(1)`, on MassQuote
and QuoteCancel alike (`FixCodec::quoteIdBlockInRange`). A venue cannot fold
2^64 accounts x 2^32 symbols into 2^64 ids sixteen at a time -- the pigeonhole
is not negotiable -- so the choice is between refusing the pairs that do not
fit and wrapping two makers onto one ladder. A maker told "your account id is
outside the quoting range" can be given an account id inside it; a maker whose
quotes are cancelled by a stranger cannot tell that is what happened.

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
