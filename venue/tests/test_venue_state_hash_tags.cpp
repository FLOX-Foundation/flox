/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The state hash's section markers, and the one property they have to have.
 *
 * stateHash is a single FNV fold over a canonical traversal of everything the
 * engine holds. Each section opens with a marker so two different states
 * cannot collapse onto one value by having their payloads line up -- a
 * section that is absent here must stay distinguishable from a different
 * section that is present there, and most of them are folded only when set
 * ("zero == absent"), which is exactly the case the markers separate. A
 * marker shared by two sections gives that property up for both of them.
 *
 * Pinned as a test rather than a static_assert: a static_assert makes a
 * repeated marker a build failure, which is louder but cannot be shown to
 * fail. A test can be run red.
 */
#include "flox-venue/engine/state_hash_tags.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace
{

namespace tags = flox::venue::hash_tags;

// The markers by name, so a failure says which two sections collided rather
// than printing a number twice. Kept in step with kAll by the size check
// below.
struct Named
{
  const char* name;
  uint64_t value;
};

const Named kNamed[] = {
    {"kRestingOrder", tags::kRestingOrder},
    {"kStop", tags::kStop},
    {"kPeg", tags::kPeg},
    {"kHold", tags::kHold},
    {"kPosition", tags::kPosition},
    {"kMmpConfig", tags::kMmpConfig},
    {"kClOrdIdAccount", tags::kClOrdIdAccount},
    {"kBalance", tags::kBalance},
    {"kReservation", tags::kReservation},
    {"kSessionClosed", tags::kSessionClosed},
    {"kFunding", tags::kFunding},
    {"kOrderStp", tags::kOrderStp},
    {"kAdmissionProfile", tags::kAdmissionProfile},
    {"kSessionDelisted", tags::kSessionDelisted},
    {"kPostOnlyOrder", tags::kPostOnlyOrder},
    {"kAccountRiskLimits", tags::kAccountRiskLimits},
    {"kStpGroup", tags::kStpGroup},
    {"kMmpFills", tags::kMmpFills},
};

}  // namespace

// The property itself.
TEST(StateHashTags, EveryMarkerIsItsOwn)
{
  for (size_t i = 0; i < std::size(kNamed); ++i)
  {
    for (size_t j = i + 1; j < std::size(kNamed); ++j)
    {
      EXPECT_NE(kNamed[i].value, kNamed[j].value)
          << kNamed[i].name << " and " << kNamed[j].name
          << " fold the same marker: the two sections are no longer distinguishable";
    }
  }
}

// The list is only worth what it covers. A marker added to the header and
// forgotten here would be a section nothing checks.
TEST(StateHashTags, TheListCoversEveryMarker)
{
  ASSERT_EQ(std::size(kNamed), std::size(tags::kAll))
      << "a marker was added to state_hash_tags.h without a name here";
  for (const uint64_t v : tags::kAll)
  {
    EXPECT_NE(std::find_if(std::begin(kNamed), std::end(kNamed),
                           [v](const Named& n)
                           { return n.value == v; }),
              std::end(kNamed))
        << "marker " << v << " is in kAll under no name";
  }
}
