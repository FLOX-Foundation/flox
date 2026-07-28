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

#include "flox/backtest/rate_limit_policy.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

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

  // Decode + admission-control one inbound frame. Returns the command to submit,
  // or nullopt with `out` set to the rejection reason.
  std::optional<InboundCommand> handle(const uint8_t* p, size_t n, int64_t nowNs,
                                       SessionReject& out)
  {
    out = SessionReject::None;
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
    if (!limits_.tryConsume(actionOf(*cmd), nowNs))
    {
      out = SessionReject::RateLimited;
      return std::nullopt;
    }
    return cmd;
  }

 private:
  // Force `a` onto every account-bearing command so the session can only ever
  // act as its own authenticated account. Admin/market commands (SetMark,
  // ApplyFunding) carry no account and are left untouched here.
  static void stampAccount(InboundCommand& c, uint64_t a) noexcept
  {
    if (auto* n = std::get_if<NewOrder>(&c))
    {
      n->accountId = a;
    }
    else if (auto* x = std::get_if<CancelOrder>(&c))
    {
      x->accountId = a;
    }
    else if (auto* m = std::get_if<ModifyOrder>(&c))
    {
      m->accountId = a;
    }
    else if (auto* mc = std::get_if<MassCancel>(&c))
    {
      mc->accountId = a;
    }
    else if (auto* q = std::get_if<Quote>(&c))
    {
      q->accountId = a;
    }
    else if (auto* ll = std::get_if<LastLookDecision>(&c))
    {
      ll->accountId = a;
    }
  }

  uint64_t account_;
  Decoder decode_;
  flox::RateLimitPolicy limits_;
  std::string secret_;
  bool authed_{false};
};

}  // namespace flox::venue
