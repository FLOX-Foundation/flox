# `.floxlog` binary tape format specification

**Version 1.0, frozen 2026-05-08.** This is the on-disk format flox uses for deterministic event capture and replay. The format is published so third-party tooling can read and write tapes without depending on flox itself.

## At a glance

A `.floxlog` is a directory of segment files plus a manifest. Segments hold trades and order-book updates as length-prefixed CRC-checked frames, optionally LZ4-compressed in fixed-size blocks. Each segment can carry a sparse index for seek. Timestamps are integer nanoseconds since Unix epoch; prices and quantities are int64 fixed-point with a scale of `1e8`.

Everything is little-endian. All structures are 8-byte aligned. CRC32 uses the standard reflected polynomial `0xEDB88320` (ISO 3309).

## Layout on disk

```
my-tape.floxlog/
├── manifest.json
├── trades-000000.bin
├── trades-000001.bin
├── book-000000.bin
└── ...
```

`manifest.json` lists segment files, their byte counts, and the time range they cover. Segments are independently parseable; the manifest is an index, not a requirement.

## Magic numbers and constants

| Constant | Value | Meaning |
|---|---|---|
| `MAGIC_SEGMENT` | `0x584F4C46` (`"FLOX"`) | Segment header sentinel. |
| `MAGIC_BLOCK` | `0x4B4C4246` (`"FBLK"`) | Compressed block header sentinel. |
| `MAGIC_INDEX` | `0x58444E49` (`"INDX"`) | Sparse index header sentinel. |
| `FORMAT_VERSION` | `1` | Bump triggers a new `.floxlog` major. |
| `INDEX_VERSION` | `1` | Bump triggers a new index format minor. |

## Segment file

A segment is a `SegmentHeader` followed by a stream of frames. If `Compressed` is set, the stream is partitioned into `CompressedBlock`s, each holding LZ4-compressed frame bytes plus an inline header. If `HasIndex` is set, the index trailer lives at `index_offset` from the file start.

### `SegmentHeader` (64 bytes, 8-byte aligned)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0  | 4 | `magic` | `MAGIC_SEGMENT`. |
| 4  | 2 | `version` | `1`. |
| 6  | 1 | `flags` | Bitfield. See below. |
| 7  | 1 | `exchange_id` | Numeric exchange tag; `0` if not used. |
| 8  | 8 | `created_ns` | Wall-clock nanoseconds when the segment was opened. |
| 16 | 8 | `first_event_ns` | Earliest event timestamp in segment. |
| 24 | 8 | `last_event_ns` | Latest event timestamp. |
| 32 | 4 | `event_count` | Total events written. |
| 36 | 4 | `symbol_count` | Distinct symbols seen. |
| 40 | 8 | `index_offset` | Byte offset of the index trailer; `0` if absent. |
| 48 | 1 | `compression` | `0` = none, `1` = LZ4. |
| 49 | 15 | `reserved[15]` | Zero-filled. Bumping any reserved byte requires a version bump. |

### `SegmentFlags`

| Bit | Name | Meaning |
|---:|---|---|
| `0x01` | `HasIndex` | Sparse index trailer present. |
| `0x02` | `Compressed` | Frame stream is partitioned into `CompressedBlock`s. |
| `0x04` | `Encrypted` | Reserved. Not used by flox 1.0. |
| `0x08` | `Sorted` | Writer guarantees `exchange_ts_ns` is monotonically non-decreasing across all events in the segment. |

A reader that sees an unknown flag set must reject the segment with a clear error. New flags need a new minor version of the format.

### Frame stream

A frame is a `FrameHeader` followed by `size` bytes of payload. Payload meaning depends on `type`:

- `type = 1` → `TradeRecord`
- `type = 2` → `BookRecordHeader` followed by `bid_count + ask_count` `BookLevel` entries (book snapshot)
- `type = 3` → same shape as `type = 2`, but the levels are deltas (positive qty = upsert, qty == 0 = remove)

If `Compressed` is set, the entire frame stream from the byte after the segment header lives inside one or more `CompressedBlock`s.

### `FrameHeader` (12 bytes)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `size` | Payload bytes, excluding this header. |
| 4 | 4 | `crc32` | CRC32 of the payload bytes (header excluded). |
| 8 | 1 | `type` | `1` Trade, `2` BookSnapshot, `3` BookDelta. |
| 9 | 1 | `rec_version` | Per-record version. `1` for the layouts below. |
| 10 | 2 | `flags` | Reserved. Must be zero. |

A reader that sees an unknown `rec_version` must reject the frame.

### `TradeRecord` (48 bytes, 8-byte aligned)

| Offset | Size | Field | Units |
|---:|---:|---|---|
| 0  | 8 | `exchange_ts_ns` | Exchange-side timestamp, integer nanoseconds. |
| 8  | 8 | `recv_ts_ns` | Receiver-side wall-clock timestamp at frame write. |
| 16 | 8 | `price_raw` | Fixed-point price, scale `1e8`. |
| 24 | 8 | `qty_raw` | Fixed-point quantity, scale `1e8`. |
| 32 | 8 | `trade_id` | Exchange trade id; `0` if unknown. |
| 40 | 4 | `symbol_id` | Numeric symbol tag from the writer's registry. |
| 44 | 1 | `side` | `0` buy, `1` sell. |
| 45 | 1 | `instrument` | `0` spot, `1` perp, etc. (see Instrument codes). |
| 46 | 2 | `exchange_id` | Numeric exchange tag. |

### `BookRecordHeader` (40 bytes, 8-byte aligned)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0  | 8 | `exchange_ts_ns` | |
| 8  | 8 | `recv_ts_ns` | |
| 16 | 8 | `seq` | Monotonic sequence number from the source. `0` if not provided. |
| 24 | 4 | `symbol_id` | |
| 28 | 2 | `bid_count` | Number of `BookLevel` entries that follow on the bid side. |
| 30 | 2 | `ask_count` | Number of `BookLevel` entries on the ask side. |
| 32 | 1 | `type` | `2` snapshot, `3` delta. Mirrors the frame type. |
| 33 | 1 | `instrument` | |
| 34 | 2 | `exchange_id` | |
| 36 | 4 | `_pad` | Zero. |

Followed by `bid_count + ask_count` `BookLevel` entries, bids first.

### `BookLevel` (16 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `price_raw` |
| 8 | 8 | `qty_raw` |

Snapshot semantics: the listed levels replace the entire side. Delta semantics: positive `qty_raw` upserts a price level; `qty_raw == 0` removes the level.

### Compressed block (when `flags.Compressed` is set)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0  | 4 | `magic` | `MAGIC_BLOCK`. |
| 4  | 4 | `compressed_size` | Bytes of LZ4 payload that follow this header. |
| 8  | 4 | `original_size` | Decompressed size in bytes. |
| 12 | 2 | `event_count` | Number of frames packed into this block. |
| 14 | 2 | `flags` | Reserved. |

The block header is 16 bytes; the payload that follows is exactly `compressed_size` LZ4-compressed bytes which decompress to `original_size` bytes of frame stream. Blocks are concatenated until end of segment (or until `index_offset` if present).

LZ4 frame format: raw block, no LZ4 frame wrapper. Use the LZ4 block API directly.

### Sparse index (when `flags.HasIndex` is set)

The trailer at `index_offset`:

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0  | 4 | `magic` | `MAGIC_INDEX`. |
| 4  | 2 | `version` | `1`. |
| 6  | 2 | `interval` | Spacing between consecutive entries. Reserved as a hint; flox writes `0` for now. |
| 8  | 4 | `entry_count` | Number of `IndexEntry` rows that follow. |
| 12 | 4 | `crc32` | CRC32 over the entries section. |
| 16 | 8 | `first_ts_ns` | First indexed timestamp. |
| 24 | 8 | `last_ts_ns` | Last indexed timestamp. |

Followed by `entry_count` `IndexEntry` rows.

### `IndexEntry` (16 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `timestamp_ns` |
| 8 | 8 | `file_offset` |

`file_offset` points at the start of a `FrameHeader` (or, for compressed segments, the start of a `CompressedBlock`). Indexes are sparse: a reader that wants timestamp `T` finds the largest indexed entry with `timestamp_ns <= T`, seeks there, and scans forward.

## Manifest

`manifest.json` is one JSON object:

```json
{
  "schema_version": 1,
  "format_version": 1,
  "exchange_id": 0,
  "created_ns": 1714123456000000000,
  "segments": [
    {
      "name": "trades-000000.bin",
      "type": "trades",
      "size_bytes": 1048576,
      "first_event_ns": 1714123456000000000,
      "last_event_ns": 1714123459000000000,
      "event_count": 50000
    }
  ]
}
```

`schema_version` covers the manifest itself; `format_version` covers the segment binary layout. Mismatched `format_version` requires the reader to fail loudly. Mismatched `schema_version` is grounds for rejection unless documented otherwise.

## Instrument codes

| Code | Meaning |
|---:|---|
| 0 | spot |
| 1 | perp |
| 2 | future |
| 3 | option |
| 4-255 | reserved |

## Numeric conventions

- All multi-byte integers are **little-endian**.
- `price_raw` and `qty_raw` are signed int64 fixed-point with **scale 1e8**. To convert to a double price, divide by `1e8`. To go from a double to fixed-point, round to nearest then clamp to int64 range. Negative values are valid (used for short-side qty in some derived analytics, never in raw recorded fields).
- Timestamps are **integer nanoseconds since Unix epoch (UTC)**. No leap-second adjustment. Writers should pull from `clock_gettime(CLOCK_REALTIME)` or its OS equivalent.

## Versioning policy

- `format_version` follows semantic versioning at the major level. A new major is incompatible. Old readers of a new tape must reject with a clear error.
- New `flags` bits, new `EventType` codes, new `Instrument` codes, and new fields in reserved space all require a new major.
- Adding new `rec_version` values for a single record type within the same `format_version` is allowed when the record's size grows monotonically and the new version stays parseable as a prefix of the new layout. This path is rarely worth the complexity; prefer a new major.

Version 1.0 is **frozen** as of this document. Any change to the layout described here ships as version 2.0 with migration guidance in the corresponding spec revision.

## Reference implementations

- **C++.** `include/flox/replay/binary_format_v1.h` is the canonical layout. `src/replay/binary_log_writer.cpp` and `src/replay/binary_log_reader.cpp` implement read and write.
- **Python.** `flox_py.DataWriter` and `flox_py.DataReader` are pybind11 bindings of the C++ writer and reader. They are the simplest entry point for tooling outside flox.
- **Round-trip exercise.** `docs/examples/python_tape_roundtrip.py` writes synthetic trades through `flox_py.DataWriter`, reads them back through `flox_py.DataReader`, and asserts byte-equality field by field. CI runs it on every push, so the reference impl never silently drifts from this spec.

## Worked example: a 7-trade tape

The replay-equivalence CI gate (`scripts/replay_equivalence_gate.py`) writes a fixed 7-trade tape and asserts the captured replay output is byte-equal to a frozen JSON. The tape generator (`tests/replay-equivalence/build_tape.py`) is a 30-line example of how to build a tape from scratch using the Python reference encoder. Read it before writing your own implementation; everything important about the writer's calling convention is there.

## Compatibility commitments

- Files written by `flox-py` 0.5.x and later remain readable by every flox 1.x release.
- A reader for an older flox must refuse to load a newer tape rather than skip frames it does not understand.
- New optional flags and codes must be additive. Writers can elect to set them; older readers must reject the segment when an unknown flag is set, but compliant 1.0-only writers must not emit anything outside this document.

## Reporting issues

Bugs in this spec or in the reference implementation: open an issue on the public repository with the byte ranges that failed to parse and the sha256 of the offending segment. Include the writer version and any LZ4 library version if compression was used.
