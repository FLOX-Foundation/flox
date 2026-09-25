/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * FIX 4.4 codec for venue order entry (D/F/G in) and execution reports (35=8
 * out): real tag=value/SOH framing with BodyLength (9) and a validated
 * CheckSum (10).
 *
 * Numeric fields parse straight from the decimal string into fixed-point via
 * decwire (no double round-trip): a bad, exponent, over-precise or overflowing
 * number is rejected, not silently coerced. Required enum/quantity fields are
 * strict -- a missing or invalid Side (54), a present-but-unknown OrdType (40),
 * or a missing OrderQty (38) rejects the message rather than guessing a default.
 * The id fields FIX types as String -- ClOrdID (11), OrigClOrdID (41), Account
 * (1), Symbol (55) -- are held to the same rule through fix_field_parse.h: a
 * value this venue cannot carry as an integer is refused naming the field,
 * never coerced to 0 or to UINT64_MAX where the next sender would land on top
 * of it. Every refusal fills in the `reason` of the two-argument decode().
 * Outbound prices/quantities serialise exactly (100.25, not 100.250000).
 *
 * Framing, field parsing, the checksum, the session header and the SendingTime
 * format are not spelled out here: they come from flox/connector/fix/fix_wire.h,
 * which the FIX initiator uses too. The two ends of a session have to produce
 * the same bytes for the same message, and one implementation is what makes
 * that structural rather than a thing tests have to keep noticing.
 */
#pragma once

#include "flox-venue/decimal_wire.h"
#include "flox-venue/fix_field_parse.h"
#include "flox-venue/messages.h"

#include "flox/connector/fix/fix_wire.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox::venue
{

class FixCodec
{
 public:
  static constexpr char SOH = flox::fix::kSoh;

  // tag=value fields of a raw FIX message (last occurrence wins on repeats).
  static std::unordered_map<int, std::string> parseFields(const std::string& msg)
  {
    return flox::fix::parseFields(msg);
  }

  // Order-preserving tag/value scan. parseFields collapses repeats into a map
  // by design (order entry is flat tag=value with no repeats), which is
  // exactly wrong for MassQuote's repeating QuoteEntry groups (299/55/132/
  // 133/134/135, once per level): a map cannot hold more than one value per
  // tag, so a group HAS to be read straight off the wire instead. Mirrors
  // FixMdCodec::parseOrdered (market data's own repeating-group reason);
  // kept local rather than shared so this header stays independent of
  // fix_md_codec.h, a different message family with its own session layer.
  static std::vector<std::pair<int, std::string>> parseOrdered(const std::string& msg)
  {
    std::vector<std::pair<int, std::string>> f;
    size_t i = 0;
    while (i < msg.size())
    {
      const size_t eq = msg.find('=', i);
      if (eq == std::string::npos)
      {
        break;
      }
      size_t soh = msg.find(SOH, eq + 1);
      if (soh == std::string::npos)
      {
        soh = msg.size();
      }
      f.emplace_back(std::atoi(msg.substr(i, eq - i).c_str()), msg.substr(eq + 1, soh - eq - 1));
      i = soh + 1;
    }
    return f;
  }

  // Deterministic per-(account, symbol) resting order-id block for a FIX
  // quoting session's ladder. MassQuote carries no venue order id of
  // its own -- 117 QuoteID is the LADDER's own name (QuoteLadder::
  // clientOrderId), not an id base -- and QuoteCancel names no ids at all.
  // A pure function of (account, symbol) is what lets both answer from the
  // same formula with no state kept anywhere: every MassQuote/QuoteCancel
  // from one account on one symbol addresses the SAME
  // 2*kQuoteLadderLevels-id block (the bid block, then the ask block
  // immediately after it), so a later MassQuote replaces the very legs a
  // QuoteCancel would have taken down -- the "replaced atomically, same ids"
  // contract QuoteLadder already documents.
  // The (account, symbol) pairs a quoting id block can be derived for. The
  // fold is positional rather than arithmetic, so each field needs a width:
  // Symbol keeps all 32 bits SymbolId has, and Account gets 24 -- 16,777,216
  // accounts on one venue. A pair outside that is refused by decode() naming
  // Account(1), never folded modulo anything: wrapping is how two makers end
  // up sharing one ladder, which is the failure this range exists to make
  // impossible rather than unlikely.
  static constexpr uint64_t kQuoteAccountLimit = 1ULL << 24;

  static constexpr bool quoteIdBlockInRange(uint64_t accountId) noexcept
  {
    return accountId < kQuoteAccountLimit;
  }

  // Defined for a pair quoteIdBlockInRange accepts; decode() refuses the rest
  // before reaching here.
  static OrderId quoteLadderIdBase(uint64_t accountId, SymbolId symbol) noexcept
  {
    constexpr uint64_t kBlock = 2ULL * kQuoteLadderLevels;
    // Account in the high bits, symbol in the low 32, one block width per
    // pair: injective by construction over the whole range above. The
    // multiply-and-add this replaces (account * 4099 + symbol) was not --
    // (1, 4099) and (2, 0) both folded to 8198 and shared sixteen ids, so
    // either maker's QuoteCancel took the other's ladder down -- and no
    // choice of multiplier fixes that, it only moves which pairs collide.
    //
    // The marker keeps the block above a range a hand-assigned ClOrdID
    // (NewOrderSingle's 11, reused as the venue OrderId) would plausibly use,
    // purely so the two ranges read as distinct in a capture -- a quotes-only
    // session's DenyNewOrder profile is what actually keeps them from ever
    // colliding for real.
    constexpr uint64_t kMarker = 0x51'00000000ULL;  // 'Q'
    constexpr uint64_t kSymbolBits = 32;
    static_assert((((kQuoteAccountLimit - 1) << kSymbolBits) | 0xFFFFFFFFULL) <=
                      (std::numeric_limits<uint64_t>::max() - kMarker) / kBlock,
                  "the widest pair in range must still fit above the marker without wrapping");
    return static_cast<OrderId>(
        kMarker + (((accountId << kSymbolBits) | static_cast<uint64_t>(symbol)) * kBlock));
  }

  // FIX integrity: if a CheckSum (tag 10) is present it MUST be correct --
  // sum of every byte up to and including the SOH before "10=", mod 256.
  // Lenient when absent, for internal/test callers that don't append one.
  static bool checksumValid(const std::string& msg) { return flox::fix::checksumValid(msg); }

  // ---- inbound: FIX message -> InboundCommand ----
  //
  // A refusal says WHAT it refused: `*reason` is set on every rejection to
  // "<FixFieldName>(<tag>): <what was wrong>", which the session layer puts
  // in the Text (58) of the answer it sends back. "Malformed" tells a market
  // maker nothing about which of its tags to fix, and a codec that knows the
  // tag and drops it on the floor is the reason it could not.
  //
  // The one-argument overload stays: it is the decoder hook the gateways
  // install (the same shape the SBE decoder has), and a caller with nowhere
  // to put a reason should not have to invent a string to get an answer.
  static std::optional<InboundCommand> decode(const std::string& msg)
  {
    return decode(msg, nullptr);
  }

  static std::optional<InboundCommand> decode(const std::string& msg, std::string* reason)
  {
    std::unordered_map<int, std::string> f = parseFields(msg);

    auto has = [&](int t)
    { return f.count(t) != 0; };
    auto s = [&](int t)
    { return has(t) ? f[t] : std::string{}; };
    auto refuse = [&](const char* name, int tag, const char* why) -> std::optional<InboundCommand>
    {
      if (reason != nullptr)
      {
        *reason = std::string(name) + "(" + std::to_string(tag) + "): " + why;
      }
      return std::nullopt;
    };

    if (!checksumValid(msg))
    {
      return refuse("CheckSum", 10, "does not match the bytes of the message");
    }

    // The id fields FIX types as String (ClOrdID, OrigClOrdID, Account,
    // Symbol) and this venue carries as integers. Strict both ways: a value
    // the integer cannot hold is refused naming the field, never coerced, so
    // two senders can never be handed one id or one book. See
    // fix_field_parse.h for what "cannot hold" covers.
    auto u64 = [&](int t, uint64_t& out)
    { return fixfield::parseU64(s(t), out); };
    auto sym = [&](int t, SymbolId& out)
    { return fixfield::parseU32(s(t), out); };
    // Strict fixed-point parse of a decimal FIX field; false if the tag is
    // absent or the value is not a clean decimal.
    auto fix = [&](int t, int64_t& out)
    { return has(t) && decwire::parse(s(t), out); };

    const std::string type = s(35);
    if (type == "D")  // NewOrderSingle
    {
      NewOrder o;
      // ClOrdID (11) becomes the venue order id, so a name this venue cannot
      // carry is refused rather than folded onto 0 -- where the next client
      // with an unparseable name would land too, on top of this one's order.
      if (!has(11) || !u64(11, o.id))
      {
        return refuse("ClOrdID", 11, "required, and must be a decimal integer below 2^64");
      }
      o.clientOrderId = o.id;
      // Symbol (55) and Account (1) stay optional -- a shard already knows
      // its symbol and the session stamps the account -- but a value that IS
      // present has to parse: a truncated Symbol routes to another
      // instrument's book, a truncated Account bills another client.
      if (has(55) && !sym(55, o.symbol))
      {
        return refuse("Symbol", 55, "must be a decimal integer below 2^32");
      }
      if (has(1) && !u64(1, o.accountId))
      {
        return refuse("Account", 1, "must be a decimal integer below 2^64");
      }

      // Side (54) is required and must be Buy(1)/Sell(2) -- never a guessed default.
      const std::string side = s(54);
      if (side == "1")
      {
        o.side = Side::BUY;
      }
      else if (side == "2")
      {
        o.side = Side::SELL;
      }
      else
      {
        return refuse("Side", 54, "required, and must be 1 (Buy) or 2 (Sell)");
      }

      // OrderQty (38) required.
      int64_t qtyRaw;
      if (!fix(38, qtyRaw))
      {
        return refuse("OrderQty", 38, "required, and must be a plain decimal quantity");
      }
      o.quantity = Quantity::fromRaw(qtyRaw);

      // OrdType (40): absent -> Limit (the common default); present must be a
      // known type -- a garbage value is rejected, not mapped to Limit.
      if (has(40))
      {
        switch (std::atoi(s(40).c_str()))
        {
          case 1:
            o.type = OrderType::MARKET;
            break;
          case 2:
            o.type = OrderType::LIMIT;
            break;
          case 3:
            o.type = OrderType::STOP_MARKET;
            break;
          case 4:
            o.type = OrderType::STOP_LIMIT;
            break;
          default:
            return refuse("OrdType", 40, "names no order type this venue runs");
        }
      }
      else
      {
        o.type = OrderType::LIMIT;
      }

      // Optional decimals: present -> must parse.
      int64_t v;
      if (has(44))
      {
        if (!fix(44, v))
        {
          return refuse("Price", 44, "not a plain decimal price");
        }
        o.price = Price::fromRaw(v);
      }
      if (has(99))
      {
        if (!fix(99, v))
        {
          return refuse("StopPx", 99, "not a plain decimal price");
        }
        o.triggerPrice = Price::fromRaw(v);
      }
      if (has(111))
      {
        if (!fix(111, v))
        {
          return refuse("MaxFloor", 111, "not a plain decimal quantity");
        }
        o.visibleQuantity = Quantity::fromRaw(v);  // MaxFloor -> iceberg peak
      }

      // TimeInForce (59): absent -> GTC, the FIX default for a venue with no
      // session schedule. Present must name one of the four this venue can
      // actually honour: 1 GTC, 3 IOC, 4 FOK, 6 GTD. Day (0), AtTheOpening
      // (2), GoodTillCrossing (5) and AtTheClose (7) are real FIX 4.4 values
      // that end at a session boundary or an auction this venue does not
      // run, and mapping them onto GTC rests an order the sender asked to
      // live for one session -- forever. There is no TIF for post-only: that
      // arrives as ExecInst (18) 6 below, the way FIX spells it.
      if (has(59))
      {
        const std::string tif = s(59);
        if (tif == "1")
        {
          o.tif = TimeInForce::GTC;
        }
        else if (tif == "3")
        {
          o.tif = TimeInForce::IOC;
        }
        else if (tif == "4")
        {
          o.tif = TimeInForce::FOK;
        }
        else if (tif == "6")
        {
          // GTD is the one TimeInForce carrying a second required field:
          // without ExpireTime (126) there is no date to be good till, and
          // the order used to rest as a GTC that never expires.
          o.tif = TimeInForce::GTD;
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
          o.expiryNs = SeqNanos::fromRaw(expiry);
        }
        else
        {
          return refuse("TimeInForce", 59,
                        "names no time in force this venue runs: 1 GTC, 3 IOC, 4 FOK, 6 GTD");
        }
      }
      const std::string execInst = s(18);
      if (execInst.find('6') != std::string::npos)  // ParticipateDoNotInitiate
      {
        o.postOnly = true;
      }
      if (execInst.find('E') != std::string::npos)  // DoNotIncrease -> reduce-only
      {
        o.reduceOnly = true;
      }
      return InboundCommand{o};
    }
    if (type == "F")  // OrderCancelRequest
    {
      CancelOrder c;
      // OrigClOrdID (41) names the order to cancel: an unparseable one that
      // decoded to 0 would cancel whatever order 0 is, not this client's.
      if (!has(41) || !u64(41, c.id))
      {
        return refuse("OrigClOrdID", 41, "required, and must be a decimal integer below 2^64");
      }
      if (has(55) && !sym(55, c.symbol))
      {
        return refuse("Symbol", 55, "must be a decimal integer below 2^32");
      }
      if (has(1) && !u64(1, c.accountId))
      {
        return refuse("Account", 1, "must be a decimal integer below 2^64");
      }
      return InboundCommand{c};
    }
    if (type == "G")  // OrderCancelReplaceRequest
    {
      ModifyOrder m;
      if (!has(41) || !u64(41, m.id))
      {
        return refuse("OrigClOrdID", 41, "required, and must be a decimal integer below 2^64");
      }
      if (has(55) && !sym(55, m.symbol))
      {
        return refuse("Symbol", 55, "must be a decimal integer below 2^32");
      }
      if (has(44))
      {
        int64_t pr;
        if (!fix(44, pr))
        {
          return refuse("Price", 44, "not a plain decimal price");
        }
        m.newPrice = Price::fromRaw(pr);
      }
      int64_t qtyRaw;
      if (!fix(38, qtyRaw))
      {
        return refuse("OrderQty", 38, "required, and must be a plain decimal quantity");
      }
      m.newQty = Quantity::fromRaw(qtyRaw);
      if (has(1) && !u64(1, m.accountId))
      {
        return refuse("Account", 1, "must be a decimal integer below 2^64");
      }
      return InboundCommand{m};
    }
    // MassQuote (35=i): a maker's whole ladder on one symbol, one command.
    // Wire mapping: 117 QuoteID -> QuoteLadder::clientOrderId (the ladder's
    // own name, dedup slot consumed once for the whole ladder); 1 Account ->
    // QuoteLadder::accountId, required here (unlike NewOrderSingle, which
    // tolerates an absent Account and lets the session stamp it) because
    // bidIdBase/askIdBase are DERIVED from it below -- a late stamp after
    // decode would leave the ladder's own accountId field disagreeing with
    // the id block it was built from. Each QuoteEntry (299 QuoteEntryID
    // starts one) carries 55 Symbol, 132 BidPx, 133 OfferPx, 134 BidSize, 135
    // OfferSize; NoQuoteSets (296) / QuoteSetID (302) / NoQuoteEntries (295)
    // are the FIX 4.4 group counts and are not needed here -- entries are
    // read as they arrive, delimited by 299, the same way FixMdCodec reads
    // repeating MD entries.
    if (type == "i")
    {
      QuoteLadder l;
      if (!has(1) || !u64(1, l.accountId))
      {
        return refuse("Account", 1, "required, and must be a decimal integer below 2^64");
      }
      if (!quoteIdBlockInRange(l.accountId))
      {
        return refuse("Account", 1,
                      "above the range a quoting id block is derived for (below 2^24)");
      }
      if (!has(117) || !u64(117, l.clientOrderId))
      {
        return refuse("QuoteID", 117, "required, and must be a decimal integer below 2^64");
      }
      struct RawLevel
      {
        std::string symbolStr, bidPx, offerPx, bidSize, offerSize;
      };
      std::vector<RawLevel> raw;
      bool orphan = false;
      for (const auto& [tag, val] : parseOrdered(msg))
      {
        switch (tag)
        {
          case 299:
            raw.emplace_back();
            break;
          case 55:
            if (raw.empty())
            {
              orphan = true;
              break;
            }
            raw.back().symbolStr = val;
            break;
          case 132:
            if (raw.empty())
            {
              orphan = true;
              break;
            }
            raw.back().bidPx = val;
            break;
          case 133:
            if (raw.empty())
            {
              orphan = true;
              break;
            }
            raw.back().offerPx = val;
            break;
          case 134:
            if (raw.empty())
            {
              orphan = true;
              break;
            }
            raw.back().bidSize = val;
            break;
          case 135:
            if (raw.empty())
            {
              orphan = true;
              break;
            }
            raw.back().offerSize = val;
            break;
          default:
            break;
        }
        if (orphan)
        {
          return refuse("QuoteEntryID", 299, "a quote entry field arrived before any entry began");
        }
      }
      if (raw.empty())
      {
        return refuse("QuoteEntryID", 299, "a MassQuote with no quote entries quotes nothing");
      }
      if (raw.size() > kQuoteLadderLevels)
      {
        return refuse("QuoteEntryID", 299, "more quote entries than the ladder holds");
      }
      l.levels = static_cast<uint8_t>(raw.size());
      SymbolId sym0 = 0;
      Price prevBid{};
      Price prevAsk{};
      for (size_t i = 0; i < raw.size(); ++i)
      {
        const RawLevel& e = raw[i];
        SymbolId levelSym = 0;
        if (e.symbolStr.empty() || !fixfield::parseU32(e.symbolStr, levelSym))
        {
          return refuse("Symbol", 55,
                        "every quote entry needs a Symbol, a decimal integer below 2^32");
        }
        if (i == 0)
        {
          sym0 = levelSym;
        }
        else if (levelSym != sym0)
        {
          return refuse("Symbol", 55, "one MassQuote is one symbol");
        }
        int64_t bidRaw, offerRaw, bidSzRaw, offerSzRaw;
        if (e.bidPx.empty() || !decwire::parse(e.bidPx, bidRaw))
        {
          return refuse("BidPx", 132, "every quote entry needs a plain decimal bid price");
        }
        if (e.offerPx.empty() || !decwire::parse(e.offerPx, offerRaw))
        {
          return refuse("OfferPx", 133, "every quote entry needs a plain decimal offer price");
        }
        if (e.bidSize.empty() || !decwire::parse(e.bidSize, bidSzRaw))
        {
          return refuse("BidSize", 134, "every quote entry needs a plain decimal bid size");
        }
        if (e.offerSize.empty() || !decwire::parse(e.offerSize, offerSzRaw))
        {
          return refuse("OfferSize", 135, "every quote entry needs a plain decimal offer size");
        }
        const Price bid = Price::fromRaw(bidRaw);
        const Price ask = Price::fromRaw(offerRaw);
        // Levels ordered as received: bid strictly descending, ask strictly
        // ascending -- the shape a real ladder walking away from the mid
        // always has. A MassQuote that does not honour it is refused rather
        // than silently sorted: sorting would submit a ladder the sender
        // never asked for under ITS OWN QuoteID.
        if (i > 0 && bid.raw() >= prevBid.raw())
        {
          return refuse("BidPx", 132, "quote entries must step away from the mid, bid descending");
        }
        if (i > 0 && ask.raw() <= prevAsk.raw())
        {
          return refuse("OfferPx", 133, "quote entries must step away from the mid, offer ascending");
        }
        prevBid = bid;
        prevAsk = ask;
        l.level[i].bidPrice = bid;
        l.level[i].askPrice = ask;
        l.level[i].bidQty = Quantity::fromRaw(bidSzRaw);
        l.level[i].askQty = Quantity::fromRaw(offerSzRaw);
      }
      l.symbol = sym0;
      l.bidIdBase = quoteLadderIdBase(l.accountId, l.symbol);
      l.askIdBase = l.bidIdBase + kQuoteLadderLevels;
      return InboundCommand{l};
    }
    // QuoteCancel (35=Z): take the ladder down -- a QuoteLadder with zero
    // levels, addressing the SAME id block a MassQuote from this account on
    // this symbol would (see quoteLadderIdBase). 298 QuoteCancelType (4 =
    // every symbol, or by symbol) is not read: every engine shard already
    // handles exactly one symbol, so "all symbols" and "this symbol" are the
    // same operation from here.
    if (type == "Z")
    {
      QuoteLadder l;
      if (!has(1) || !u64(1, l.accountId))
      {
        return refuse("Account", 1, "required, and must be a decimal integer below 2^64");
      }
      if (!quoteIdBlockInRange(l.accountId))
      {
        return refuse("Account", 1,
                      "above the range a quoting id block is derived for (below 2^24)");
      }
      if (!has(55) || !sym(55, l.symbol))
      {
        return refuse("Symbol", 55, "required, and must be a decimal integer below 2^32");
      }
      l.levels = 0;
      l.bidIdBase = quoteLadderIdBase(l.accountId, l.symbol);
      l.askIdBase = l.bidIdBase + kQuoteLadderLevels;
      return InboundCommand{l};
    }
    return refuse("MsgType", 35, "not a message this venue's order entry decodes");
  }

  // MsgSeqNum (34) of a raw FIX message; 0 when absent (a structurally
  // incomplete message -- FIX 4.4 requires 34 on every message).
  static uint64_t msgSeqNum(const std::string& msg) { return flox::fix::msgSeqNum(msg); }

  // ---- outbound: OutboundEvent -> ExecutionReport (35=8) ----
  // Session-framed variant: injects the FIX 4.4 required header fields --
  // MsgSeqNum (34), SenderCompID (49), TargetCompID (56), SendingTime (52) --
  // that the bare encode() (embedded/test use) omits. Empty on events with no
  // exec-report mapping. A resend replay sets possDup (PossDupFlag 43=Y) and
  // origSendingTime (OrigSendingTime 122, the first transmission's 52) -- the
  // reason resends re-encode instead of replaying bytes: 43/122 change
  // BodyLength and CheckSum.
  // `text` overrides the Text (58) a reject would otherwise carry, for a
  // refusal whose reason does not say enough on its own -- a rate limit that
  // has to name the wait, a ban that has to name when it lifts. Ignored by
  // every other message: there is nothing else 58 would be telling the truth
  // about.
  static std::string encode(const OutboundEvent& ev, uint64_t seq, const std::string& senderCompId,
                            const std::string& targetCompId, const std::string& sendingTime,
                            bool possDup = false, const std::string& origSendingTime = {},
                            std::string_view text = {})
  {
    const std::string bare = encode(ev, text);
    if (bare.empty())
    {
      return bare;
    }
    // Re-frame: keep the body after the message type, prepend the session
    // header fields. The type is whatever encode() chose -- an execution report
    // for most events, OrderCancelReject for a refused cancel -- so it is read
    // off the message rather than assumed.
    const std::string anchor = std::string(1, SOH) + "35=";
    const size_t a = bare.find(anchor);
    if (a == std::string::npos)
    {
      return {};
    }
    const size_t tend = bare.find(SOH, a + anchor.size());
    if (tend == std::string::npos)
    {
      return {};
    }
    const std::string marker = bare.substr(a + 1, tend - a);
    const size_t bodyStart = tend + 1;
    const size_t csum = bare.rfind(std::string(1, SOH) + "10=");
    const std::string tail =
        bare.substr(bodyStart, (csum == std::string::npos ? bare.size() : csum + 1) - bodyStart);
    std::string b = marker;
    flox::fix::appendHeader(b, seq, senderCompId, targetCompId, sendingTime, possDup,
                            origSendingTime);
    b += tail;
    return frame(b);
  }

  // Session/admin message (Logon 35=A, Heartbeat 35=0, TestRequest 35=1,
  // ResendRequest 35=2, SequenceReset 35=4, Logout 35=5) with the full FIX 4.4
  // header and body `fields`, framed with BodyLength and CheckSum.
  static std::string encodeAdmin(const std::string& msgType, uint64_t seq,
                                 const std::string& senderCompId, const std::string& targetCompId,
                                 const std::string& sendingTime,
                                 const std::vector<std::pair<int, std::string>>& fields = {},
                                 bool possDup = false)
  {
    return flox::fix::encodeAdmin(msgType, seq, senderCompId, targetCompId, sendingTime, fields,
                                  possDup);
  }

  static std::string encode(const OutboundEvent& ev, std::string_view text = {})
  {
    std::string b;  // body after 35
    auto add = [&](int tag, const std::string& val)
    { b += std::to_string(tag) + "=" + val + SOH; };
    auto px = [](Price p)
    {
      std::string s;
      decwire::append(s, p.raw());
      return s;
    };
    auto qn = [](Quantity q)
    {
      std::string s;
      decwire::append(s, q.raw());
      return s;
    };

    // OrderCancelReject (35=9). FIX 4.4 answers a refused 35=F / 35=G with
    // this, not with an execution report: an exec report describes the state of
    // an ORDER, and a refused cancel changed no order state at all. Handled
    // before the exec-report body below because it is a different message, not
    // a variant of one.
    if (const auto* cr = std::get_if<CancelRejected>(&ev))
    {
      add(35, "9");
      add(37, std::to_string(cr->id));
      add(11, std::to_string(cr->id));
      add(41, std::to_string(cr->id));  // OrigClOrdID: the order being acted on
      // OrdStatus: an order we never had is Rejected; anything else is still
      // working, and 0 is the most this venue can substantiate without
      // carrying the resting state onto the event.
      const bool unknown = cr->reason == RejectReason::UnknownOrder;
      add(39, unknown ? "8" : "0");
      add(434, cr->wasReplace ? "2" : "1");  // CxlRejResponseTo
      add(102, unknown ? "1" : "99");        // CxlRejReason: Unknown order / Other
      add(58, text.empty() ? std::string(toString(cr->reason)) : std::string(text));
      return frame(b);
    }

    // A Quote/QuoteLadder admission refusal (AdmissionDeny::DenyQuote)
    // is QuoteStatusReport-shaped, not exec-report-shaped, the same reasoning
    // as CancelRejected above: FIX has no honest ExecType for "your ladder
    // never reached the book", and this venue's own MassQuote/QuoteCancel
    // path already answers every accepted request through QuoteStatusReport
    // (see fix_session.h) -- an engine-side refusal of a ladder that made it
    // past THAT admission answers through the same shape rather than
    // switching to an exec report partway through one conversation. Safe to
    // key on the reason alone: today QuoteNotPermitted fires only from
    // applyQuote (Quote and QuoteLadder), and the plain Quote struct has no
    // FIX mapping of its own, so on the wire this can only be a QuoteLadder.
    if (const auto* qj = std::get_if<OrderRejected>(&ev); qj != nullptr &&
                                                          qj->reason == RejectReason::QuoteNotPermitted)
    {
      std::string qb;
      auto qadd = [&](int tag, const std::string& val)
      { qb += std::to_string(tag) + "=" + val + SOH; };
      qadd(35, "AI");
      if (qj->clientOrderId != 0)
      {
        qadd(117, std::to_string(qj->clientOrderId));
      }
      qadd(55, std::to_string(qj->symbol));
      qadd(297, "5");  // QuoteStatus Rejected
      qadd(58, text.empty() ? std::string(toString(qj->reason)) : std::string(text));
      return frame(qb);
    }

    add(35, "8");  // ExecutionReport
    // ClOrdID (11) on every report that describes an order. FIX 4.4 requires
    // it, and the reason is practical: a submitter reconciles against the
    // identifier it chose, and on a reject there may never have been another
    // one to reconcile against. 37 stays the venue's own identifier; the two
    // are not the same field and this used to put 37's value in both.
    auto clOrd = [&](uint64_t v)
    {
      if (v != 0)
      {
        add(11, std::to_string(v));
      }
    };
    if (const auto* a = std::get_if<OrderAccepted>(&ev))
    {
      add(37, std::to_string(a->id));
      clOrd(a->clientOrderId);
      add(55, std::to_string(a->symbol));
      add(54, a->side == Side::SELL ? "2" : "1");
      add(150, "0");  // ExecType New
      add(39, "0");   // OrdStatus New
      add(151, qn(a->leavesQty));
      add(44, px(a->price));
      add(14, qn(a->cumQty));  // what this order filled of itself before this accept
    }
    else if (const auto* x = std::get_if<OrderExecuted>(&ev))
    {
      add(37, std::to_string(x->id));
      clOrd(x->clientOrderId);
      add(55, std::to_string(x->symbol));
      add(150, "F");                     // ExecType Trade
      add(39, x->complete ? "2" : "1");  // Filled / Partially filled
      add(32, qn(x->lastQty));           // LastQty
      add(31, px(x->lastPx));            // LastPx -- price of this fill
      add(6, px(x->lastPx));             // AvgPx (single-fill report)
      add(151, qn(x->leavesQty));        // LeavesQty
      add(14, qn(x->cumQty));            // CumQty -- total filled as of this fill
    }
    else if (const auto* c = std::get_if<OrderCanceled>(&ev))
    {
      add(37, std::to_string(c->id));
      clOrd(c->clientOrderId);
      add(150, "4");  // Canceled
      add(39, "4");
      // The residual this cancel actually killed, and what the order
      // filled before it. Without 151 a counterparty that reads LeavesQty
      // off terminal reports (routine for an IOC/FOK residual) has no way to
      // tell "filled completely" from "the remainder was silently canceled".
      add(151, qn(c->leavesQty));  // LeavesQty
      add(14, qn(c->cumQty));      // CumQty
    }
    else if (const auto* j = std::get_if<OrderRejected>(&ev))
    {
      add(37, std::to_string(j->id));
      clOrd(j->clientOrderId);
      add(150, "8");  // Rejected
      add(39, "8");
      add(58, text.empty() ? std::string(toString(j->reason)) : std::string(text));
      // A rejected order is never left resting (151 is always 0), but
      // CumQty is not always 0 -- a fill-time risk re-check or an STP block
      // can reject an order's residual after matcher_.cross() already
      // printed part of it (see OrderRejected::cumQty).
      add(151, qn(Quantity{}));
      add(14, qn(j->cumQty));
    }
    else if (const auto* m = std::get_if<OrderModified>(&ev))
    {
      add(37, std::to_string(m->id));
      clOrd(m->clientOrderId);
      add(150, "5");  // Replaced
      add(39, "5");
      add(151, qn(m->leavesQty));
      add(44, px(m->price));
      add(14, qn(m->cumQty));  // running total, unaffected by a reprice/resize
    }
    else if (const auto* fh = std::get_if<FillHeld>(&ev))
    {
      // Last-look hold: FIX has no honest ExecType for "fill pending the
      // maker's confirmation", so this uses the documented custom value
      // ExecType=U plus custom tags 20001 (heldId) / 20002 (makerId); see
      // docs/venue/matching.md. The order is still working (39=0); 32/31
      // carry the held size and price.
      add(37, std::to_string(fh->takerId));
      clOrd(fh->clientOrderId);
      add(55, std::to_string(fh->symbol));
      add(150, "U");  // custom ExecType: fill held pending last look
      add(39, "0");   // OrdStatus New/working -- nothing has executed yet
      add(32, qn(fh->qty));
      add(31, px(fh->price));
      add(20001, std::to_string(fh->heldId));
      add(20002, std::to_string(fh->makerId));
      add(14, qn(fh->cumQty));  // taker's confirmed total as of hold creation
    }
    else if (const auto* fr = std::get_if<FillRejected>(&ev))
    {
      // Held fill rejected/timed out: the pending fill is busted, which maps
      // honestly onto ExecType=H (Trade Cancel). Same custom tags identify the
      // held fill being cancelled.
      add(37, std::to_string(fr->takerId));
      clOrd(fr->clientOrderId);
      add(55, std::to_string(fr->symbol));
      add(150, "H");  // ExecType Trade Cancel: the held fill will not stand
      add(39, "0");
      add(32, qn(fr->qty));
      add(31, px(fr->price));
      add(20001, std::to_string(fr->heldId));
      add(20002, std::to_string(fr->makerId));
      add(58, "LastLookRejected");
      add(14, qn(fr->cumQty));  // same value FillHeld reported when this hold opened
    }
    else
    {
      // Trade/Triggered are market-data, not exec reports. PositionAdjusted is
      // deliberately here too: FIX 4.4 carries a position change in a Position
      // Report (AP), a different message category with its own request flow,
      // and squeezing a correction into an execution report would tell the
      // client a fill happened when none did. A FIX client learns of a
      // correction through the position report or out of band; SBE clients get
      // the event itself.
      static_assert(std::variant_size_v<OutboundEvent> == 17,
                    "new OutboundEvent alternative: encode it above, or decide here -- with a "
                    "reason -- that FIX has no mapping for it");
      return {};
    }

    return frame(b);
  }

  // Prepend 8/9, append 10 with correct BodyLength and CheckSum.
  static std::string frame(const std::string& body) { return flox::fix::frame(body); }
};

// FIX session-layer state: monotonic outbound MsgSeqNum, inbound MsgSeqNum
// validation, CompIDs from the gateway configuration and SendingTime.
//
// Scope: sequencing/framing building block for embedded and test use. The
// full session layer -- Logon negotiation, Heartbeat/TestRequest liveness,
// ResendRequest 35=2 / SequenceReset-GapFill 35=4, PossDup replay -- lives in
// fix_session.h (FixSessionHost / FixConnection), wired into the gateways via
// setFixSession. See docs/venue/perimeter.md.
class FixSession
{
 public:
  FixSession(std::string senderCompId, std::string targetCompId)
      : sender_(std::move(senderCompId)), target_(std::move(targetCompId))
  {
  }

  // Encode an exec report as the next sequenced session message. Empty string
  // = the event has no FIX mapping (the seq is NOT consumed).
  std::string encode(const OutboundEvent& ev, int64_t wallClockNs)
  {
    const std::string msg =
        FixCodec::encode(ev, nextOut_, sender_, target_, sendingTime(wallClockNs));
    if (!msg.empty())
    {
      ++nextOut_;
    }
    return msg;
  }

  uint64_t nextOutboundSeq() const noexcept { return nextOut_; }

  enum class InSeq : uint8_t
  {
    Ok,         // expected seq (or first message)
    Gap,        // seq jumped forward: messages lost -> reject the session
    Duplicate,  // seq at or below the last accepted one
    Missing,    // no tag 34: structurally invalid FIX 4.4
  };

  // Validate the inbound MsgSeqNum. Ok advances the expectation; anything else
  // leaves it unchanged so the caller can terminate the session.
  InSeq acceptInbound(const std::string& msg)
  {
    const uint64_t seq = FixCodec::msgSeqNum(msg);
    if (seq == 0)
    {
      return InSeq::Missing;
    }
    if (seq == expectedIn_)
    {
      ++expectedIn_;
      return InSeq::Ok;
    }
    return seq > expectedIn_ ? InSeq::Gap : InSeq::Duplicate;
  }

  uint64_t expectedInboundSeq() const noexcept { return expectedIn_; }

  // UTCTimestamp for tag 52: YYYYMMDD-HH:MM:SS.sss from wall-clock ns.
  static std::string sendingTime(int64_t wallClockNs)
  {
    return flox::fix::sendingTime(wallClockNs);
  }

 private:
  std::string sender_;
  std::string target_;
  uint64_t nextOut_{1};
  uint64_t expectedIn_{1};
};

}  // namespace flox::venue
