/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/messages.h"
#include "flox/log/log.h"
#include "flox/util/crypto.h"

#include "flox/execution/rate_limit_policy.h"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace flox::venue
{

inline flox::RateLimitPolicy::ActionKind actionOf(const InboundCommand& c) noexcept
{
  if (std::get_if<NewOrder>(&c))
  {
    return flox::RateLimitPolicy::ActionKind::Submit;
  }
  if (std::get_if<CancelOrder>(&c) || std::get_if<MassCancel>(&c))
  {
    return flox::RateLimitPolicy::ActionKind::Cancel;
  }
  if (std::get_if<Quote>(&c))
  {
    return flox::RateLimitPolicy::ActionKind::Submit;
  }
  if (std::get_if<LastLookDecision>(&c))
  {
    return flox::RateLimitPolicy::ActionKind::QueryAccount;
  }
  return flox::RateLimitPolicy::ActionKind::Replace;
}

// How fast a client session may send -- a SETTING of the session, not one
// venue's published profile.
//
// Every session used to be handed RateLimitPolicy::binance_um_futures(): 50
// order actions per 10 seconds, then a three-minute ban after three refusals.
// That is one exchange's retail tier wired in as the only answer available.
// A client bridge that fans one price move out into a burst of amendments is
// not abusing anything, and against that profile its ordinary traffic is a
// disconnect followed by three minutes of silence.
//
// `off()` is a real answer, not a hole. A venue behind a trusted transport, or
// one whose admission is budgeted somewhere else, gains nothing from a limiter
// and loses real flow to one it did not choose. An off policy has no buckets
// and no ban, so tryConsume admits everything.
struct SessionRateLimit
{
  bool enabled{true};
  uint32_t actionsPerWindow{50};
  int64_t windowNs{10'000'000'000LL};
  uint32_t banAfterRejects{3};  // 0 disables the ban on its own
  int64_t banNs{180'000'000'000LL};

  static SessionRateLimit off() noexcept
  {
    SessionRateLimit l;
    l.enabled = false;
    return l;
  }

  flox::RateLimitPolicy policy() const
  {
    flox::RateLimitPolicy p;
    if (!enabled)
    {
      return p;
    }
    p.addBucket("session", windowNs, actionsPerWindow);
    p.setBan(banAfterRejects, banNs);
    return p;
  }
};

// A session put in a rate-limit ban. Announced to the client in the refusal
// itself and, separately, to whoever runs the venue: a ban is the venue
// refusing a named client for minutes at a time, and that is an operational
// event, not a private matter between the limiter and one connection.
struct SessionBan
{
  std::string session;  // the session's own name; empty falls back to the account
  uint64_t account{0};
  int64_t untilNs{0};      // on the clock handle() was called with
  int64_t remainingNs{0};  // how long the client must wait
};

inline void logSessionBan(const SessionBan& b)
{
  FLOX_LOG_WARN("venue.session " << (b.session.empty() ? std::to_string(b.account) : b.session)
                                 << ": rate-limit ban, " << (b.remainingNs / 1'000'000)
                                 << " ms to lift");
}

// The words a rate-limit refusal travels with: what tripped, and how long the
// client has to wait before trying again. FIX carries it in Text (58).
//
// "Rate limited" alone leaves the client choosing between retrying in a
// millisecond and retrying in three minutes, and the usual choice -- retry at
// once -- is the one that walks into the ban. A ban that says nothing is worse
// still: 35=8 with a bare reason reads exactly like one refused order, so the
// client keeps sending into a session that will refuse everything for the next
// three minutes.
inline std::string rateLimitText(bool banned, int64_t retryAfterNs)
{
  const int64_t ms = (retryAfterNs + 999'999) / 1'000'000;  // round up: never say 0
  return std::string(banned ? "RateLimitBanned" : "RateLimited") + ": retry in " +
         std::to_string(ms) + " ms";
}

enum class SessionReject : uint8_t
{
  None = 0,
  Unauthenticated,
  RateLimited,
  DecodeError,
};

// Wire form of a session-level rejection: reuse the exec-report reject (the
// client already understands OrderRejected) with a session-scoped reason.
inline RejectReason toRejectReason(SessionReject r) noexcept
{
  switch (r)
  {
    case SessionReject::Unauthenticated:
      return RejectReason::Unauthenticated;
    case SessionReject::RateLimited:
      return RejectReason::RateLimited;
    case SessionReject::DecodeError:
      return RejectReason::MalformedMessage;
    case SessionReject::None:
      break;
  }
  return RejectReason::None;
}

inline const char* toString(SessionReject r) noexcept
{
  switch (r)
  {
    case SessionReject::None:
      return "None";
    case SessionReject::Unauthenticated:
      return "Unauthenticated";
    case SessionReject::RateLimited:
      return "RateLimited";
    case SessionReject::DecodeError:
      return "DecodeError";
  }
  return "?";
}

// Identifying fields of a command refused before it reached the engine (a
// session-level admission reject: rate limit, an unauthenticated frame, a
// frame that did not decode). The engine's own OrderRejected always carries
// these off the order it refused; a session reject answered with id 0 /
// symbol 0 / no clOrdId (silence dressed up as an exec report) because
// handle() threw the frame away the moment admission failed -- so the fields
// it should have echoed were briefly in hand and never carried out. This is
// that hand-off.
struct RejectEcho
{
  OrderId id{0};
  SymbolId symbol{0};
  uint64_t clientOrderId{0};
};

// The subset of InboundCommand a client submits that a rate limit can catch
// (see actionOf) and that name an order worth echoing back. Anything else
// (money moves, admin/market commands) answers with a zeroed echo, same as
// before this existed.
inline RejectEcho rejectEchoOf(const InboundCommand& c) noexcept
{
  if (const auto* n = std::get_if<NewOrder>(&c))
  {
    return RejectEcho{n->id, n->symbol, n->clientOrderId};
  }
  if (const auto* x = std::get_if<CancelOrder>(&c))
  {
    return RejectEcho{x->id, x->symbol, 0};
  }
  if (const auto* m = std::get_if<ModifyOrder>(&c))
  {
    return RejectEcho{m->id, m->symbol, 0};
  }
  if (const auto* q = std::get_if<Quote>(&c))
  {
    // A quote names two ids; bidId is the one OrderRejected already uses
    // elsewhere for a whole-quote refusal (see MatchingEngine::onQuote).
    return RejectEcho{q->bidId, q->symbol, q->clientOrderId};
  }
  return RejectEcho{};
}

// ClOrdID (FIX tag 11) read straight out of a frame that did NOT decode.
//
// A refusal with no identity in it is worse than useless to the client: it has
// nothing to match the refusal against, so it waits out its timeout and
// resends -- and the resend is refused again, this time as a duplicate
// ClOrdID. One refusal becomes two and the second one is unexplainable. A
// tag=value frame stays readable when it is not decodable (an unknown
// MsgType, a field the codec refuses, a truncated tail), so the identifier
// the client chose is usually still right there in the bytes.
//
// Deliberately narrow: only a numeric ClOrdID at a field boundary, because
// the echo is a uint64 and a wrong identifier is worse than none -- it points
// the client at an order it did not send.
inline uint64_t clientOrderIdFromRaw(const uint8_t* p, size_t n) noexcept
{
  constexpr uint8_t kSoh = 0x01;
  for (size_t i = 0; i + 3 <= n; ++i)
  {
    if (p[i] != '1' || p[i + 1] != '1' || p[i + 2] != '=')
    {
      continue;
    }
    if (i != 0 && p[i - 1] != kSoh)
    {
      continue;  // "411=..." is tag 411, not tag 11
    }
    uint64_t v = 0;
    size_t digits = 0;
    for (size_t j = i + 3; j < n && p[j] != kSoh; ++j)
    {
      if (p[j] < '0' || p[j] > '9' || digits == 19)
      {
        return 0;  // not a number, or too long to be one: echo nothing
      }
      v = v * 10 + static_cast<uint64_t>(p[j] - '0');
      ++digits;
    }
    return digits == 0 ? 0 : v;
  }
  return 0;
}

// An account field as the commands spell it: `accountId` on the order-flow and
// balance commands, `account` on the per-account configuration ones.
template <class T>
concept HasAccountId = requires(T t) {
  { t.accountId } -> std::same_as<uint64_t&>;
};
template <class T>
concept HasAccount = requires(T t) {
  { t.account } -> std::same_as<uint64_t&>;
};
template <class T>
concept NamesAnAccount = requires(T t) { t.accountId; } || requires(T t) { t.account; };

class GatewaySession
{
 public:
  using Decoder = std::function<std::optional<InboundCommand>(const uint8_t*, size_t)>;

  using BanObserver = std::function<void(const SessionBan&)>;

  GatewaySession(uint64_t account, Decoder decode,
                 flox::RateLimitPolicy limits = flox::RateLimitPolicy::binance_um_futures(),
                 std::string secret = {})
      : accounts_{account},
        decode_(std::move(decode)),
        limits_(std::move(limits)),
        secret_(std::move(secret))
  {
  }

  // A session that speaks for SEVERAL accounts. The ordinary shape of a client
  // bridge: one connection carrying the flow of many customers, each with its
  // own account at the venue. The first account is the session's own identity
  // (the one an unnamed command is stamped with, the one the outbound stream
  // is keyed by); the rest are accounts it is entitled to act for.
  GatewaySession(std::vector<uint64_t> accounts, Decoder decode,
                 flox::RateLimitPolicy limits = flox::RateLimitPolicy::binance_um_futures(),
                 std::string secret = {})
      : accounts_(std::move(accounts)),
        decode_(std::move(decode)),
        limits_(std::move(limits)),
        secret_(std::move(secret))
  {
    if (accounts_.empty())
    {
      accounts_.push_back(0);
    }
  }

  // The name this session is known by in the venue's own log (a ban says who
  // was banned). Empty falls back to the account id.
  void setName(std::string n) { name_ = std::move(n); }
  const std::string& name() const noexcept { return name_; }

  // Where a ban is announced on the venue side. Defaults to the log; a
  // deployment that routes operational events somewhere else replaces it.
  void setBanObserver(BanObserver o) { banObserver_ = std::move(o); }

  void authenticate(bool ok) noexcept { authed_ = ok; }
  bool authenticated() const noexcept { return authed_; }
  // The session's own identity: the first of its accounts.
  uint64_t account() const noexcept { return accounts_.front(); }
  // Every account this session may act for, identity first.
  const std::vector<uint64_t>& accounts() const noexcept { return accounts_; }
  bool speaksFor(uint64_t a) const noexcept
  {
    return std::find(accounts_.begin(), accounts_.end(), a) != accounts_.end();
  }

  // Per-SESSION cancel-on-disconnect. The gateway-wide atomic is only the
  // default seeded into each new session; this flag is what the connection
  // actually honours. Wire-level negotiation (a logon COD field / per-account
  // config) is future work -- today deployments set it at accept time.
  void setCancelOnDisconnect(bool on) noexcept { cancelOnDisconnect_ = on; }
  bool cancelOnDisconnect() const noexcept { return cancelOnDisconnect_; }
  // Bind this session to an authenticated account (e.g. after logon resolves
  // the API key to an account). Once bound to a non-zero account, handle()
  // stamps that account onto a command that names none, and REFUSES one that
  // names an account the session does not speak for -- see the authorization
  // block in handle().
  void bindAccount(uint64_t a) { accounts_.assign(1, a); }
  void bindAccounts(std::vector<uint64_t> accounts)
  {
    accounts_ = std::move(accounts);
    if (accounts_.empty())
    {
      accounts_.push_back(0);
    }
  }

  // API-key HMAC logon (crypto-exchange style): the client signs
  // "apiKey:timestamp" with the shared secret. Verifies signature (constant
  // time) and timestamp skew, then marks the session authenticated.
  bool logon(const std::string& apiKey, uint64_t ts, const std::string& sigHex, uint64_t nowTs,
             uint64_t maxSkewSec = 5)
  {
    if (secret_.empty())
    {
      return false;
    }
    const uint64_t skew = (nowTs > ts) ? (nowTs - ts) : (ts - nowTs);
    if (skew > maxSkewSec)
    {
      return false;
    }
    const std::string payload = apiKey + ":" + std::to_string(ts);
    const auto mac = crypto::hmacSha256(reinterpret_cast<const uint8_t*>(secret_.data()),
                                        secret_.size(),
                                        reinterpret_cast<const uint8_t*>(payload.data()),
                                        payload.size());
    const std::string expect = crypto::toHex(mac.data(), mac.size());
    if (expect.size() != sigHex.size())
    {
      return false;
    }
    unsigned diff = 0;
    for (size_t i = 0; i < expect.size(); ++i)
    {
      diff |= static_cast<unsigned>(expect[i] ^ sigHex[i]);
    }
    if (diff == 0)
    {
      authed_ = true;
      return true;
    }
    return false;
  }

  // Decode + admission-control one inbound frame. Returns the command to
  // submit, or nullopt with `out` set to the rejection reason. `echo`, when
  // given, names the frame that was refused, for EVERY reason -- the client
  // has to be able to match a refusal to the order it sent, and which of the
  // venue's own admission rules tripped is not something it can use to do
  // that. Where the identity comes from:
  //
  //   frame decoded  -> the command's own id / symbol / clientOrderId
  //   frame did not  -> ClOrdID scraped out of the raw bytes if one is
  //                     legible there (clientOrderIdFromRaw), zeros if not
  //
  // `text`, when given, carries what the reason alone cannot say -- for a
  // rate limit, how long the client has to wait, and whether it is now in a
  // ban rather than one refusal. Empty for a reason that speaks for itself.
  //
  // Both are optional and defaulted so every existing caller that only wants
  // the reason keeps compiling unchanged.
  std::optional<InboundCommand> handle(const uint8_t* p, size_t n, int64_t nowNs,
                                       SessionReject& out, RejectEcho* echo = nullptr,
                                       std::string* text = nullptr)
  {
    out = SessionReject::None;
    if (echo != nullptr)
    {
      *echo = RejectEcho{};
    }
    if (text != nullptr)
    {
      text->clear();
    }
    if (!authed_)
    {
      out = SessionReject::Unauthenticated;
      // A frame sent before logon is still a frame about an order. It is
      // decoded here only to name it in the refusal -- nothing is stamped,
      // charged or submitted.
      nameFrame(echo, p, n);
      return std::nullopt;
    }
    auto cmd = decode_(p, n);
    if (!cmd)
    {
      out = SessionReject::DecodeError;
      if (echo != nullptr)
      {
        echo->clientOrderId = clientOrderIdFromRaw(p, n);
      }
      return std::nullopt;
    }
    if (echo != nullptr)
    {
      *echo = rejectEchoOf(*cmd);
    }
    // Authorization: a bound session may act only for the accounts it speaks
    // for. A command that names none is stamped with the session's identity; a
    // command that names one of the session's accounts keeps it; a command
    // that names anything else is refused, so a client can never place orders
    // on, spend the collateral of, or mass-cancel an account it was not given.
    // account() == 0 is the "unbound / trusted-transport" sentinel and passes
    // through untouched.
    //
    // The refusal replaces a silent overwrite. Forcing the session's own
    // account onto a foreign id was safe with exactly one account and became
    // meaningless with several -- there is no single id to force -- and it was
    // never honest even with one: a client that named the wrong account had
    // its order placed on a different one and was told nothing.
    if (account() != 0)
    {
      const uint64_t named = accountOf(*cmd);
      if (named == 0)
      {
        stampAccount(*cmd, account());
      }
      else if (!speaksFor(named))
      {
        out = SessionReject::Unauthenticated;
        return std::nullopt;
      }
    }
    if (!limits_.tryConsume(actionOf(*cmd), nowNs))
    {
      out = SessionReject::RateLimited;
      // tryConsume arms the ban on the refusal that reaches the threshold, so
      // the ban is known here, on the very frame that caused it -- which is
      // the only frame the client will get an answer to for a while.
      const bool banned = limits_.banUntilNs() > nowNs;
      const int64_t retryNs = limits_.retryAfterNs(actionOf(*cmd), nowNs);
      if (text != nullptr)
      {
        *text = rateLimitText(banned, retryNs);
      }
      if (banned && limits_.banUntilNs() != announcedBanUntilNs_)
      {
        announcedBanUntilNs_ = limits_.banUntilNs();
        if (banObserver_)
        {
          banObserver_(SessionBan{name_, account(), announcedBanUntilNs_, retryNs});
        }
      }
      return std::nullopt;
    }
    return cmd;
  }

 private:
  // Identity to answer a refusal with when the frame was never admitted far
  // enough to produce a command: decode it for the echo if it decodes, read
  // the ClOrdID out of the bytes if it does not.
  void nameFrame(RejectEcho* echo, const uint8_t* p, size_t n) const
  {
    if (echo == nullptr)
    {
      return;
    }
    if (auto cmd = decode_(p, n))
    {
      *echo = rejectEchoOf(*cmd);
      return;
    }
    echo->clientOrderId = clientOrderIdFromRaw(p, n);
  }

  // The account a command names, or 0 when it names none (admin and market
  // commands carry no account). Reads the same field stampAccount writes, by
  // the same walk over the variant, so the two cannot drift apart.
  static uint64_t accountOf(const InboundCommand& c) noexcept
  {
    return std::visit(
        [](const auto& m) -> uint64_t
        {
          using Cmd = std::remove_cvref_t<decltype(m)>;
          if constexpr (HasAccountId<Cmd>)
          {
            return m.accountId;
          }
          else if constexpr (HasAccount<Cmd>)
          {
            return m.account;
          }
          else
          {
            static_assert(!NamesAnAccount<Cmd>, "account field of an unexpected type");
            return 0;
          }
        },
        c);
  }

  // Force `a` onto every account-bearing command so the session can only ever
  // act as its own authenticated account. Admin and market commands (SetMark,
  // ApplyFunding) carry no account and are left untouched.
  //
  // Written as a visit over the whole variant rather than a list of branches.
  // The list version covered the six order-flow commands and missed the five
  // that move money, close positions and set per-account entitlements, which is
  // the failure mode a list has: it is correct on the day it is written and
  // silently incomplete from the next command onwards. Here a new alternative
  // with an account field is stamped the moment it is added.
  static void stampAccount(InboundCommand& c, uint64_t a) noexcept
  {
    std::visit(
        [a](auto& m)
        {
          using Cmd = std::remove_reference_t<decltype(m)>;
          if constexpr (HasAccountId<Cmd>)
          {
            m.accountId = a;
          }
          else if constexpr (HasAccount<Cmd>)
          {
            m.account = a;
          }
          else
          {
            static_assert(!NamesAnAccount<Cmd>, "account field of an unexpected type");
          }
        },
        c);
  }

  std::vector<uint64_t> accounts_;  // never empty; accounts_.front() is the identity
  Decoder decode_;
  flox::RateLimitPolicy limits_;
  std::string secret_;
  std::string name_;
  BanObserver banObserver_{&logSessionBan};
  int64_t announcedBanUntilNs_{0};  // announce each ban once, not once per refused frame
  bool authed_{false};
  bool cancelOnDisconnect_{false};
};

}  // namespace flox::venue
