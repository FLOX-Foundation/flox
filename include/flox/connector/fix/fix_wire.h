/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * FIX 4.4 wire primitives: tag=value/SOH field parsing, the BodyLength (9) and
 * CheckSum (10) framing, the session header every message carries, and the
 * UTCTimestamp format of SendingTime (52).
 *
 * Nothing here knows about orders, and nothing here holds session state. That
 * is the point: an acceptor and an initiator disagree about almost everything
 * at the session layer and must agree exactly at this one, down to the byte.
 * The venue's FixCodec (venue/include/flox-venue/fix_codec.h) and the FIX
 * initiator in this directory both frame through these functions, so a
 * checksum or a header-field order can only change for both at once.
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox::fix
{

inline constexpr char kSoh = '\x01';

// The FIX 4.4 begin string, the first field of every message.
inline constexpr std::string_view kBeginString = "FIX.4.4";

using Fields = std::unordered_map<int, std::string>;

// tag=value fields of a raw FIX message (last occurrence wins on repeats).
inline Fields parseFields(const std::string& msg)
{
  Fields f;
  size_t i = 0;
  while (i < msg.size())
  {
    const size_t eq = msg.find('=', i);
    if (eq == std::string::npos)
    {
      break;
    }
    size_t soh = msg.find(kSoh, eq + 1);
    if (soh == std::string::npos)
    {
      soh = msg.size();
    }
    const int tag = std::atoi(msg.substr(i, eq - i).c_str());
    f[tag] = msg.substr(eq + 1, soh - eq - 1);
    i = soh + 1;
  }
  return f;
}

// FIX integrity: if a CheckSum (tag 10) is present it MUST be correct -- the
// sum of every byte up to and including the SOH before "10=", mod 256.
// Lenient when absent, for internal and test callers that append none.
inline bool checksumValid(const std::string& msg)
{
  const std::string marker = std::string(1, kSoh) + "10=";
  const size_t p = msg.rfind(marker);
  if (p == std::string::npos)
  {
    return true;
  }
  unsigned sum = 0;
  for (size_t k = 0; k <= p; ++k)
  {
    sum += static_cast<unsigned char>(msg[k]);
  }
  return (sum % 256) == static_cast<unsigned>(std::atoi(msg.c_str() + p + marker.size()));
}

// Prepend 8/9, append 10, with the right BodyLength and CheckSum. `body` is
// everything from tag 35 onward.
inline std::string frame(const std::string& body)
{
  const std::string prefix = std::string("8=") + std::string(kBeginString) + kSoh;
  const std::string lenField = std::string("9=") + std::to_string(body.size()) + kSoh;
  std::string msg = prefix + lenField + body;
  uint32_t sum = 0;
  for (unsigned char ch : msg)
  {
    sum += ch;
  }
  char cs[4];
  std::snprintf(cs, sizeof(cs), "%03u", sum % 256);
  msg += std::string("10=") + cs + kSoh;
  return msg;
}

// Value of the first `tag=` field as u64 (0 when absent or non-numeric).
// Reads the raw message rather than a parsed map, so a caller can pull the
// MsgSeqNum off a frame it has not decoded.
inline uint64_t tagValueU64(const std::string& msg, int tag)
{
  const std::string needle = std::to_string(tag) + "=";
  size_t p = 0;
  while ((p = msg.find(needle, p)) != std::string::npos)
  {
    if (p == 0 || msg[p - 1] == kSoh)  // a field boundary, not a substring of another tag
    {
      return std::strtoull(msg.c_str() + p + needle.size(), nullptr, 10);
    }
    p += needle.size();
  }
  return 0;
}

// MsgSeqNum (34) of a raw FIX message; 0 when absent, which FIX 4.4 makes a
// structurally incomplete message.
inline uint64_t msgSeqNum(const std::string& msg) { return tagValueU64(msg, 34); }

// UTCTimestamp for tag 52: YYYYMMDD-HH:MM:SS.sss from wall-clock nanoseconds.
inline std::string sendingTime(int64_t wallClockNs)
{
  const time_t secs = static_cast<time_t>(wallClockNs / 1'000'000'000);
  const int millis = static_cast<int>((wallClockNs / 1'000'000) % 1000);
  tm g{};
#if defined(_WIN32)
  gmtime_s(&g, &secs);
#else
  gmtime_r(&secs, &g);
#endif
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d:%02d:%02d.%03d", g.tm_year + 1900, g.tm_mon + 1,
                g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, millis);
  return buf;
}

// The five header fields FIX 4.4 requires after MsgType, in the order both
// ends of a flox session write them: 34, 49, 56, 52, then the optional PossDup
// pair. Appended to `b`, which already carries "35=<type><SOH>".
inline void appendHeader(std::string& b, uint64_t seq, const std::string& senderCompId,
                         const std::string& targetCompId, const std::string& sendingTimeStr,
                         bool possDup = false, const std::string& origSendingTime = {})
{
  b += "34=" + std::to_string(seq) + kSoh;
  b += "49=" + senderCompId + kSoh;
  b += "56=" + targetCompId + kSoh;
  b += "52=" + sendingTimeStr + kSoh;
  if (possDup)
  {
    b += std::string("43=Y") + kSoh;
  }
  if (!origSendingTime.empty())
  {
    b += "122=" + origSendingTime + kSoh;
  }
}

// A session/admin message (Logon 35=A, Heartbeat 35=0, TestRequest 35=1,
// ResendRequest 35=2, SequenceReset 35=4, Logout 35=5, Reject 35=3,
// BusinessMessageReject 35=j) with the full header and `fields` as the body.
inline std::string encodeAdmin(const std::string& msgType, uint64_t seq,
                               const std::string& senderCompId, const std::string& targetCompId,
                               const std::string& sendingTimeStr,
                               const std::vector<std::pair<int, std::string>>& fields = {},
                               bool possDup = false, const std::string& origSendingTime = {})
{
  std::string b = "35=" + msgType + kSoh;
  appendHeader(b, seq, senderCompId, targetCompId, sendingTimeStr, possDup, origSendingTime);
  for (const auto& [tag, val] : fields)
  {
    b += std::to_string(tag) + "=" + val + kSoh;
  }
  return frame(b);
}

// Is this a FIX 4.4 APPLICATION message type -- something a counterparty may
// legitimately send that this end does not implement? Those earn a
// business-level reject (35=j). Anything else is a session-level problem and
// earns 35=3.
//
// Deliberately a list rather than "not one of the session types": an
// unrecognised byte is not a message we declined to support, it is a message we
// could not parse, and saying otherwise hides a framing bug behind a polite
// refusal.
inline bool isApplicationMsgType(const std::string& t)
{
  static const char* kAppTypes[] = {
      "6", "7", "8", "9", "AB", "AC", "AD", "AE", "AF", "AG", "AH", "AI", "AJ", "AK", "AL",
      "AM", "AN", "AO", "AP", "AQ", "AR", "AS", "AT", "AU", "AV", "AW", "AX", "AY", "AZ", "B",
      "BA", "BB", "BC", "BD", "BE", "BF", "BG", "C", "D", "E", "F", "G", "H", "J", "K",
      "L", "M", "N", "P", "Q", "R", "S", "T", "V", "W", "X", "Y", "Z", "a", "b",
      "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q",
      "r", "s", "t", "u", "v", "w", "x", "y", "z"};
  for (const char* a : kAppTypes)
  {
    if (t == a)
    {
      return true;
    }
  }
  return false;
}

}  // namespace flox::fix
