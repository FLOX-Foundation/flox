/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Hash-bucket order must not reach anything observable (perimeter half).
 *
 * The FIX session sidecar is the venue's only hand-rolled binary file outside
 * the journal: `<base>.fixsessions`, written at every checkpoint boundary with
 * a CRC32 over the whole payload. Both halves of it -- FixSessionHost's
 * inbound expectedIn and SessionRegistry's outbound lastSeq -- were serialized
 * by walking an unordered_map, so the BYTES, and the CRC over them, were a
 * function of which standard library the venue was built against rather than
 * of the sequence state being persisted. Two venues holding identical sessions
 * wrote different files.
 *
 * Nothing in the suite could see it: restore() reads the entries into a map
 * keyed by account, so a reordered blob restores to the same state and every
 * round-trip test agrees. Only a byte or hash comparison -- a replica diffing
 * its sidecar against the primary's, or a checkpoint compared against a
 * recorded one -- would have found it, and only across libraries.
 *
 * The engine half of the audit is in test_venue_iteration_order.cpp; it has to
 * stay there, because this file needs the perimeter and venue-engine-only
 * configures without it.
 */
#include "flox-venue/fix_session.h"
#include "flox-venue/session_registry.h"

#include "flox/util/crc32.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)

// Scattered rather than 1..N: contiguous small keys land in contiguous
// buckets, the one layout that can come out ascending by accident.
const std::vector<uint64_t> kAccounts = {9007, 13, 480001, 226, 5, 78, 31337,
                                         1902, 64, 7, 250000, 4111, 88, 1500003,
                                         19, 6002};

// The blob both halves speak: [u32 count]{u64 key, u64 value}...
std::vector<uint8_t> blobOf(const std::vector<uint64_t>& accounts)
{
  std::vector<uint8_t> b;
  const auto count = static_cast<uint32_t>(accounts.size());
  b.insert(b.end(), reinterpret_cast<const uint8_t*>(&count),
           reinterpret_cast<const uint8_t*>(&count) + sizeof count);
  for (uint64_t a : accounts)
  {
    const uint64_t v = a * 3 + 1;  // any per-account value; it rides along
    b.insert(b.end(), reinterpret_cast<const uint8_t*>(&a),
             reinterpret_cast<const uint8_t*>(&a) + sizeof a);
    b.insert(b.end(), reinterpret_cast<const uint8_t*>(&v),
             reinterpret_cast<const uint8_t*>(&v) + sizeof v);
  }
  return b;
}

std::vector<uint64_t> accountsIn(const std::vector<uint8_t>& blob)
{
  std::vector<uint64_t> out;
  uint32_t count = 0;
  if (blob.size() < sizeof count)
  {
    return out;
  }
  std::memcpy(&count, blob.data(), sizeof count);
  const uint8_t* p = blob.data() + sizeof count;
  for (uint32_t i = 0; i < count && (p - blob.data()) + 16 <= static_cast<long>(blob.size());
       ++i, p += 16)
  {
    uint64_t a = 0;
    std::memcpy(&a, p, sizeof a);
    out.push_back(a);
  }
  return out;
}

// The inbound half: FixSessionHost::serialize.
void test_fix_session_sidecar_accounts_sorted()
{
  std::printf("test_fix_session_sidecar_accounts_sorted\n");
  std::vector<uint64_t> descending = kAccounts;
  std::sort(descending.begin(), descending.end(), std::greater<uint64_t>());

  FixSessionHost host;
  const auto seed = blobOf(descending);
  CHECK(host.restore(seed.data(), seed.size()) == seed.size());

  std::vector<uint8_t> out;
  host.serialize(out);
  const auto got = accountsIn(out);
  CHECK(got.size() == kAccounts.size());
  CHECK(std::is_sorted(got.begin(), got.end()));

  std::vector<uint64_t> want = kAccounts;
  std::sort(want.begin(), want.end());
  CHECK(got == want);

  // The bytes are a function of the state, not of how the state was reached:
  // a host seeded in the opposite order writes the identical payload, CRC
  // included. This is the claim a replica diffing sidecars actually relies on.
  FixSessionHost other;
  const auto ascending = blobOf(want);
  CHECK(other.restore(ascending.data(), ascending.size()) == ascending.size());
  std::vector<uint8_t> out2;
  other.serialize(out2);
  CHECK(out == out2);
  CHECK(flox::util::Crc32::compute(out.data(), out.size()) ==
        flox::util::Crc32::compute(out2.data(), out2.size()));
}

// The outbound half: SessionRegistry::serializeSeqs, into the same file.
void test_session_registry_seqs_sorted()
{
  std::printf("test_session_registry_seqs_sorted\n");
  std::vector<uint64_t> descending = kAccounts;
  std::sort(descending.begin(), descending.end(), std::greater<uint64_t>());

  SessionRegistry reg;
  const auto seed = blobOf(descending);
  CHECK(reg.restoreSeqs(seed.data(), seed.size()) == seed.size());

  std::vector<uint8_t> out;
  reg.serializeSeqs(out);
  const auto got = accountsIn(out);
  CHECK(got.size() == kAccounts.size());
  CHECK(std::is_sorted(got.begin(), got.end()));

  std::vector<uint64_t> want = kAccounts;
  std::sort(want.begin(), want.end());
  CHECK(got == want);

  SessionRegistry other;
  const auto ascending = blobOf(want);
  CHECK(other.restoreSeqs(ascending.data(), ascending.size()) == ascending.size());
  std::vector<uint8_t> out2;
  other.serializeSeqs(out2);
  CHECK(out == out2);
}

}  // namespace

TEST(IterationOrderSidecar, PerimeterSuite)
{
  test_fix_session_sidecar_accounts_sorted();
  test_session_registry_seqs_sorted();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
