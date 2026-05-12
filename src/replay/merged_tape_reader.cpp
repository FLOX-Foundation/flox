/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/merged_tape_reader.h"

#include "flox/error/flox_error.h"
#include "flox/replay/readers/binary_log_reader.h"

#include <algorithm>
#include <memory>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace flox::replay
{

namespace
{

ReaderConfig makeReaderConfig(const std::filesystem::path& dir,
                              const std::optional<int64_t>& from_ns,
                              const std::optional<int64_t>& to_ns)
{
  ReaderConfig rc{};
  rc.data_dir = dir;
  rc.from_ns = from_ns;
  rc.to_ns = to_ns;
  return rc;
}

}  // namespace

MergedTapeReader::MergedTapeReader(MergedTapeReaderConfig config)
    : _config(std::move(config))
{
  if (_config.tape_dirs.empty())
  {
    throw flox::FloxError("E_INPUT_001",
                          "MergedTapeReader: paths list is empty");
  }
  loadManifests();
  detectBookOverlap();
}

void MergedTapeReader::loadManifests()
{
  _manifests.reserve(_config.tape_dirs.size());
  _local_to_global.resize(_config.tape_dirs.size());
  _per_tape_stats.reserve(_config.tape_dirs.size());
  _inspect_total_events.reserve(_config.tape_dirs.size());

  std::unordered_map<std::string, uint32_t> key_to_global;
  auto canon_key = [](const std::string& exchange, const std::string& name)
  {
    return exchange + "\x1f" + name;  // unit-separator, can't appear in either
  };

  uint32_t next_global = 1;
  int64_t agg_first = 0;
  int64_t agg_last = 0;
  bool any_seen = false;

  for (size_t i = 0; i < _config.tape_dirs.size(); ++i)
  {
    const auto& dir = _config.tape_dirs[i];
    auto meta_path = RecordingMetadata::metadataPath(dir);
    auto loaded = RecordingMetadata::load(meta_path);
    if (!loaded)
    {
      throw flox::FloxError(
          "E_INPUT_002",
          "MergedTapeReader: cannot load metadata.json from " + dir.string());
    }
    _manifests.push_back(*std::move(loaded));
    const auto& meta = _manifests.back();

    // Build local→global rekey table. Local ids may be sparse, so size to
    // max+1 and use -1 sentinel.
    uint32_t max_local = 0;
    for (const auto& s : meta.symbols)
    {
      max_local = std::max(max_local, s.symbol_id);
    }
    auto& table = _local_to_global[i];
    table.assign(static_cast<size_t>(max_local) + 1, -1);

    for (const auto& s : meta.symbols)
    {
      auto key = canon_key(meta.exchange, s.name);
      auto it = key_to_global.find(key);
      uint32_t global_id;
      if (it == key_to_global.end())
      {
        global_id = next_global++;
        key_to_global[key] = global_id;
        MergedSymbol sym{};
        sym.global_id = global_id;
        sym.exchange = meta.exchange;
        sym.name = s.name;
        sym.price_precision = s.price_precision;
        sym.qty_precision = s.qty_precision;
        _symbols.push_back(std::move(sym));
      }
      else
      {
        global_id = it->second;
        // Precision conflict on same (exchange, name) — data quality issue.
        auto& existing = _symbols[global_id - 1];
        if (existing.price_precision != s.price_precision ||
            existing.qty_precision != s.qty_precision)
        {
          std::ostringstream oss;
          oss << "MergedTapeReader: precision mismatch for (" << meta.exchange
              << ", " << s.name << "): tape "
              << _config.tape_dirs[i].string() << " has price_precision="
              << int(s.price_precision) << " qty_precision="
              << int(s.qty_precision) << " but global registry has "
              << "price_precision=" << int(existing.price_precision)
              << " qty_precision=" << int(existing.qty_precision);
          throw flox::FloxError("E_INPUT_003", oss.str());
        }
      }
      table[s.symbol_id] = global_id;
    }

    // Stat the tape for time range + counts via a summary scan.
    auto summary = BinaryLogReader::inspect(dir);
    PerTapeStats st{};
    st.path = dir;
    st.first_event_ns = summary.first_event_ns;
    st.last_event_ns = summary.last_event_ns;
    _per_tape_stats.push_back(st);
    _inspect_total_events.push_back(summary.total_events);

    if (summary.total_events > 0)
    {
      if (!any_seen)
      {
        agg_first = summary.first_event_ns;
        agg_last = summary.last_event_ns;
        any_seen = true;
      }
      else
      {
        agg_first = std::min(agg_first, summary.first_event_ns);
        agg_last = std::max(agg_last, summary.last_event_ns);
      }
    }
  }
  _time_range = {agg_first, agg_last};
}

void MergedTapeReader::detectBookOverlap()
{
  // For each global symbol that appears in more than one tape, if both
  // tapes carry books, their time ranges must not overlap. Trades are
  // permitted to overlap (faithful replay of what each writer saw).
  //
  // Heuristic: use the manifest's has_book_snapshots/deltas flag + the
  // tape's overall first/last event range. Per-symbol per-tape range
  // would need to scan, but tape-level coarse bound is enough to flag
  // the common case (two captures of the same venue/symbol).
  struct Slice
  {
    uint32_t tape_index;
    int64_t first;
    int64_t last;
    bool has_book;
  };
  std::unordered_map<uint32_t, std::vector<Slice>> per_global;
  for (size_t i = 0; i < _manifests.size(); ++i)
  {
    const auto& meta = _manifests[i];
    const bool has_book = meta.has_book_snapshots || meta.has_book_deltas;
    const auto& stats = _per_tape_stats[i];
    for (const auto& s : meta.symbols)
    {
      auto gid = globalIdForLocal(static_cast<uint32_t>(i), s.symbol_id);
      if (gid == 0)
      {
        continue;
      }
      per_global[gid].push_back(
          Slice{static_cast<uint32_t>(i), stats.first_event_ns,
                stats.last_event_ns, has_book});
    }
  }
  for (auto& [gid, slices] : per_global)
  {
    if (slices.size() < 2)
    {
      continue;
    }
    int n_book = 0;
    for (const auto& sl : slices)
    {
      if (sl.has_book)
      {
        ++n_book;
      }
    }
    if (n_book < 2)
    {
      continue;
    }
    // Check pairwise overlap among book-carrying slices.
    std::vector<Slice> book_slices;
    for (const auto& sl : slices)
    {
      if (sl.has_book)
      {
        book_slices.push_back(sl);
      }
    }
    std::sort(book_slices.begin(), book_slices.end(),
              [](const Slice& a, const Slice& b)
              { return a.first < b.first; });
    for (size_t i = 1; i < book_slices.size(); ++i)
    {
      if (book_slices[i].first <= book_slices[i - 1].last)
      {
        std::ostringstream oss;
        oss << "MergedTapeReader: overlapping book streams for global "
            << "symbol_id=" << gid << " (\""
            << _symbols[gid - 1].exchange << "/"
            << _symbols[gid - 1].name << "\"): tape "
            << _config.tape_dirs[book_slices[i - 1].tape_index].string()
            << " ["
            << book_slices[i - 1].first << ", "
            << book_slices[i - 1].last << "] overlaps tape "
            << _config.tape_dirs[book_slices[i].tape_index].string() << " ["
            << book_slices[i].first << ", " << book_slices[i].last << "]";
        throw OverlappingBookStreamError(oss.str());
      }
    }
  }
}

uint32_t MergedTapeReader::globalIdForLocal(uint32_t tape_index,
                                            uint32_t local_id) const
{
  const auto& table = _local_to_global[tape_index];
  if (local_id >= table.size())
  {
    return 0;
  }
  auto v = table[local_id];
  return v < 0 ? 0 : static_cast<uint32_t>(v);
}

bool MergedTapeReader::symbolPassesFilter(uint32_t global_id) const
{
  if (_config.symbol_filter.empty())
  {
    return true;
  }
  return std::find(_config.symbol_filter.begin(),
                   _config.symbol_filter.end(), global_id) !=
         _config.symbol_filter.end();
}

std::vector<MergedTradeRow> MergedTapeReader::readTrades()
{
  std::vector<MergedTradeRow> rows;
  for (size_t i = 0; i < _config.tape_dirs.size(); ++i)
  {
    BinaryLogReader reader(
        makeReaderConfig(_config.tape_dirs[i], _config.from_ns, _config.to_ns));
    reader.forEach(
        [&](const ReplayEvent& ev) -> bool
        {
          if (ev.type != EventType::Trade)
          {
            return true;
          }
          uint32_t gid = globalIdForLocal(static_cast<uint32_t>(i),
                                          ev.trade.symbol_id);
          if (gid == 0 || !symbolPassesFilter(gid))
          {
            return true;
          }
          MergedTradeRow row{};
          row.exchange_ts_ns = ev.trade.exchange_ts_ns;
          row.recv_ts_ns = ev.trade.recv_ts_ns;
          row.price_raw = ev.trade.price_raw;
          row.qty_raw = ev.trade.qty_raw;
          row.trade_id = ev.trade.trade_id;
          row.global_symbol_id = gid;
          row.tape_index = static_cast<uint32_t>(i);
          row.side = ev.trade.side;
          rows.push_back(row);
          ++_per_tape_stats[i].trades;
          return true;
        });
  }
  // Stable sort by (exchange_ts_ns, tape_index) — recv_ts within tape is
  // already monotonic; tape_index breaks ties across tapes deterministically.
  std::sort(rows.begin(), rows.end(),
            [](const MergedTradeRow& a, const MergedTradeRow& b)
            {
              if (a.exchange_ts_ns != b.exchange_ts_ns)
              {
                return a.exchange_ts_ns < b.exchange_ts_ns;
              }
              return a.tape_index < b.tape_index;
            });
  return rows;
}

std::pair<std::vector<MergedBookRow>, std::vector<BookLevel>>
MergedTapeReader::readBooks()
{
  // First collect per-tape book events with their bid/ask vectors,
  // then sort, then materialize the flat level array in sorted order.
  struct Pending
  {
    MergedBookRow row;
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
  };
  std::vector<Pending> pending;

  for (size_t i = 0; i < _config.tape_dirs.size(); ++i)
  {
    BinaryLogReader reader(
        makeReaderConfig(_config.tape_dirs[i], _config.from_ns, _config.to_ns));
    reader.forEach(
        [&](const ReplayEvent& ev) -> bool
        {
          if (ev.type != EventType::BookSnapshot &&
              ev.type != EventType::BookDelta)
          {
            return true;
          }
          uint32_t gid = globalIdForLocal(static_cast<uint32_t>(i),
                                          ev.book_header.symbol_id);
          if (gid == 0 || !symbolPassesFilter(gid))
          {
            return true;
          }
          Pending p{};
          p.row.exchange_ts_ns = ev.book_header.exchange_ts_ns;
          p.row.recv_ts_ns = ev.book_header.recv_ts_ns;
          p.row.seq = ev.book_header.seq;
          p.row.global_symbol_id = gid;
          p.row.tape_index = static_cast<uint32_t>(i);
          p.row.bid_count = ev.book_header.bid_count;
          p.row.ask_count = ev.book_header.ask_count;
          // Map writer's type byte (0=snap, 1=delta) onto reader's event
          // type convention (2=snap, 3=delta) to match
          // BinaryLogReader.read_book_updates output.
          p.row.event_type = (ev.type == EventType::BookSnapshot) ? 2 : 3;
          p.bids = ev.bids;
          p.asks = ev.asks;
          pending.push_back(std::move(p));
          ++_per_tape_stats[i].books;
          return true;
        });
  }

  std::sort(pending.begin(), pending.end(),
            [](const Pending& a, const Pending& b)
            {
              if (a.row.exchange_ts_ns != b.row.exchange_ts_ns)
              {
                return a.row.exchange_ts_ns < b.row.exchange_ts_ns;
              }
              return a.row.tape_index < b.row.tape_index;
            });

  std::vector<MergedBookRow> rows;
  std::vector<BookLevel> levels;
  rows.reserve(pending.size());
  uint64_t offset = 0;
  for (auto& p : pending)
  {
    p.row.level_offset = offset;
    for (const auto& b : p.bids)
    {
      levels.push_back(b);
    }
    for (const auto& a : p.asks)
    {
      levels.push_back(a);
    }
    offset += p.bids.size() + p.asks.size();
    rows.push_back(p.row);
  }
  return {std::move(rows), std::move(levels)};
}

namespace
{

// Walks one tape's segments in order via BinaryLogIterator, applying
// the merged reader's from_ns/to_ns filter. Cheaper than letting
// BinaryLogReader::forEach materialise everything per tape.
class TapeWalker
{
 public:
  TapeWalker(std::vector<SegmentInfo> segments,
             std::optional<int64_t> from_ns,
             std::optional<int64_t> to_ns)
      : _segments(std::move(segments)),
        _from_ns(from_ns ? *from_ns : std::numeric_limits<int64_t>::min()),
        _to_ns(to_ns ? *to_ns : std::numeric_limits<int64_t>::max())
  {
    advanceSegment();
  }

  bool next(ReplayEvent& out)
  {
    while (_iter)
    {
      if (_iter->next(out))
      {
        if (out.timestamp_ns < _from_ns)
        {
          continue;
        }
        if (out.timestamp_ns > _to_ns)
        {
          _iter.reset();
          _segment_idx = _segments.size();
          return false;
        }
        return true;
      }
      ++_segment_idx;
      advanceSegment();
    }
    return false;
  }

 private:
  void advanceSegment()
  {
    _iter.reset();
    while (_segment_idx < _segments.size())
    {
      const auto& seg = _segments[_segment_idx];
      // Skip segments entirely outside the time window.
      if (seg.last_event_ns < _from_ns)
      {
        ++_segment_idx;
        continue;
      }
      if (seg.first_event_ns > _to_ns)
      {
        return;  // done
      }
      _iter = std::make_unique<BinaryLogIterator>(seg.path);
      if (!_iter->isValid())
      {
        _iter.reset();
        ++_segment_idx;
        continue;
      }
      if (_from_ns > seg.first_event_ns && _iter->hasIndex())
      {
        _iter->seekToTimestamp(_from_ns);
      }
      else if (_from_ns > seg.first_event_ns)
      {
        // No index — try to load one; harmless if absent.
        _iter->loadIndex();
        _iter->seekToTimestamp(_from_ns);
      }
      return;
    }
  }

  std::vector<SegmentInfo> _segments;
  size_t _segment_idx{0};
  std::unique_ptr<BinaryLogIterator> _iter;
  int64_t _from_ns;
  int64_t _to_ns;
};

}  // namespace

bool MergedTapeReader::streamEvents(StreamCallback callback)
{
  if (!callback)
  {
    return false;
  }

  const size_t n_tapes = _config.tape_dirs.size();

  // One walker per tape (owns the segment chain + iterator). We need
  // stable storage for these; the heap stores tape_index ints.
  std::vector<std::unique_ptr<TapeWalker>> walkers;
  walkers.reserve(n_tapes);
  std::vector<ReplayEvent> heads(n_tapes);
  std::vector<bool> alive(n_tapes, false);

  for (size_t i = 0; i < n_tapes; ++i)
  {
    // Use BinaryLogReader to discover this tape's segments via the
    // same filter machinery as the eager path. We grab segment list
    // then hand it to TapeWalker which iterates without buffering.
    BinaryLogReader reader(
        makeReaderConfig(_config.tape_dirs[i], _config.from_ns, _config.to_ns));
    // segments() returns the cached vector — populate via summary().
    (void)reader.summary();
    auto seg_list = reader.segments();
    auto walker = std::make_unique<TapeWalker>(
        std::move(seg_list), _config.from_ns, _config.to_ns);
    if (walker->next(heads[i]))
    {
      alive[i] = true;
    }
    walkers.push_back(std::move(walker));
  }

  // Min-heap by (timestamp_ns, tape_index). priority_queue is a
  // max-heap, hence the > comparator.
  struct HeapItem
  {
    int64_t ts_ns;
    uint32_t tape_index;
  };
  auto cmp = [](const HeapItem& a, const HeapItem& b)
  {
    if (a.ts_ns != b.ts_ns)
    {
      return a.ts_ns > b.ts_ns;
    }
    return a.tape_index > b.tape_index;
  };
  std::priority_queue<HeapItem, std::vector<HeapItem>, decltype(cmp)> heap(cmp);
  for (size_t i = 0; i < n_tapes; ++i)
  {
    if (alive[i])
    {
      heap.push({heads[i].timestamp_ns, static_cast<uint32_t>(i)});
    }
  }

  while (!heap.empty())
  {
    auto top = heap.top();
    heap.pop();
    const uint32_t ti = top.tape_index;
    ReplayEvent& ev = heads[ti];

    // Apply symbol rekey + filter.
    bool emit = true;
    if (ev.type == EventType::Trade)
    {
      uint32_t gid = globalIdForLocal(ti, ev.trade.symbol_id);
      if (gid == 0 || !symbolPassesFilter(gid))
      {
        emit = false;
      }
      else
      {
        ev.trade.symbol_id = gid;
      }
    }
    else if (ev.type == EventType::BookSnapshot ||
             ev.type == EventType::BookDelta)
    {
      uint32_t gid = globalIdForLocal(ti, ev.book_header.symbol_id);
      if (gid == 0 || !symbolPassesFilter(gid))
      {
        emit = false;
      }
      else
      {
        ev.book_header.symbol_id = gid;
      }
    }

    if (emit)
    {
      if (!callback(ti, ev))
      {
        return false;  // caller aborted
      }
    }

    // Advance this tape's walker.
    if (walkers[ti]->next(heads[ti]))
    {
      heap.push({heads[ti].timestamp_ns, ti});
    }
  }
  return true;
}

namespace
{

// IMultiSegmentReader view over a MergedTapeReader. Used by
// BacktestRunner::runTapes to plug the merged stream into the
// existing single-reader engine pipeline.
class MergedAsMultiSegmentReader : public IMultiSegmentReader
{
 public:
  explicit MergedAsMultiSegmentReader(MergedTapeReader* parent) : _parent(parent)
  {
  }

  uint64_t forEach(EventCallback callback) override
  {
    uint64_t n = 0;
    _parent->streamEvents(
        [&](uint32_t, const ReplayEvent& ev) -> bool
        {
          ++n;
          return callback(ev);
        });
    return n;
  }

  uint64_t forEachFrom(int64_t /*start_ts_ns*/, EventCallback callback) override
  {
    // The from_ns filter on MergedTapeReader is set at construction;
    // a mid-stream re-seek would require re-opening the underlying
    // readers. For BacktestRunner this isn't exercised — `run` calls
    // `forEach` only.
    return forEach(std::move(callback));
  }

  const std::vector<SegmentInfo>& segments() const override
  {
    return _empty_segments;
  }
  uint64_t totalEvents() const override { return 0; }

 private:
  MergedTapeReader* _parent;
  std::vector<SegmentInfo> _empty_segments;
};

}  // namespace

std::unique_ptr<IMultiSegmentReader>
MergedTapeReader::asMultiSegmentReader()
{
  return std::make_unique<MergedAsMultiSegmentReader>(this);
}

MergedTapeReader::Summary MergedTapeReader::summary() const noexcept
{
  Summary s{};
  s.first_event_ns = _time_range.first;
  s.last_event_ns = _time_range.second;
  s.tape_count = static_cast<uint32_t>(_per_tape_stats.size());
  s.symbol_count = static_cast<uint32_t>(_symbols.size());
  // Prefer the actual contribution counts if `readTrades`/`readBooks`
  // already ran (post-filter, exact). Otherwise fall back to the
  // construction-time `BinaryLogReader::inspect` totals so freshly
  // built readers report a meaningful event count.
  uint64_t contributed = 0;
  for (const auto& t : _per_tape_stats)
  {
    contributed += t.trades + t.books;
  }
  if (contributed > 0)
  {
    s.total_events = contributed;
  }
  else
  {
    for (auto n : _inspect_total_events)
    {
      s.total_events += n;
    }
  }
  return s;
}

}  // namespace flox::replay
