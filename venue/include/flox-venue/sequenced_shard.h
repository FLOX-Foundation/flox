/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/journal.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/messages.h"

#include "flox/util/eventing/event_bus.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace flox::venue
{

// ---- ingress: normalized commands from every gateway ----
struct InboundCommandEvent;
struct ICommandListener
{
  virtual ~ICommandListener() = default;
  virtual void onCommand(const InboundCommandEvent& ev) = 0;
  // The ingress has been drained of what was available. Where a batched
  // durability barrier belongs: waiting past this point buys no more
  // amortisation and only adds latency.
  virtual void onBatchEnd() {}
};
struct InboundCommandEvent
{
  using Listener = ICommandListener;
  InboundCommand cmd{};
  uint64_t tickSequence = 0;  // gateway sequence number, stamped by the bus
};

// ---- outbound: engine events fanned out to exec-report / market-data / ... ----
struct EngineEventMsg;
struct IEngineEventListener
{
  virtual ~IEngineEventListener() = default;
  virtual void onEngineEvent(const EngineEventMsg& ev) = 0;
};
struct EngineEventMsg
{
  using Listener = IEngineEventListener;
  OutboundEvent event{};
  uint64_t tickSequence = 0;
};

}  // namespace flox::venue

namespace flox
{
template <>
struct EventDispatcher<flox::venue::InboundCommandEvent>
{
  static void dispatch(const flox::venue::InboundCommandEvent& ev, flox::venue::ICommandListener& l)
  {
    l.onCommand(ev);
  }
  static void endOfBatch(flox::venue::ICommandListener& l) { l.onBatchEnd(); }
};
template <>
struct EventDispatcher<flox::venue::EngineEventMsg>
{
  static void dispatch(const flox::venue::EngineEventMsg& ev, flox::venue::IEngineEventListener& l)
  {
    l.onEngineEvent(ev);
  }
};
}  // namespace flox

namespace flox::venue
{

// Checkpoint / journal-rotation policy of a SequencedShard.
struct CheckpointConfig
{
  // Auto-trigger thresholds on the CURRENT journal segment, checked by the
  // idle sweeper thread -- automatic checkpoints therefore require
  // idleSweepIntervalNs > 0 (checkpointNow() works regardless). 0 disables
  // the corresponding threshold.
  uint64_t maxSegmentRecords{1'000'000};
  uint64_t maxSegmentBytes{256ULL << 20};
  // Snapshot+journal generations kept on disk after a checkpoint (>= 1). The
  // pre-checkpoint single-file journal counts as the oldest generation and is
  // deleted once `retainGenerations` snapshot generations exist.
  int retainGenerations{2};
};

template <class Book = MatchingBook, size_t IngressCap = 1 << 16, size_t OutboundCap = 1 << 16>
class SequencedShard
{
 public:
  // ---- on-disk layout ----
  // Before the first checkpoint the shard journals into the single file the
  // caller named (`<base>`, the historical layout). Every checkpoint at
  // boundary ts atomically publishes `<base>.snapshot.<ts>` and rotates the
  // journal onto `<base>.journal.<ts>`; a journal segment's numeric suffix
  // names its base snapshot, so the naming convention IS the manifest (plus
  // per-record CRC) -- no separate SegmentHeader record or manifest file to
  // tear. Recovery replays the newest snapshot that validates end-to-end
  // (structure + stateHash) plus every segment with ts >= that snapshot's;
  // an invalid snapshot falls back a generation, and with no valid snapshot
  // at all recovery replays the legacy file plus all segments from scratch.
  static std::string snapshotPath(const std::string& base, int64_t ts)
  {
    return base + ".snapshot." + std::to_string(ts);
  }
  static std::string segmentPath(const std::string& base, int64_t ts)
  {
    return base + ".journal." + std::to_string(ts);
  }

  struct Generations
  {
    std::vector<int64_t> snapshots;  // ascending checkpoint ts
    std::vector<int64_t> segments;   // ascending checkpoint ts
  };

  static Generations scanGenerations(const std::string& base)
  {
    namespace fs = std::filesystem;
    Generations g;
    const fs::path bp(base);
    fs::path dir = bp.parent_path();
    if (dir.empty())
    {
      dir = ".";
    }
    const std::string snapPrefix = bp.filename().string() + ".snapshot.";
    const std::string segPrefix = bp.filename().string() + ".journal.";
    const auto tsOf = [](const std::string& name, const std::string& prefix) -> int64_t
    {
      if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
      {
        return -1;
      }
      int64_t v = 0;
      for (size_t i = prefix.size(); i < name.size(); ++i)
      {
        if (name[i] < '0' || name[i] > '9')
        {
          return -1;  // excludes ".tmp" staging files and foreign names
        }
        v = v * 10 + (name[i] - '0');
      }
      return v;
    };
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; ec == std::error_code{} && it != end;
         it.increment(ec))
    {
      const std::string name = it->path().filename().string();
      if (const int64_t ts = tsOf(name, snapPrefix); ts >= 0)
      {
        g.snapshots.push_back(ts);
      }
      else if (const int64_t ts = tsOf(name, segPrefix); ts >= 0)
      {
        g.segments.push_back(ts);
      }
    }
    std::sort(g.snapshots.begin(), g.snapshots.end());
    std::sort(g.segments.begin(), g.segments.end());
    return g;
  }

  using IngressBus = flox::EventBus<InboundCommandEvent, IngressCap, 4>;
  using OutboundBus = flox::EventBus<EngineEventMsg, OutboundCap, 8>;

  // Sequencer clock: nanosecond timestamps stamped on every accepted command,
  // journaled AND fed to the engine as the same value. Injectable so tests are
  // deterministic; defaults to system wall time.
  using TimeSource = std::function<int64_t()>;

  static int64_t systemNowNs()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  // journalSync defaults to Full: the shard WAL is fsync-durable before a
  // command is applied (production posture). Pass Journal::Sync::Off for
  // throughput-oriented runs where power-loss durability is not required.
  //
  // The journal opens in Append mode: a restart never erases the log it must
  // recover from. start() replays any existing records before serving.
  //
  // idleSweepIntervalNs > 0 arms the idle time sweeper: a background thread
  // that, while last-look holds are open, injects a TimeTick command through
  // the normal sequenced path at most once per interval. That is what expires
  // a hold on a QUIET symbol -- expiry otherwise runs only when traffic
  // arrives. Going through the command stream (journaled, timestamped by the
  // same sequencer clock) keeps replay deterministic: the sweep is a record
  // like any other. 0 = off (the historical behavior; tests drive time by
  // submitting commands or calling engine().tick()).
  SequencedShard(SymbolConfig cfg, const std::string& journalPath, Book book = Book{},
                 Journal::Sync journalSync = Journal::Sync::Full,
                 TimeSource clock = &SequencedShard::systemNowNs,
                 int64_t idleSweepIntervalNs = 0, CheckpointConfig checkpointCfg = {})
      : journalPath_(journalPath),
        cfg_(cfg),
        bookProto_(book),  // pristine (empty) copy: scratch snapshot validation clones it
        checkpointCfg_(checkpointCfg),
        // Append to the newest journal segment when checkpoint generations
        // exist, else to the legacy single file (historical layout).
        journal_(initialJournalPath(journalPath), journalSync, Journal::OpenMode::Append),
        consumer_(cfg, journal_, outbound_, std::move(book), clock, this),
        symbol_(cfg.id),
        sweepClock_(std::move(clock)),
        idleSweepNs_(idleSweepIntervalNs)
  {
    ingress_.enableDrainOnStop();
    outbound_.enableDrainOnStop();
    if (journalSync == Journal::Sync::Group)
    {
      consumer_.enableGroupCommit();
    }
  }

  ~SequencedShard()
  {
    stopIdleSweeper();
    waitCheckpointPublish();  // never destroy members under a live background writer
  }

  // Subscribe an outbound consumer (exec-report, market-data, ...). Must be
  // called before start().
  bool subscribeOutbound(IEngineEventListener* l, bool required = true)
  {
    return outbound_.subscribe(l, required);
  }

  // Checkpoint hook: runs on the consumer thread after each checkpoint
  // generation is published (snapshot renamed in, journal rotated), with the
  // boundary ts of the new generation. The gateway harness uses it to persist
  // session-layer sidecars at the same durability point -- e.g. the FIX
  // session sidecar (FixSessionSidecar::write to
  // FixSessionSidecar::pathFor(journal base), i.e. `<base>.fixsessions`).
  // Keep it fast: matching is paused for its duration. Set before start().
  void onCheckpoint(std::function<void(int64_t boundaryTs)> hook)
  {
    checkpointHook_ = std::move(hook);
  }

  void start()
  {
    // Recovery FIRST: newest valid snapshot + its tail segments (or, without
    // one, the full journal history) replayed into the engine before any new
    // command is served. Replayed events are not re-published outbound --
    // reconnecting clients reconcile via snapshots, not a re-broadcast of
    // history.
    recovered_ = recoverAll();
    outbound_.start();  // must be live before the matching thread publishes
    ingress_.subscribe(&consumer_, true);
    ingress_.start();
    if (idleSweepNs_ > 0)
    {
      startIdleSweeper();
    }
    ready_ = true;
  }

  // Recovery gate: callers hold traffic until ready(); recoveredCommands()
  // reports how much of the journal was replayed on start().
  bool ready() const noexcept { return ready_; }
  uint64_t recoveredCommands() const noexcept { return recovered_; }

  // The shard's engine, for wiring (setLedger, fees, MMP) before start() and
  // for off-hot-path queries (book, snapshots) after quiescence.
  MatchingEngine<Book>& engine() noexcept { return consumer_.engine(); }
  const MatchingEngine<Book>& engine() const noexcept { return consumer_.engine(); }

  // Producer side (gateway). Returns the ingress sequence number.
  int64_t submit(const InboundCommand& cmd) { return ingress_.publish(InboundCommandEvent{cmd}); }

  void flush()
  {
    ingress_.flush();
    outbound_.flush();
  }

  void stop()
  {
    stopIdleSweeper();
    ingress_.flush();
    ingress_.stop();
    outbound_.flush();
    outbound_.stop();
    waitCheckpointPublish();  // a checkpoint in flight publishes before shutdown
    journal_.flush();
  }

  uint64_t journaled() const noexcept { return journal_.count(); }

  // Durability barriers taken by the journal (Sync::Group). One per drained
  // ingress batch rather than one per command, which is the whole trade.
  uint64_t journalSyncs() const noexcept { return journal_.syncs(); }

  // Checkpoint on demand (the control-plane SnapshotNow verb): requests a
  // snapshot at the next command boundary on the consumer thread -- the only
  // point of natural quiescence -- and waits for it. The TimeTick nudge
  // guarantees a boundary even on a quiet symbol. Deliberately NOT a
  // journaled verb: a snapshot must never be replay-visible (the nudge itself
  // is an ordinary, harmless time sweep record). The consumer pause covers
  // only the state CLONE + journal rotation; this call additionally waits for
  // the background publish, so it still returns true only once the new
  // generation is on disk.
  bool checkpointNow()
  {
    if (!ready_)
    {
      return false;
    }
    const uint64_t before = checkpoints_.load(std::memory_order_acquire);
    // Drain FIRST: the request must be observed no earlier than the boundary
    // of the last command submitted before this call -- otherwise a lagging
    // consumer would snapshot an earlier boundary (correct but surprising).
    ingress_.flush();
    checkpointRequested_.store(true, std::memory_order_release);
    submit(InboundCommand{TimeTick{symbol_}});
    ingress_.flush();  // the boundary ran: clone taken, background publish spawned
    waitCheckpointPublish();
    return checkpoints_.load(std::memory_order_acquire) > before;
  }

  uint64_t checkpointsTaken() const noexcept
  {
    return checkpoints_.load(std::memory_order_acquire);
  }

  // Consumer-thread stall of the most recent checkpoint (state clone + journal
  // rotation; serialization runs in the background). Observability gauge.
  int64_t lastCheckpointPauseNs() const noexcept
  {
    return lastCheckpointPauseNs_.load(std::memory_order_acquire);
  }

  // Records recovery applied from a snapshot file (0 = no snapshot was
  // consumed: fresh start or full-history replay). Lets callers -- and tests
  // -- prove recovery really went through the snapshot path instead of
  // silently replaying the whole journal history.
  uint64_t recoveredFromSnapshotRecords() const noexcept { return recoveredSnapshot_; }

 private:
  class MatchingConsumer : public ICommandListener
  {
   public:
    MatchingConsumer(SymbolConfig cfg, Journal& journal, OutboundBus& out, Book book,
                     TimeSource clock, SequencedShard* owner)
        : journal_(journal),
          out_(out),
          clock_(std::move(clock)),
          owner_(owner),
          engine_(cfg, [this](const OutboundEvent& ev)
                  {
                    if (replaying_)
                    {
                      return;
                    }
                    if (staged_ != nullptr)
                    {
                      // Group commit: an event is a promise, and a promise
                      // made before the record behind it is durable is the
                      // promise this mode exists to keep. Held until the
                      // barrier at the end of the batch.
                      staged_->push_back(EngineEventMsg{ev});
                      return;
                    }
                    out_.publish(EngineEventMsg{ev}); }, std::move(book))
    {
    }

    // Replay the journal's intact prefix into the engine (suppressing outbound
    // re-publication) and seed the timestamp floor, so records appended after
    // recovery stay monotonic relative to the recovered stream.
    uint64_t recover(const std::string& path)
    {
      const auto records = Journal::loadTimed(path);
      replaying_ = true;
      for (const auto& [ts, cmd] : records)
      {
        engine_.submit(cmd, ts);
        if (ts > lastTs_)
        {
          lastTs_ = ts;
        }
      }
      replaying_ = false;
      return records.size();
    }

    // Apply a snapshot file the shard has ALREADY validated end-to-end into
    // the real engine (recovery path: outbound suppressed like journal
    // replay, timestamp floor seeded from the snapshot records).
    uint64_t applySnapshot(const std::string& path)
    {
      const auto records = Journal::loadTimed(path);
      replaying_ = true;
      for (const auto& [ts, cmd] : records)
      {
        engine_.applySnapshotRecord(cmd, ts);
        if (ts > lastTs_)
        {
          lastTs_ = ts;
        }
      }
      replaying_ = false;
      return records.size();
    }

    void onCommand(const InboundCommandEvent& ev) override
    {
      const int64_t ts = nextTs();
      journal_.append(ev.cmd, ts);  // write-ahead, before applying
      engine_.submit(ev.cmd, ts);   // the SAME timestamp the journal holds
      lastBatchTs_ = ts;
      if (staged_ == nullptr)
      {
        owner_->maybeCheckpoint(ts);  // command boundary: natural quiescence
      }
    }

    // The ingress is drained. Under group commit this is the durability
    // barrier: one fsync for the whole batch, and only then are the events it
    // produced allowed out. Under the other modes the records are already as
    // durable as they are going to get and nothing was staged, so this costs a
    // branch.
    void onBatchEnd() override
    {
      if (staged_ == nullptr)
      {
        return;
      }
      journal_.sync();
      for (const EngineEventMsg& m : *staged_)
      {
        out_.publish(m);
      }
      staged_->clear();
      // Checkpointing is deferred to here for the same reason: a snapshot
      // taken mid-batch would capture state whose journal records are not
      // durable yet.
      if (lastBatchTs_ != 0)
      {
        owner_->maybeCheckpoint(lastBatchTs_);
      }
    }

    // Called once at construction when the journal batches its barrier.
    void enableGroupCommit() { staged_ = &stagedStorage_; }

    MatchingEngine<Book>& engine() noexcept { return engine_; }
    const MatchingEngine<Book>& engine() const noexcept { return engine_; }

   private:
    // Strictly monotonic, non-zero sequencer time: a stalled or backwards
    // clock still yields lastTs_ + 1, so replay ordering is unambiguous.
    int64_t nextTs()
    {
      int64_t t = clock_();
      if (t <= lastTs_)
      {
        t = lastTs_ + 1;
      }
      lastTs_ = t;
      return t;
    }

    Journal& journal_;
    OutboundBus& out_;
    std::vector<EngineEventMsg> stagedStorage_;
    std::vector<EngineEventMsg>* staged_{nullptr};  // non-null = group commit
    int64_t lastBatchTs_{0};
    TimeSource clock_;
    SequencedShard* owner_;
    bool replaying_{false};
    int64_t lastTs_{0};
    MatchingEngine<Book> engine_;
  };

  // ---- checkpoint machinery (consumer thread unless noted) ----

  static std::string initialJournalPath(const std::string& base)
  {
    const Generations g = scanGenerations(base);
    return g.segments.empty() ? base : segmentPath(base, g.segments.back());
  }

  // Recovery: newest snapshot that validates end-to-end, plus every journal
  // segment at or after it; an invalid snapshot logs and falls back a
  // generation; no valid snapshot at all -> full-history replay (legacy
  // single file, then all segments in order).
  uint64_t recoverAll()
  {
    const Generations g = scanGenerations(journalPath_);
    int64_t chosen = -1;
    for (auto it = g.snapshots.rbegin(); it != g.snapshots.rend(); ++it)
    {
      if (validateSnapshot(snapshotPath(journalPath_, *it)))
      {
        chosen = *it;
        break;
      }
      std::fprintf(stderr, "flox-venue: WARN snapshot %s invalid, falling back a generation\n",
                   snapshotPath(journalPath_, *it).c_str());
    }
    uint64_t n = 0;
    if (chosen >= 0)
    {
      recoveredSnapshot_ = consumer_.applySnapshot(snapshotPath(journalPath_, chosen));
      n += recoveredSnapshot_;
      for (const int64_t ts : g.segments)
      {
        if (ts >= chosen)
        {
          n += consumer_.recover(segmentPath(journalPath_, ts));
        }
      }
      // Continue appending to the newest segment of the recovered history. A
      // crash between snapshot publish and segment rotation leaves a snapshot
      // with no segment of its own -- open (create) that segment now, so new
      // records never land in a segment older than the snapshot they follow.
      const int64_t cur =
          (!g.segments.empty() && g.segments.back() >= chosen) ? g.segments.back() : chosen;
      journal_.reopen(segmentPath(journalPath_, cur), Journal::OpenMode::Append);
      lastCheckpointTs_ = chosen;
    }
    else
    {
      // Falling back a generation is unbounded, but what pruning keeps is not.
      // Once no snapshot validates, the only thing covering history before the
      // first checkpoint is the pre-checkpoint journal -- and pruning deletes
      // it as soon as `retainGenerations` snapshots exist. Replaying what is
      // left would rebuild a state that looks plausible, is missing everything
      // before the oldest surviving segment, and would then be served. Refuse
      // instead: a venue that will not start is an incident, a venue that
      // starts on a truncated ledger is a disaster.
      const bool haveLegacy = std::filesystem::exists(journalPath_);
      if (!haveLegacy && (!g.snapshots.empty() || !g.segments.empty()))
      {
        throw std::runtime_error(
            "flox-venue: no snapshot validates and the pre-checkpoint journal " + journalPath_ +
            " is gone, so history cannot be replayed in full. Refusing to start on a "
            "truncated state -- restore a good snapshot generation from backup.");
      }
      if (!g.snapshots.empty())
      {
        std::fprintf(stderr, "flox-venue: WARN no valid snapshot for %s, full-history replay\n",
                     journalPath_.c_str());
      }
      n += consumer_.recover(journalPath_);  // legacy single file (absent -> 0 records)
      for (const int64_t ts : g.segments)
      {
        n += consumer_.recover(segmentPath(journalPath_, ts));
      }
    }
    return n;
  }

  // Scratch-validate a snapshot file end-to-end -- framing/CRC via loadTimed,
  // SnapshotBegin/SnapshotEnd structure (a torn tail always lacks the End
  // record, which is written last), every Restore* invariant, and the state
  // hash at SnapshotEnd -- against a throwaway engine + ledger. The real
  // engine is touched only after the whole file verified, so a corrupt
  // snapshot can never pollute it.
  bool validateSnapshot(const std::string& path)
  {
    const auto records = Journal::loadTimed(path);
    if (records.size() < 2 || !std::holds_alternative<SnapshotBegin>(records.front().second) ||
        !std::holds_alternative<SnapshotEnd>(records.back().second))
    {
      return false;
    }
    Ledger scratch;
    MatchingEngine<Book> probe(cfg_, [](const OutboundEvent&) {}, Book{bookProto_});
    if (consumer_.engine().ledger() != nullptr)
    {
      probe.setLedger(&scratch, consumer_.engine().venueAccount());
    }
    for (const auto& [ts, cmd] : records)
    {
      if (!probe.applySnapshotRecord(cmd, ts))
      {
        return false;
      }
    }
    return true;
  }

  // Consumer-thread entry at every command boundary (see onCommand).
  void maybeCheckpoint(int64_t boundaryTs)
  {
    if (!checkpointRequested_.load(std::memory_order_acquire))
    {
      return;
    }
    checkpointRequested_.store(false, std::memory_order_release);
    doCheckpoint(boundaryTs);
  }

  // Consumer thread only. ASYNCHRONOUS checkpoint: under the pause only the
  // engine state is CLONED (deep copy, ledger by value) and the journal is
  // rotated onto the segment named by the boundary ts -- the segment boundary
  // and the snapshot content are fixed at the same command boundary.
  // Serialization + fsync + rename (atomic publish) + retention pruning run on
  // a background thread against the clone, so matching resumes after an
  // O(clone) pause instead of O(serialize+fsync). fork()-based copy-on-write
  // was rejected: this process is multi-threaded (see the rationale at
  // MatchingEngine::cloneForSnapshot).
  //
  // Crash window: between rotation and the background rename the disk holds
  // "segment ts exists, snapshot ts absent (only a .tmp at most)". recoverAll
  // tolerates exactly that: the .tmp never parses as a generation, recovery
  // picks the PREVIOUS valid snapshot and replays BOTH tail segments (ts-1's
  // and ts's) after it, reproducing the state; the snapshot only ever appears
  // atomically via rename. A failed background publish logs a WARN and leaves
  // the same recoverable layout.
  void doCheckpoint(int64_t ts)
  {
    if (ts <= lastCheckpointTs_)
    {
      return;  // repeated request inside one boundary: state already on disk
    }
    // No snapshot queue: at most one background publish in flight -- a new
    // checkpoint first waits for the previous one to finish.
    waitCheckpointPublish();
    const auto pause0 = std::chrono::steady_clock::now();
    auto clone = consumer_.engine().cloneForSnapshot(Book{bookProto_});
    journal_.flush();
    journal_.reopen(segmentPath(journalPath_, ts), Journal::OpenMode::Truncate);
    syncDir();
    lastCheckpointTs_ = ts;
    if (checkpointHook_)
    {
      checkpointHook_(ts);  // sidecar persistence rides the same boundary (consumer thread)
    }
    lastCheckpointPauseNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - pause0)
                                     .count(),
                                 std::memory_order_release);
    auto publish = [this, ts, cl = std::move(clone)]() mutable -> bool
    {
      const std::string snap = snapshotPath(journalPath_, ts);
      const std::string tmp = snap + ".tmp";
      {
        Journal out(tmp, Journal::Sync::Off, Journal::OpenMode::Truncate);
        cl.engine->writeSnapshot(out);
        out.flush();  // durable BEFORE the rename publishes it
      }
      std::error_code ec;
      std::filesystem::rename(tmp, snap, ec);
      if (ec)
      {
        std::fprintf(stderr, "flox-venue: WARN checkpoint rename failed for %s: %s\n",
                     snap.c_str(), ec.message().c_str());
        return false;  // recovery falls back a generation (both segments replay)
      }
      syncDir();
      pruneGenerations();
      checkpoints_.fetch_add(1, std::memory_order_release);
      return true;
    };
    std::lock_guard<std::mutex> lk(ckptMx_);
    ckptPending_ = std::async(std::launch::async, std::move(publish)).share();
  }

  // Wait for the in-flight background snapshot publish, if any. Safe from any
  // thread; the consumer thread calls it before starting the next checkpoint.
  void waitCheckpointPublish()
  {
    std::shared_future<bool> f;
    {
      std::lock_guard<std::mutex> lk(ckptMx_);
      f = ckptPending_;
    }
    if (f.valid())
    {
      f.wait();
    }
  }

  void pruneGenerations()
  {
    namespace fs = std::filesystem;
    const Generations g = scanGenerations(journalPath_);
    const size_t retain =
        checkpointCfg_.retainGenerations < 1 ? 1 : static_cast<size_t>(checkpointCfg_.retainGenerations);
    std::error_code ec;
    if (g.snapshots.size() > retain)
    {
      const int64_t keepFrom = g.snapshots[g.snapshots.size() - retain];
      for (const int64_t ts : g.snapshots)
      {
        if (ts < keepFrom)
        {
          fs::remove(snapshotPath(journalPath_, ts), ec);
        }
      }
      for (const int64_t ts : g.segments)
      {
        if (ts < keepFrom)
        {
          fs::remove(segmentPath(journalPath_, ts), ec);
        }
      }
    }
    if (g.snapshots.size() >= retain)
    {
      fs::remove(journalPath_, ec);  // legacy pre-checkpoint file: oldest generation
    }
  }

  void syncDir() const
  {
    std::filesystem::path dir = std::filesystem::path(journalPath_).parent_path();
    const std::string d = dir.empty() ? "." : dir.string();
    const int fd = ::open(d.c_str(), O_RDONLY);
    if (fd >= 0)
    {
      ::fsync(fd);
      ::close(fd);
    }
  }

  // Idle sweeper: while holds are open, inject a TimeTick through the normal
  // ingress path at most once per idleSweepNs_ (per the injected TimeSource).
  // The consumer thread stamps and journals it like any other command, so the
  // sweep replays deterministically. The engine's openHolds() gauge keeps the
  // journal free of ticks when nothing is pending.
  void startIdleSweeper()
  {
    sweepStop_.store(false, std::memory_order_release);
    sweeper_ = std::thread(
        [this]
        {
          int64_t lastSweep = 0;
          while (!sweepStop_.load(std::memory_order_acquire))
          {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            // Checkpoint auto-trigger: the current segment crossed its
            // record/byte threshold. Only requests (exchange guards against
            // re-requesting); the snapshot itself runs on the consumer thread
            // at the next command boundary, and the TimeTick nudge guarantees
            // one on a quiet symbol.
            if (((checkpointCfg_.maxSegmentRecords > 0 &&
                  journal_.count() >= checkpointCfg_.maxSegmentRecords) ||
                 (checkpointCfg_.maxSegmentBytes > 0 &&
                  journal_.bytes() >= checkpointCfg_.maxSegmentBytes)) &&
                !checkpointRequested_.exchange(true, std::memory_order_acq_rel))
            {
              submit(InboundCommand{TimeTick{symbol_}});
            }
            if (consumer_.engine().openHolds() == 0)
            {
              continue;
            }
            const int64_t now = sweepClock_();
            if (now - lastSweep < idleSweepNs_)
            {
              continue;
            }
            lastSweep = now;
            submit(InboundCommand{TimeTick{symbol_}});
          }
        });
  }

  void stopIdleSweeper()
  {
    if (sweeper_.joinable())
    {
      sweepStop_.store(true, std::memory_order_release);
      sweeper_.join();
    }
  }

  std::string journalPath_;
  SymbolConfig cfg_;  // construction config (scratch validation engines)
  Book bookProto_;    // pristine copy of the ctor book, cloned per validation
  CheckpointConfig checkpointCfg_;
  Journal journal_;
  OutboundBus outbound_;
  MatchingConsumer consumer_;
  IngressBus ingress_;
  SymbolId symbol_{};
  TimeSource sweepClock_;
  int64_t idleSweepNs_{0};
  std::thread sweeper_;
  std::atomic<bool> sweepStop_{false};
  std::atomic<bool> checkpointRequested_{false};
  std::function<void(int64_t)> checkpointHook_;
  std::atomic<uint64_t> checkpoints_{0};
  int64_t lastCheckpointTs_{0};  // consumer thread (and pre-start recovery) only
  // Background snapshot publish: at most one in flight (doCheckpoint waits for
  // the previous). Guarded by ckptMx_ against checkpointNow()/stop() readers.
  std::mutex ckptMx_;
  std::shared_future<bool> ckptPending_;
  std::atomic<int64_t> lastCheckpointPauseNs_{0};
  uint64_t recovered_{0};
  uint64_t recoveredSnapshot_{0};
  bool ready_{false};
};

}  // namespace flox::venue
