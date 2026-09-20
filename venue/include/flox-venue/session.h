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
#include "flox/util/crypto.h"

#include "flox/execution/rate_limit_policy.h"

#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

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
// session-level admission reject: rate limit today). The engine's own
// OrderRejected always carries these off the order it refused; a router
// reject answered with id 0 / symbol 0 / no clOrdId (silence dressed up as an
// exec report) because handle() decoded the command, stamped it, then threw
// it away the moment admission failed -- so the fields it should have echoed
// were briefly in hand and never carried out. This is that hand-off.
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

  GatewaySession(uint64_t account, Decoder decode,
                 flox::RateLimitPolicy limits = flox::RateLimitPolicy::binance_um_futures(),
                 std::string secret = {})
      : account_(account), decode_(std::move(decode)), limits_(std::move(limits)), secret_(std::move(secret))
  {
  }

  void authenticate(bool ok) noexcept { authed_ = ok; }
  bool authenticated() const noexcept { return authed_; }
  uint64_t account() const noexcept { return account_; }

  // Per-SESSION cancel-on-disconnect. The gateway-wide atomic is only the
  // default seeded into each new session; this flag is what the connection
  // actually honours. Wire-level negotiation (a logon COD field / per-account
  // config) is future work -- today deployments set it at accept time.
  void setCancelOnDisconnect(bool on) noexcept { cancelOnDisconnect_ = on; }
  bool cancelOnDisconnect() const noexcept { return cancelOnDisconnect_; }
  // Bind this session to an authenticated account (e.g. after logon resolves the
  // API key to an account). Once bound to a non-zero account, handle() forces
  // that account onto every command -- see stampAccount.
  void bindAccount(uint64_t a) noexcept { account_ = a; }

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
  // given, is filled with the refused command's own id/symbol/clientOrderId
  // for a reject the caller answers on the wire (RateLimited: the frame DID
  // decode, so there is a real order to echo) and left zeroed for one where
  // there never was a command to take them from (Unauthenticated,
  // DecodeError). Optional and defaulted so every existing caller that only
  // wants the reason keeps compiling unchanged.
  std::optional<InboundCommand> handle(const uint8_t* p, size_t n, int64_t nowNs,
                                       SessionReject& out, RejectEcho* echo = nullptr)
  {
    out = SessionReject::None;
    if (echo != nullptr)
    {
      *echo = RejectEcho{};
    }
    if (!authed_)
    {
      out = SessionReject::Unauthenticated;
      return std::nullopt;
    }
    auto cmd = decode_(p, n);
    if (!cmd)
    {
      out = SessionReject::DecodeError;
      return std::nullopt;
    }
    // Authorization: a session bound to a real account may only act as that
    // account. Overwrite the client-supplied accountId so a client can never
    // place orders on, spend the collateral of, or mass-cancel another account
    // by writing a different id into the payload. account_ == 0 is the "unbound
    // / trusted-transport" sentinel (current gateway stubs) and passes through.
    if (account_ != 0)
    {
      stampAccount(*cmd, account_);
    }
    if (echo != nullptr)
    {
      *echo = rejectEchoOf(*cmd);
    }
    if (!limits_.tryConsume(actionOf(*cmd), nowNs))
    {
      out = SessionReject::RateLimited;
      return std::nullopt;
    }
    return cmd;
  }

 private:
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

  uint64_t account_;
  Decoder decode_;
  flox::RateLimitPolicy limits_;
  std::string secret_;
  bool authed_{false};
  bool cancelOnDisconnect_{false};
};

}  // namespace flox::venue
