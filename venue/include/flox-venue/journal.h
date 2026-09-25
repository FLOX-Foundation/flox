/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Write-ahead log for the sequenced venue shard: every inbound command is
 * appended here before it is applied, so a crash can be recovered by replaying
 * the log through a fresh engine.
 *
 * Record framing (native-endian, one record per command):
 *   [ts:8][stamp:1][tag:1][len:4][body:len][crc32:4]
 * stamp carries the format version (see kRecordVersion); tag is the
 * InboundCommand wire tag; len is the body's length -- sizeof the body struct
 * for every command but QuoteLadder, which is written as its head plus the
 * rungs it names (journaledBodySize / quoteLadderBodySize); crc32
 * (flox::util::Crc32) covers ts+stamp+tag+len+body. A torn tail (a record whose
 * bytes are not fully present) or a corrupted record is DETECTED on load: the
 * loader returns the largest intact prefix and never materialises a
 * partial/garbage command. The crc is what separates damage from a foreign
 * format, so it is verified BEFORE the stamp and the tag it covers are
 * believed; loadReported names which of the two ended the read and where.
 *
 * Format compatibility: ONE version is readable, the one this build writes.
 * A file in any other version is refused by name (JournalFormatError) rather
 * than decoded, truncated or guessed at. See docs/venue/runtime.md.
 *
 * Durability: the writer uses a raw file descriptor, so an appended record is
 * in the OS page cache before append() returns -- it survives a process crash
 * even in the default (Sync::Off) mode, unlike a userspace-buffered stream.
 * Sync::Full additionally fsync()s each record, so it also survives power loss;
 * that is the mode a production venue should run its shard WAL in (trading
 * throughput for durability). SequencedShard selects the mode.
 */
#pragma once

#include "flox-venue/messages.h"
#include "flox/util/file_io.h"

#include "flox/util/base/scale_check.h"  // FLOX_SCALE_CHECKS: it changes the on-disk layout
#include "flox/util/crc32.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace flox::venue
{

static_assert(std::is_trivially_copyable_v<NewOrder>, "NewOrder must be blittable");
static_assert(std::is_trivially_copyable_v<CancelOrder>, "CancelOrder must be blittable");
static_assert(std::is_trivially_copyable_v<ModifyOrder>, "ModifyOrder must be blittable");
static_assert(std::is_trivially_copyable_v<MassCancel>, "MassCancel must be blittable");
static_assert(std::is_trivially_copyable_v<Quote>, "Quote must be blittable");
static_assert(std::is_trivially_copyable_v<LastLookDecision>, "LastLookDecision must be blittable");
static_assert(std::is_trivially_copyable_v<SetMark>, "SetMark must be blittable");
static_assert(std::is_trivially_copyable_v<ApplyFunding>, "ApplyFunding must be blittable");
static_assert(std::is_trivially_copyable_v<AdminCmd>, "AdminCmd must be blittable");
static_assert(std::is_trivially_copyable_v<Deposit>, "Deposit must be blittable");
static_assert(std::is_trivially_copyable_v<Withdraw>, "Withdraw must be blittable");
static_assert(std::is_trivially_copyable_v<ListInstrument>, "ListInstrument must be blittable");
static_assert(std::is_trivially_copyable_v<SetBands>, "SetBands must be blittable");
static_assert(std::is_trivially_copyable_v<TimeTick>, "TimeTick must be blittable");
static_assert(std::is_trivially_copyable_v<SetTriggerRef>, "SetTriggerRef must be blittable");
static_assert(std::is_trivially_copyable_v<SnapshotBegin>, "SnapshotBegin must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreOrder>, "RestoreOrder must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreStop>, "RestoreStop must be blittable");
static_assert(std::is_trivially_copyable_v<RestorePeg>, "RestorePeg must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreHeld>, "RestoreHeld must be blittable");
static_assert(std::is_trivially_copyable_v<RestorePosition>, "RestorePosition must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreMmpCfg>, "RestoreMmpCfg must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreClOrdIds>, "RestoreClOrdIds must be blittable");
static_assert(std::is_trivially_copyable_v<SnapshotEnd>, "SnapshotEnd must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreReservation>,
              "RestoreReservation must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreBalance>, "RestoreBalance must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreMmpFills>, "RestoreMmpFills must be blittable");
static_assert(std::is_trivially_copyable_v<SetStpGroup>, "SetStpGroup must be blittable");
static_assert(std::is_trivially_copyable_v<SetFundingSchedule>,
              "SetFundingSchedule must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreFunding>, "RestoreFunding must be blittable");
static_assert(std::is_trivially_copyable_v<ForceClosePosition>,
              "ForceClosePosition must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreOrderStp>, "RestoreOrderStp must be blittable");
static_assert(std::is_trivially_copyable_v<SetAdmissionProfile>,
              "SetAdmissionProfile must be blittable");
static_assert(std::is_trivially_copyable_v<SetRiskLimits>, "SetRiskLimits must be blittable");
static_assert(std::is_trivially_copyable_v<AdjustPosition>, "AdjustPosition must be blittable");
static_assert(std::is_trivially_copyable_v<SetAccountRiskLimits>,
              "SetAccountRiskLimits must be blittable");
static_assert(std::is_trivially_copyable_v<QuoteLadderLevel>,
              "QuoteLadderLevel must be blittable");
static_assert(std::is_trivially_copyable_v<QuoteLadder>, "QuoteLadder must be blittable");
static_assert(std::variant_size_v<InboundCommand> == 37,
              "new InboundCommand alternative: extend expectedBodySize/appendDecoded and the "
              "blittable asserts above");

// T057: every journaled/snapshot body is written as raw bytes (Journal::append
// below), so any compiler-inserted alignment padding a struct above carries
// would reach disk uninitialised. The fix is in messages.h, not here: each gap
// the compiler would otherwise insert implicitly is instead an explicit
// `uint8_t padN_[k]{}` member. An explicit field has a default member
// initializer like every other field, so ordinary construction -- aggregate
// init that leaves it unspecified, or the type's own default constructor --
// zeroes it the same way it zeroes any other field the caller did not set;
// nothing special about padding has to hold, and nothing runs here at
// serialization time to compensate for it.
//
// `has_unique_object_representations_v<T>` is true exactly when T has no
// padding bits (and no field whose value has more than one valid bit
// pattern), so it is the direct, compiler-checked statement of "this struct
// has no implicit padding left" -- it fails again, loudly, the moment a new
// field reintroduces a gap this list does not know about. ApplyFunding is the
// one exception: it carries a `double`, and the trait is specified false for
// any type with a floating-point member regardless of padding (multiple bit
// patterns can represent the same value, e.g. NaN), so it is checked by
// sizeof instead -- the sum of its fields' sizes already includes the
// explicit pad field, so if that sum ever stops matching sizeof(ApplyFunding),
// either a field changed size or a gap opened up again.
static_assert(std::has_unique_object_representations_v<NewOrder>, "NewOrder carries padding");
static_assert(std::has_unique_object_representations_v<CancelOrder>, "CancelOrder carries padding");
static_assert(std::has_unique_object_representations_v<ModifyOrder>, "ModifyOrder carries padding");
static_assert(std::has_unique_object_representations_v<MassCancel>, "MassCancel carries padding");
static_assert(std::has_unique_object_representations_v<Quote>, "Quote carries padding");
static_assert(std::has_unique_object_representations_v<LastLookDecision>,
              "LastLookDecision carries padding");
static_assert(std::has_unique_object_representations_v<SetMark>, "SetMark carries padding");
static_assert(sizeof(ApplyFunding) == sizeof(ApplyFunding::symbol) + sizeof(ApplyFunding::pad0_) +
                                          sizeof(ApplyFunding::rate) + sizeof(ApplyFunding::mark),
              "ApplyFunding carries padding beyond its explicit pad field (has_unique_object_"
              "representations_v is unconditionally false here because of the double member, "
              "not because of padding, so it cannot be used for this one)");
static_assert(std::has_unique_object_representations_v<AdminCmd>, "AdminCmd carries padding");
static_assert(std::has_unique_object_representations_v<Deposit>, "Deposit carries padding");
static_assert(std::has_unique_object_representations_v<Withdraw>, "Withdraw carries padding");
static_assert(std::has_unique_object_representations_v<ListInstrument>,
              "ListInstrument carries padding");
static_assert(std::has_unique_object_representations_v<SetBands>, "SetBands carries padding");
static_assert(std::has_unique_object_representations_v<TimeTick>, "TimeTick carries padding");
static_assert(std::has_unique_object_representations_v<SetTriggerRef>,
              "SetTriggerRef carries padding");
static_assert(std::has_unique_object_representations_v<SnapshotBegin>,
              "SnapshotBegin carries padding");
static_assert(std::has_unique_object_representations_v<RestoreOrder>,
              "RestoreOrder carries padding");
static_assert(std::has_unique_object_representations_v<RestoreStop>, "RestoreStop carries padding");
static_assert(std::has_unique_object_representations_v<RestorePeg>, "RestorePeg carries padding");
static_assert(std::has_unique_object_representations_v<RestoreHeld>, "RestoreHeld carries padding");
static_assert(std::has_unique_object_representations_v<RestorePosition>,
              "RestorePosition carries padding");
static_assert(std::has_unique_object_representations_v<RestoreMmpCfg>,
              "RestoreMmpCfg carries padding");
static_assert(std::has_unique_object_representations_v<RestoreClOrdIds>,
              "RestoreClOrdIds carries padding");
static_assert(std::has_unique_object_representations_v<SnapshotEnd>, "SnapshotEnd carries padding");
static_assert(std::has_unique_object_representations_v<RestoreReservation>,
              "RestoreReservation carries padding");
static_assert(std::has_unique_object_representations_v<RestoreBalance>,
              "RestoreBalance carries padding");
static_assert(std::has_unique_object_representations_v<RestoreMmpFills>,
              "RestoreMmpFills carries padding");
static_assert(std::has_unique_object_representations_v<SetStpGroup>, "SetStpGroup carries padding");
static_assert(std::has_unique_object_representations_v<SetFundingSchedule>,
              "SetFundingSchedule carries padding");
static_assert(std::has_unique_object_representations_v<RestoreFunding>,
              "RestoreFunding carries padding");
static_assert(std::has_unique_object_representations_v<ForceClosePosition>,
              "ForceClosePosition carries padding");
static_assert(std::has_unique_object_representations_v<RestoreOrderStp>,
              "RestoreOrderStp carries padding");
static_assert(std::has_unique_object_representations_v<AdmissionProfile>,
              "AdmissionProfile (nested in SetAdmissionProfile) carries padding");
static_assert(std::has_unique_object_representations_v<SetAdmissionProfile>,
              "SetAdmissionProfile carries padding");
static_assert(std::has_unique_object_representations_v<SetRiskLimits>,
              "SetRiskLimits carries padding");
static_assert(std::has_unique_object_representations_v<SetAccountRiskLimits>,
              "SetAccountRiskLimits carries padding");
static_assert(std::has_unique_object_representations_v<AdjustPosition>,
              "AdjustPosition carries padding");
static_assert(std::has_unique_object_representations_v<QuoteLadderLevel>,
              "QuoteLadderLevel carries padding");
static_assert(std::has_unique_object_representations_v<QuoteLadder>,
              "QuoteLadder carries padding");
// The ladder is the one body written short: a record carries the head and the
// rungs it names, never the unused tail of the block (quoteLadderBodySize).
// So the head has to END on the level array, with nothing between them that a
// short write would drop and a reader would then invent -- which is exactly
// what offsetof(level) == the sum of the fields before it says.
static_assert(kQuoteLadderHeadSize ==
                  sizeof(QuoteLadder::accountId) + sizeof(QuoteLadder::clientOrderId) +
                      sizeof(QuoteLadder::expiryNs) + sizeof(QuoteLadder::visibleQuantity) +
                      sizeof(QuoteLadder::bidIdBase) + sizeof(QuoteLadder::askIdBase) +
                      sizeof(QuoteLadder::symbol) + sizeof(QuoteLadder::levels) +
                      sizeof(QuoteLadder::stp) + sizeof(QuoteLadder::lastLook) +
                      sizeof(QuoteLadder::postOnly) + sizeof(QuoteLadder::reduceOnly) +
                      sizeof(QuoteLadder::tif) + sizeof(QuoteLadder::pad0_),
              "QuoteLadder has a gap between its head and its levels");
static_assert(sizeof(QuoteLadder) ==
                  kQuoteLadderHeadSize + kQuoteLadderLevels * sizeof(QuoteLadderLevel),
              "QuoteLadder carries something after its levels");

// Version of the on-disk record format this build writes and reads. Bodies go
// to disk as raw bytes, so the version stands for the layout of all 34 command
// structs as much as for the framing around them.
//
// A scale-checked build is a DIFFERENT format, not a debugging variant of the
// same one: FLOX_SCALE_CHECKS widens Decimal, so sizeof(Price) goes 8 -> 16 and
// every body that holds a price or a quantity moves its fields. Giving the two
// layouts one version number would mean a journal written by a debug venue is
// read at the wrong offsets by a release one, which is the failure this whole
// stamp exists to prevent. They get separate numbers and refuse each other by
// name.
//
// The two numbers move by TWO when the layout changes, never by one: bumping
// both by one would give the unchecked build the number the checked build
// just vacated, and a checked journal would then pass the version test in an
// unchecked reader and be decoded at the wrong offsets -- the exact failure
// the separate numbering exists to prevent.
//
// 9/10: FillHeld carries the taker's side. Nothing journaled changed size --
// the fingerprint below is still the one 7/8 stood for, and proves it -- but
// the number moves anyway, because it names the format GENERATION a build
// speaks rather than the body sizes alone: a journal replayed by this build
// produces a different exec-report stream than the same file replayed by the
// previous one, and an operator pairing a file with the reports recovered
// from it reads that off one number.
// 9/10 -> 11/12: Quote gained clientOrderId (see docs/venue/runtime.md). A file
// written by a build without it holds quote-spawned orders whose reports name
// nobody on either leg, so it is refused rather than read as though the field
// had always been absent.
// 11/12 -> 13/14: FillHeld and FillRejected carry the taker's clientOrderId.
// Nothing journaled changed size -- the fingerprint below is still the one
// 9/10 stood for, and proves it -- but the number moves anyway, for the same
// reason it did at 9/10: a journal replayed by this build produces a
// different exec-report stream (the hold and its reject now name the taker)
// than the same file replayed by the previous one.
// 13/14 -> 15/16: QuoteLadder (tag 35), a market maker's whole set of levels
// on one symbol in ONE record instead of one Quote per level. A build without
// the alternative has no type for tag 35 at all, so it would refuse such a
// file by tag rather than read it -- the version pair says so first, and by
// name. It is also the first body written SHORT: a ladder record carries the
// head plus the rungs it names (quoteLadderBodySize), so the length of a
// record is a property of the record and not only of its tag. Every other tag
// is unchanged and still fixed-length.
// 15/16 -> 17/18 (T058): RestoreOrder (tag unchanged) grows one field,
// cumQty -- the resting order's running fill total, restored so a cancel
// reported after a recovery still carries its real FIX CumQty instead of
// resetting to 0. Every other journaled body is unchanged and still
// fixed-length; only RestoreOrder's sizeof moved. The pair moves by two, not
// one: 16 already names the scale-checked build's PREVIOUS format (see
// "moves by two" in docs/venue/runtime.md), so the unchecked build's new
// number cannot land on it without colliding with that older format.
// 17/18 -> 19/20 (T059): RestoreHeld (tag unchanged) grows two fields,
// makerCumQtyAtHold/takerCumQtyAtHold -- each leg's confirmed running fill
// total as of the moment a last-look hold opened, restored so a hold that
// resolves after a recovery still reports the real FIX CumQty on
// FillHeld/FillRejected and on the reports that rebuild a leg the hold took
// fully off the book, instead of resetting to 0. Every other journaled body
// is unchanged and still fixed-length; only RestoreHeld's sizeof moved. Pair
// moves by two, same reasoning as 15/16 -> 17/18.
// 21/22 (W26-T064): a new journaled body, SetAccountRiskLimits (tag 36),
// carried in the snapshot's config section. Every earlier body is unchanged;
// the pair moves by two for the same reason 19/20 did.
// 21/22 -> 23/24 (the snapshot round-trip fix): RestoreClOrdIds (tag unchanged) grows one field,
// rotatedAtNs -- the account's window rotation moment, which the state hash
// has always folded and the snapshot never carried, so any engine configured
// with clOrdIdWindowNs > 0 wrote checkpoints that failed their own hash check
// at SnapshotEnd. Every other journaled body is unchanged and still
// fixed-length; only RestoreClOrdIds' sizeof moved. Pair moves by two, same
// reasoning as 19/20 -> 21/22.
#if FLOX_SCALE_CHECKS
inline constexpr uint8_t kRecordVersion = 24;
#else
inline constexpr uint8_t kRecordVersion = 23;
#endif

// Bit 7 of the stamp byte marks a versioned record; bits 0-6 carry the version.
// The mark exists so a file written before versioning is recognised as such
// instead of being misread: its byte at that offset is the variant tag, which
// is below 35 and therefore always has bit 7 clear.
inline constexpr uint8_t kVersionedMark = 0x80;
inline constexpr uint8_t kVersionMask = 0x7F;
inline constexpr uint8_t kRecordStamp = kVersionedMark | kRecordVersion;

// Fingerprint of the wire layout kRecordVersion stands for: FNV-1a over the
// size of every journaled body, in tag order.
consteval uint64_t bodyLayoutFingerprint()
{
  // Sizes taken from the variant itself, not from a list written out below.
  // The list used to be hand-maintained in declaration order, which coupled it
  // to the variant by POSITION: reorder the alternatives and every size paired
  // with the wrong tag, silently, because both lists still looked complete.
  // A list is data, not knowledge -- so it is derived.
  //
  // Folded in TAG order. That is what makes this a property of the format
  // rather than of the variant's current shape: rearrange the type and the
  // value does not move, because the bytes on disk do not move either.
  constexpr size_t kN = std::variant_size_v<InboundCommand>;
  struct Entry
  {
    uint8_t tag;
    size_t size;
  };
  Entry entries[kN]{};
  [&]<size_t... I>(std::index_sequence<I...>)
  {
    ((entries[I] = Entry{kWireTag[I], sizeof(std::variant_alternative_t<I, InboundCommand>)}), ...);
  }(std::make_index_sequence<kN>{});

  for (size_t i = 1; i < kN; ++i)
  {
    const Entry key = entries[i];
    size_t j = i;
    while (j > 0 && entries[j - 1].tag > key.tag)
    {
      entries[j] = entries[j - 1];
      --j;
    }
    entries[j] = key;
  }

  uint64_t h = 1469598103934665603ULL;
  for (const Entry& e : entries)
  {
    h ^= static_cast<uint64_t>(e.tag);
    h *= 1099511628211ULL;
    h ^= static_cast<uint64_t>(e.size);
    h *= 1099511628211ULL;
  }
  return h;
}

// The guard the format did not have. A field added to any journaled command
// silently changed what a record means and surfaced much later, as a length
// that did not add up during recovery. Now it stops the build here, next to
// the version it invalidates.
#if FLOX_SCALE_CHECKS
static_assert(bodyLayoutFingerprint() == 0x0159e90e53c0851bULL,
              "a journaled command struct changed size, so the on-disk layout is no longer the "
              "one kRecordVersion promises. Bump kRecordVersion, update this fingerprint, and "
              "record the change in docs/venue/runtime.md");
#else
static_assert(bodyLayoutFingerprint() == 0xe6bd1bd208922c4bULL,
              "a journaled command struct changed size, so the on-disk layout is no longer the "
              "one kRecordVersion promises. Bump kRecordVersion, update this fingerprint, and "
              "record the change in docs/venue/runtime.md");
#endif

// A journal or snapshot written in a format version this build does not read.
// Distinct from a torn tail or a crc failure, which are recoverable and leave
// the intact prefix behind: this one says the file is not ours to decode.
// A record tag no type in this build answers to. Names the tag and where it
// sits, because the first question is always "which command, and how far in".
inline std::string unknownTagMessage(const std::string& path, uint8_t tag, size_t afterRecords)
{
  return "journal '" + path + "': record tag " + std::to_string(static_cast<unsigned>(tag)) +
         " after " + std::to_string(afterRecords) + " good records" +
         " is not a command this build knows. The file was written by a build with a command "
         "this one does not have; reading past it would silently return a prefix of the history.";
}

// Bytes of a command that actually go into its record. sizeof for all but one
// of them: the ladder is written as its head plus the rungs it names, so a
// five-level ladder costs five levels of wire and not eight. Everything else
// about the framing is unchanged -- the length still travels in the header,
// and the crc still covers exactly the bytes written.
template <class T>
inline constexpr size_t journaledBodySize(const T&) noexcept
{
  return sizeof(T);
}

inline constexpr size_t journaledBodySize(const QuoteLadder& l) noexcept
{
  return quoteLadderBodySize(l);
}

class JournalFormatError : public std::runtime_error
{
 public:
  explicit JournalFormatError(const std::string& what) : std::runtime_error(what) {}
};

class Journal
{
 public:
  enum class Sync
  {
    Off,    // record is in the OS cache before append returns (survives process crash)
    Full,   // + fsync per record (survives power loss); production WAL default
    Group,  // + fsync on demand, not per record: the caller decides where the
            // barrier goes and pays for one fsync per batch instead of one per
            // command. Only durable if the caller calls sync() BEFORE telling
            // anyone the commands took effect -- an unsynced batch that has
            // already been acknowledged is exactly the promise Full exists to
            // keep, broken more cheaply.
  };

  // A recovering writer MUST open in Append: Truncate erases the very log the
  // process is supposed to replay. SequencedShard always uses Append; Truncate
  // is for starting a fresh log on a path the caller knows is disposable.
  enum class OpenMode
  {
    Truncate,
    Append,
  };

  // ts + stamp + tag + len = 14
  static constexpr size_t kHeaderSize = sizeof(int64_t) + 1 + 1 + sizeof(uint32_t);

  explicit Journal(const std::string& path, Sync sync = Sync::Off,
                   OpenMode mode = OpenMode::Truncate)
      : sync_(sync)
  {
    fd_ = openFile(path, mode);
  }

  ~Journal()
  {
    if (fd_ >= 0)
    {
      flox::fileio::closeFd(fd_);
    }
  }

  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  // Close the current file and continue on `path` (segment rotation at a
  // checkpoint). Resets the per-segment record/byte counters; they count
  // appends since open, so on an Append reopen of an existing file they
  // approximate the segment size from this process's perspective only.
  void reopen(const std::string& path, OpenMode mode)
  {
    if (fd_ >= 0)
    {
      flox::fileio::syncFd(fd_);
      flox::fileio::closeFd(fd_);
    }
    fd_ = openFile(path, mode);
    count_.store(0, std::memory_order_relaxed);
    bytes_.store(0, std::memory_order_relaxed);
  }

  void append(const InboundCommand& c) { append(c, 0); }

  // Write-ahead record: [ts][stamp][tag][len][body][crc]. The sequencer
  // timestamp is journaled so replay reproduces last-look / MMP / LULD timing
  // exactly; the stamp names the format the body was written in.
  void append(const InboundCommand& c, int64_t tsNs)
  {
    // The alternative's own tag, not its position in the variant. See
    // kWireTag: the two used to be the same thing, which made reordering the
    // variant a silent re-read of every old journal.
    const uint8_t tag = wireTagOf(c);
    std::visit(
        [&](const auto& v)
        {
          const uint32_t len = static_cast<uint32_t>(journaledBodySize(v));
          const uint8_t stamp = kRecordStamp;
          rec_.clear();
          appendBytes(&tsNs, sizeof(tsNs));
          appendBytes(&stamp, sizeof(stamp));
          appendBytes(&tag, sizeof(tag));
          appendBytes(&len, sizeof(len));
          // T057: every byte of v is real data, including what used to be
          // implicit alignment padding -- messages.h now names those bytes as
          // explicit pad fields with their own default member initializer, so
          // v carries zeros there the same way it carries zeros in any other
          // field the caller left unset. Nothing to zero here.
          //
          // `len` is sizeof(v) for every command but the ladder, whose unused
          // tail is not written at all -- which is the point of the record,
          // and is safe for exactly the same reason: the bytes left out are
          // the ones that were zero.
          appendBytes(&v, len);
          const uint32_t crc = flox::util::Crc32::compute(rec_.data(), rec_.size());
          appendBytes(&crc, sizeof(crc));
        },
        c);
    writeAll(rec_.data(), rec_.size());
    if (sync_ == Sync::Full)
    {
      flox::fileio::syncFd(fd_);
    }
    else if (sync_ == Sync::Group)
    {
      dirty_ = true;
    }
    count_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(rec_.size(), std::memory_order_relaxed);
  }

  // Make everything appended since the last call durable. A no-op unless there
  // is something to make durable, so calling it on every quiet batch costs a
  // branch rather than a syscall.
  void sync()
  {
    if (dirty_ && fd_ >= 0)
    {
      flox::fileio::syncFd(fd_);
      ++syncs_;
      dirty_ = false;
    }
  }

  // Barriers taken. Observable because the barrier's ORDER relative to
  // publication is testable from this machine and its physics is not: a normal
  // read sees the page cache whether or not fsync ran, so a test that reads the
  // file back proves nothing about durability. This counter proves the code
  // took the barrier before it spoke, which is the part a test can own.
  uint64_t syncs() const noexcept { return syncs_; }

  void flush()
  {
    if (fd_ >= 0)
    {
      flox::fileio::syncFd(fd_);
    }
  }
  // Records/bytes appended since open (atomic: the shard's idle sweeper reads
  // them from another thread for the checkpoint auto-trigger).
  uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
  uint64_t bytes() const noexcept { return bytes_.load(std::memory_order_relaxed); }

  static std::vector<InboundCommand> load(const std::string& path)
  {
    std::vector<InboundCommand> v;
    for (auto& [ts, cmd] : loadTimed(path))
    {
      (void)ts;
      v.push_back(cmd);
    }
    return v;
  }

  // How a load ended.
  //
  // Intact:  the read stopped on a record boundary with nothing left over.
  // Torn:    the last record's bytes are not all in the file -- the ordinary
  //          shape of a crash, and the prefix ahead of it is sound.
  // Corrupt: a record failed to verify and WHOLE BYTES FOLLOW it, so the file
  //          was not cut short: it rotted, and there is history behind the
  //          hole that this build will not replay.
  enum class Tail : uint8_t
  {
    Intact,
    Torn,
    Corrupt,
  };

  // The same read loadTimed does, with the stop described instead of implied.
  // "Returned nine records" and "returned nine records and the tenth was torn"
  // are different answers, and only the second lets an operator tell a clean
  // shutdown from a crash without diffing file sizes by hand.
  struct LoadReport
  {
    std::vector<std::pair<int64_t, InboundCommand>> records;
    Tail tail{Tail::Intact};
    // Byte offset of the first record that was not recovered; the file size
    // when the tail is Intact.
    uint64_t stopOffset{0};
  };

  // Replay records with their sequencer timestamps. Stops at the first record
  // that is short (torn tail) or fails its crc (corruption), returning the
  // intact prefix -- a partial trailing record is never materialised.
  //
  // Throws JournalFormatError if a record carries a format version this build
  // does not read, or a tag it has no command for. That is deliberately NOT
  // the torn-tail treatment: a torn tail is the expected shape of a crash and
  // the prefix before it is sound, whereas a foreign version means every byte
  // after the header was laid out by rules this build does not have. Returning
  // a prefix there would hand back a state that looks plausible and is short of
  // history, which is the outcome the whole recovery path is written to refuse.
  static std::vector<std::pair<int64_t, InboundCommand>> loadTimed(const std::string& path)
  {
    return loadReported(path).records;
  }

  // THE CRC DECIDES WHICH FAILURE THIS IS, so it is checked before anything
  // the header says is believed.
  //
  // The stamp and the tag live in the header, and the crc covers the header.
  // Testing them first -- as this loader used to -- asks a damaged byte what
  // format the record is in and then refuses the whole file on its answer: one
  // flipped bit in the last record's stamp cost every record before it, which
  // is the opposite of the promise at the top of this file. A record whose crc
  // does not cover its own bytes was damaged after it was written, whatever
  // its stamp now reads, and damage stops the read with the intact prefix.
  //
  // Only once the crc has passed is the header the writer's own: then a
  // foreign version or a tag this build has no command for is a real format
  // statement and is refused by name, exactly as before.
  //
  // The other half of the rule: "the largest intact prefix" has to be a prefix
  // OF SOMETHING. A file whose very first record does not read has never been
  // this build's file, and handing back zero records from it is not a prefix,
  // it is the whole history lost without a word -- the outcome every check in
  // this loader exists to refuse. So a stop at offset 0, with a header there to
  // read, is refused by name rather than returned. That is also where a
  // genuinely foreign file usually lands: a version that moves a field moves
  // the frame the crc is computed over, so a foreign build's crc does not match
  // the one computed here either. "Crc valid, stamp foreign" is the narrow
  // case; "the file reads as nothing at all" is the common one.
  //
  // Truncation is the exception, because truncation is not a statement about
  // format: a file that simply ENDS inside a record whose shape this build
  // writes -- or before a whole header -- is an append caught by a power cut,
  // and a segment rotated and then cut inside its first append must still let
  // the shard start. That comes back as an empty, Torn prefix.
  static LoadReport loadReported(const std::string& path)
  {
    LoadReport report;
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
      return report;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff endPos = in.tellg();
    const uint64_t fileSize = endPos > 0 ? static_cast<uint64_t>(endPos) : 0;
    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> frame;  // header+body, staged for one-shot crc
    uint64_t off = 0;
    while (true)
    {
      report.stopOffset = off;
      const uint64_t left = fileSize - off;
      if (left == 0)
      {
        report.tail = Tail::Intact;
        break;
      }
      if (left < kHeaderSize)
      {
        report.tail = Tail::Torn;  // not even a header left
        break;
      }
      frame.resize(kHeaderSize);
      if (!in.read(reinterpret_cast<char*>(frame.data()), kHeaderSize))
      {
        report.tail = Tail::Torn;
        break;
      }
      uint32_t len;
      std::memcpy(&len, frame.data() + 10, sizeof(len));
      // A header is there, so from here on a stop that recovers nothing is a
      // file this build cannot read rather than a prefix of one it can.
      const uint8_t stamp = frame[8];
      const auto refuseIfNothingRead = [&]
      {
        if (report.records.empty())
        {
          throw JournalFormatError(unreadableFileMessage(path, stamp));
        }
      };
      // A length past the largest body this build writes describes no record
      // this build could have produced, so it is not a cut-short one of ours:
      // a truncated record of ours still names one of our lengths. There is no
      // crc to reach past it either, and the header's number must not become
      // the allocation it asked for.
      if (len > maxBodySize())
      {
        refuseIfNothingRead();
        report.tail = off + kHeaderSize < fileSize ? Tail::Corrupt : Tail::Torn;
        break;
      }
      // The extent the header claims, now that the claim is one this build
      // could have written.
      const uint64_t need = static_cast<uint64_t>(kHeaderSize) + len + sizeof(uint32_t);
      // The file ends inside a record whose shape is ours: an append caught by
      // a power cut, the ordinary torn tail. This is the one stop that is NOT
      // refused when it recovers nothing -- a segment rotated and then cut
      // inside its first append is a crash, not an unreadable file.
      if (need > left)
      {
        report.tail = Tail::Torn;
        break;
      }
      frame.resize(kHeaderSize + len);
      if (!in.read(reinterpret_cast<char*>(frame.data() + kHeaderSize), len))
      {
        report.tail = Tail::Torn;  // torn body
        break;
      }
      uint32_t crc;
      if (!in.read(reinterpret_cast<char*>(&crc), sizeof(crc)))
      {
        report.tail = Tail::Torn;  // torn crc
        break;
      }
      // Whole, and verified or not: from here the record either stops the read
      // as damage, or its header speaks for the build that wrote it.
      const Tail damaged = off + need < fileSize ? Tail::Corrupt : Tail::Torn;
      if (flox::util::Crc32::compute(frame.data(), frame.size()) != crc)
      {
        refuseIfNothingRead();
        report.tail = damaged;
        break;
      }

      if (stamp != kRecordStamp)
      {
        throw JournalFormatError(versionMismatchMessage(path, report.records.size(), stamp));
      }
      const uint8_t tag = frame[9];
      const uint32_t expect = expectedBodySize(tag);
      if (expect == 0)
      {
        // A tag this build has no record type for, on a record the crc says
        // was written exactly as it reads. That means a file written by a
        // build that knows a command this one does not -- and stopping here
        // quietly would hand back a PREFIX of the history as if it were all of
        // it: recovery lands in a state the venue was never in, and nothing
        // says so. A short read is a torn tail and is fine to stop on; this is
        // not.
        throw JournalFormatError(unknownTagMessage(path, tag, report.records.size()));
      }
      if (!bodySizeAccepted(tag, len, expect))
      {
        refuseIfNothingRead();
        report.tail = damaged;  // body the wrong size for its tag: corrupt, stop
        break;
      }

      int64_t ts;
      std::memcpy(&ts, frame.data(), sizeof(ts));
      // The one length that the framing alone cannot confirm: a ladder's
      // record length has to be the length its own `levels` byte asks for.
      // The crc has already passed, so a mismatch is not damage in transit --
      // it is a record laid out by rules this build does not have, and
      // applying its rungs would replay a ladder nobody sent.
      if (tag == kQuoteLadderWireTag && !quoteLadderLengthAgrees(frame.data() + kHeaderSize, len))
      {
        refuseIfNothingRead();
        report.tail = damaged;
        break;
      }
      appendDecoded(report.records, ts, tag, frame.data() + kHeaderSize, len);
      off += need;
    }
    return report;
  }

 private:
  // What to tell an operator whose file this build will not read. The point is
  // to name the two versions: the old failure mode was a length that did not
  // add up, several structs away from the field that had changed.
  static std::string versionMismatchMessage(const std::string& path, size_t recordsBefore,
                                            uint8_t stamp)
  {
    std::string msg = "Journal: '" + path + "' record " + std::to_string(recordsBefore) + " is ";
    if ((stamp & kVersionedMark) == 0)
    {
      msg += "in the unversioned format (version 0, written before records carried a version)";
    }
    else
    {
      msg += "format version " + std::to_string(static_cast<unsigned>(stamp & kVersionMask));
    }
    msg += "; this build reads version " + std::to_string(static_cast<unsigned>(kRecordVersion)) +
           " only. Older versions are not converted -- replay the file with the build that "
           "wrote it, or start from a snapshot taken by this one.";
    return msg;
  }

  // What to tell an operator whose file reads as nothing at all. Named off the
  // header at offset 0, because that is the only account of the file there is
  // -- and it is usually the true one: a format change moves fields, which
  // moves the frame the crc covers, so a foreign file typically fails the crc
  // here rather than passing it with a foreign stamp.
  static std::string unreadableFileMessage(const std::string& path, uint8_t stamp)
  {
    if (stamp == kRecordStamp)
    {
      return "Journal: '" + path +
             "' record 0 does not verify against its own crc, so nothing in this file has been "
             "read. An empty result would be the whole history gone without a word, so the file "
             "is refused instead -- recover from a snapshot, or from the generation before it.";
    }
    return versionMismatchMessage(path, 0, stamp) +
           " Its crc does not cover the bytes on disk either, which is what a frame laid out by "
           "other rules looks like from here; nothing was decoded.";
  }

  // Expected body size for a variant tag, or 0 if the tag is unknown.
  // Public so a test can hold the decoder to the invariant that makes
  // reordering safe: the size expected for a tag is the size of the
  // alternative that owns it.
 public:
  static constexpr uint32_t expectedBodySizeForTag(uint8_t tag) { return expectedBodySize(tag); }

 private:
  // The largest body any command in this build occupies on disk. The bound the
  // loader reads a record against when the header's own length is the part it
  // cannot trust yet.
  static constexpr uint32_t maxBodySize() noexcept
  {
    uint32_t m = 0;
    for (unsigned t = 0; t < 256; ++t)
    {
      const uint32_t e = expectedBodySize(static_cast<uint8_t>(t));
      m = e > m ? e : m;
    }
    return m;
  }

  // Is `len` a length this tag can legitimately have? For every fixed-length
  // command that is "the one size its type has". The ladder is the exception
  // the whole record exists for: its body is the head plus 0..K rungs, so the
  // decoder checks the SHAPE of the length here and the record's own `levels`
  // byte confirms which of those shapes it is (quoteLadderLengthAgrees).
  static bool bodySizeAccepted(uint8_t tag, uint32_t len, uint32_t expect)
  {
    if (tag != kQuoteLadderWireTag)
    {
      return len == expect;
    }
    if (len < kQuoteLadderHeadSize || len > expect)
    {
      return false;
    }
    return (len - kQuoteLadderHeadSize) % sizeof(QuoteLadderLevel) == 0;
  }

  // The record's length against the record's own account of it. Read off the
  // raw body rather than off a decoded struct: a length that disagrees must
  // stop the load before anything is materialised from it.
  static bool quoteLadderLengthAgrees(const uint8_t* body, uint32_t len)
  {
    QuoteLadder probe{};
    std::memcpy(&probe.levels, body + offsetof(QuoteLadder, levels), sizeof(probe.levels));
    return quoteLadderBodySize(probe) == len;
  }

  static constexpr uint32_t expectedBodySize(uint8_t tag)
  {
    switch (tag)
    {
      case 0:
        return sizeof(NewOrder);
      case 1:
        return sizeof(CancelOrder);
      case 2:
        return sizeof(ModifyOrder);
      case 3:
        return sizeof(MassCancel);
      case 4:
        return sizeof(Quote);
      case 5:
        return sizeof(LastLookDecision);
      case 6:
        return sizeof(SetMark);
      case 7:
        return sizeof(ApplyFunding);
      case 8:
        return sizeof(AdminCmd);
      case 9:
        return sizeof(Deposit);
      case 10:
        return sizeof(Withdraw);
      case 11:
        return sizeof(ListInstrument);
      case 12:
        return sizeof(SetBands);
      case 13:
        return sizeof(TimeTick);
      case 14:
        return sizeof(SetTriggerRef);
      case 15:
        return sizeof(SnapshotBegin);
      case 16:
        return sizeof(RestoreOrder);
      case 17:
        return sizeof(RestoreStop);
      case 18:
        return sizeof(RestorePeg);
      case 19:
        return sizeof(RestoreHeld);
      case 20:
        return sizeof(RestorePosition);
      case 21:
        return sizeof(RestoreMmpCfg);
      case 22:
        return sizeof(RestoreClOrdIds);
      case 23:
        return sizeof(SnapshotEnd);
      case 24:
        return sizeof(RestoreReservation);
      case 25:
        return sizeof(RestoreBalance);
      case 26:
        return sizeof(RestoreMmpFills);
      case 27:
        return sizeof(SetStpGroup);
      case 28:
        return sizeof(SetFundingSchedule);
      case 29:
        return sizeof(RestoreFunding);
      case 30:
        return sizeof(ForceClosePosition);
      case 31:
        return sizeof(RestoreOrderStp);
      case 32:
        return sizeof(SetAdmissionProfile);
      case 33:
        return sizeof(SetRiskLimits);
      case 34:
        return sizeof(AdjustPosition);
      case 36:
        return sizeof(SetAccountRiskLimits);
      case 35:
        // The MAXIMUM, not the only length: a ladder record is as long as the
        // rungs it names (bodySizeAccepted below). Every caller that wants
        // "the size of the alternative that owns this tag" -- the fingerprint,
        // the decoder's upper bound -- wants this number.
        return sizeof(QuoteLadder);
      default:
        return 0;
    }
  }

  template <class T>
  static InboundCommand fromBody(const uint8_t* body)
  {
    T proto;
    std::memcpy(&proto, body, sizeof(T));
    return InboundCommand{proto};
  }

  // The ladder, whose record is as long as the rungs it carries. The unnamed
  // tail of the block never reached disk, so it is read back as what it was
  // when the record was written: zero. `proto` starts zeroed for exactly that
  // reason -- the memcpy covers `len` bytes and nothing else may be inherited
  // from the stack.
  static InboundCommand quoteLadderFromBody(const uint8_t* body, uint32_t len)
  {
    QuoteLadder proto{};
    std::memcpy(&proto, body, len);
    return InboundCommand{proto};
  }

  static void appendDecoded(std::vector<std::pair<int64_t, InboundCommand>>& v, int64_t ts,
                            uint8_t tag, const uint8_t* body, uint32_t len)
  {
    (void)len;
    switch (tag)
    {
      case 0:
        v.emplace_back(ts, fromBody<NewOrder>(body));
        break;
      case 1:
        v.emplace_back(ts, fromBody<CancelOrder>(body));
        break;
      case 2:
        v.emplace_back(ts, fromBody<ModifyOrder>(body));
        break;
      case 3:
        v.emplace_back(ts, fromBody<MassCancel>(body));
        break;
      case 4:
        v.emplace_back(ts, fromBody<Quote>(body));
        break;
      case 5:
        v.emplace_back(ts, fromBody<LastLookDecision>(body));
        break;
      case 6:
        v.emplace_back(ts, fromBody<SetMark>(body));
        break;
      case 7:
        v.emplace_back(ts, fromBody<ApplyFunding>(body));
        break;
      case 8:
        v.emplace_back(ts, fromBody<AdminCmd>(body));
        break;
      case 9:
        v.emplace_back(ts, fromBody<Deposit>(body));
        break;
      case 10:
        v.emplace_back(ts, fromBody<Withdraw>(body));
        break;
      case 11:
        v.emplace_back(ts, fromBody<ListInstrument>(body));
        break;
      case 12:
        v.emplace_back(ts, fromBody<SetBands>(body));
        break;
      case 13:
        v.emplace_back(ts, fromBody<TimeTick>(body));
        break;
      case 14:
        v.emplace_back(ts, fromBody<SetTriggerRef>(body));
        break;
      case 15:
        v.emplace_back(ts, fromBody<SnapshotBegin>(body));
        break;
      case 16:
        v.emplace_back(ts, fromBody<RestoreOrder>(body));
        break;
      case 17:
        v.emplace_back(ts, fromBody<RestoreStop>(body));
        break;
      case 18:
        v.emplace_back(ts, fromBody<RestorePeg>(body));
        break;
      case 19:
        v.emplace_back(ts, fromBody<RestoreHeld>(body));
        break;
      case 20:
        v.emplace_back(ts, fromBody<RestorePosition>(body));
        break;
      case 21:
        v.emplace_back(ts, fromBody<RestoreMmpCfg>(body));
        break;
      case 22:
        v.emplace_back(ts, fromBody<RestoreClOrdIds>(body));
        break;
      case 23:
        v.emplace_back(ts, fromBody<SnapshotEnd>(body));
        break;
      case 24:
        v.emplace_back(ts, fromBody<RestoreReservation>(body));
        break;
      case 25:
        v.emplace_back(ts, fromBody<RestoreBalance>(body));
        break;
      case 26:
        v.emplace_back(ts, fromBody<RestoreMmpFills>(body));
        break;
      case 27:
        v.emplace_back(ts, fromBody<SetStpGroup>(body));
        break;
      case 28:
        v.emplace_back(ts, fromBody<SetFundingSchedule>(body));
        break;
      case 29:
        v.emplace_back(ts, fromBody<RestoreFunding>(body));
        break;
      case 30:
        v.emplace_back(ts, fromBody<ForceClosePosition>(body));
        break;
      case 31:
        v.emplace_back(ts, fromBody<RestoreOrderStp>(body));
        break;
      case 32:
        v.emplace_back(ts, fromBody<SetAdmissionProfile>(body));
        break;
      case 33:
        v.emplace_back(ts, fromBody<SetRiskLimits>(body));
        break;
      case 34:
        v.emplace_back(ts, fromBody<AdjustPosition>(body));
        break;
      case 35:
        v.emplace_back(ts, quoteLadderFromBody(body, len));
        break;
      case 36:
        v.emplace_back(ts, fromBody<SetAccountRiskLimits>(body));
        break;
    }
  }

  // A file that is about to become this journal.
  //
  // A fresh file gets its first block here rather than on its first record.
  // The difference is not academic on the rotation path: reopen() runs inside
  // the checkpoint pause, with matching stopped, and the first append after it
  // runs on the matching path with an order waiting -- so the cheaper place to
  // pay is the one that is already stopped (flox::fileio::reserveFirstBlock).
  // A byte left behind by a failure there would sit in front of record 0, so a
  // failure is fatal to the open rather than ignored.
  static int openFile(const std::string& path, OpenMode mode)
  {
    const int fd = flox::fileio::openForWrite(
        path, mode == OpenMode::Truncate ? flox::fileio::OpenMode::Truncate
                                         : flox::fileio::OpenMode::Append);
    if (fd < 0)
    {
      throw std::runtime_error("Journal: cannot open '" + path + "' for writing");
    }
    if (mode == OpenMode::Truncate && !flox::fileio::reserveFirstBlock(fd))
    {
      flox::fileio::closeFd(fd);
      throw std::runtime_error("Journal: cannot prepare '" + path + "' for writing");
    }
    return fd;
  }

  void appendBytes(const void* p, size_t n)
  {
    const auto* b = static_cast<const uint8_t*>(p);
    rec_.insert(rec_.end(), b, b + n);
  }

  void writeAll(const uint8_t* p, size_t n)
  {
    if (!flox::fileio::writeAll(fd_, p, n))
    {
      throw std::runtime_error("Journal: write failed");
    }
  }

  int fd_{-1};
  Sync sync_{Sync::Off};
  bool dirty_{false};  // Group mode: records appended since the last sync()
  uint64_t syncs_{0};
  std::atomic<uint64_t> count_{0};
  std::atomic<uint64_t> bytes_{0};
  std::vector<uint8_t> rec_;
};

}  // namespace flox::venue
