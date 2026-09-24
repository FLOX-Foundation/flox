/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include <cstdint>

namespace flox::venue::hash_tags
{

// Section markers of the engine's determinism digest (MatchingEngine::stateHash).
//
// The digest is one FNV fold over a canonical traversal, and each section
// opens with a marker so two different states cannot collapse onto one value
// by having their payloads line up -- a section that is absent here has to be
// distinguishable from a different section that is present there. That only
// works while the markers are DISTINCT, and three pairs of them were not:
// the delisted marker and the per-account risk limits both folded 0xB00E,
// funding and the firm STP groups both folded 0xB00B, the closed marker and
// the MMP fill windows both folded 0xB00A.
//
// They are named here, in one place, for the same reason a journal tag is:
// so the next section takes the next free number rather than whichever
// constant was nearest to copy. venue/tests/test_venue_state_hash_tags.cpp
// walks kAll and refuses a repeat.
//
// A value here is a FORMAT constant. Changing one changes every state hash
// that folds that section, which invalidates the snapshots already on disk
// that carry it and moves the corresponding golden row.
//
// The three that moved are the later arrivals of each colliding pair, so the
// two session markers -- the oldest of the six and the ones an operator sees
// named in the admin path -- keep the numbers they always had.
inline constexpr uint64_t kRestingOrder = 0xB001;       // one book order
inline constexpr uint64_t kStop = 0xB002;               // one pending conditional
inline constexpr uint64_t kPeg = 0xB003;                // one peg spec
inline constexpr uint64_t kHold = 0xB004;               // one open last-look hold
inline constexpr uint64_t kPosition = 0xB005;           // one cleared position
inline constexpr uint64_t kMmpConfig = 0xB006;          // one account's MMP limit
inline constexpr uint64_t kClOrdIdAccount = 0xB007;     // one account's dedup window
inline constexpr uint64_t kBalance = 0xB008;            // one (account, asset) split
inline constexpr uint64_t kReservation = 0xB009;        // one buying-power reservation
inline constexpr uint64_t kSessionClosed = 0xB00A;      // the session boundary, when set
inline constexpr uint64_t kFunding = 0xB00B;            // the funding rate and calendar
inline constexpr uint64_t kOrderStp = 0xB00C;           // one order's STP mode
inline constexpr uint64_t kAdmissionProfile = 0xB00D;   // one account's admission profile
inline constexpr uint64_t kSessionDelisted = 0xB00E;    // the delisted marker, when set
inline constexpr uint64_t kPostOnlyOrder = 0xB00F;      // a book order's post-only flag, when set
inline constexpr uint64_t kAccountRiskLimits = 0xB010;  // one account's own caps
inline constexpr uint64_t kStpGroup = 0xB011;           // one account's firm STP group
inline constexpr uint64_t kMmpFills = 0xB012;           // one account's MMP window fills

// Every marker above, in the order they are declared. A new section adds its
// constant AND its entry here; the test walks this and nothing else, so a
// marker left out of it is a marker nothing checks.
inline constexpr uint64_t kAll[] = {
    kRestingOrder, kStop, kPeg, kHold, kPosition,
    kMmpConfig, kClOrdIdAccount, kBalance, kReservation, kSessionClosed,
    kFunding, kOrderStp, kAdmissionProfile, kSessionDelisted, kPostOnlyOrder,
    kAccountRiskLimits, kStpGroup, kMmpFills};

}  // namespace flox::venue::hash_tags
