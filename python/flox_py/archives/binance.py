"""Binance public archive → floxlog converter.

``data.binance.vision`` publishes daily aggTrades zip archives going
back 2+ years for spot, USDT-margined futures, and coin-margined
futures. The on-disk CSV layout is well-known and stable:

    agg_trade_id, price, quantity, first_trade_id, last_trade_id,
    transact_time_ms, is_buyer_maker, is_best_match

Some daily files prepend a header row; the reader skips it
transparently. Side encoding follows the existing floxlog convention:

  * ``is_buyer_maker = true``  → buyer rests as maker → active flow is
    selling → ``Side::SELL`` (1)
  * ``is_buyer_maker = false`` → seller rests as maker → active flow is
    buying  → ``Side::BUY``  (0)

The converter writes through ``flox_py.DataWriter`` and is append-safe:
re-running the same day is a no-op because dedup keys on
``agg_trade_id`` (stable, exchange-assigned).

A standalone ``metadata.json`` is written alongside the segments so the
result keys cleanly into ``MergedTapeReader`` against other floxlog
tapes — exchange = "binance", instrument_type derived from ``market``.
"""
from __future__ import annotations

import csv
import io
import json
import shutil
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from typing import Iterator, List, Optional, Sequence, Tuple, Union

# Market identifier → URL path prefix on data.binance.vision.
_MARKET_PATHS = {
    "spot": "data/spot/daily/aggTrades",
    "um-futures": "data/futures/um/daily/aggTrades",
    "cm-futures": "data/futures/cm/daily/aggTrades",
}
_BASE_URL = "https://data.binance.vision"

# Side values match flox::Side (BUY=0, SELL=1) as written by the
# in-engine recorder hook. Keep these locked to the C++ enum so the
# converter and the live recorder produce equivalent tapes.
_SIDE_BUY = 0
_SIDE_SELL = 1

# instrument_type stamped into metadata.json. Mirrors the wording
# the live binance connector uses so MergedTapeReader buckets converge.
_INSTRUMENT_TYPE = {
    "spot": "spot",
    "um-futures": "perpetual",
    "cm-futures": "perpetual",
}

DateLike = Union[str, date]


@dataclass
class ConvertStats:
    """Per-invocation summary returned by the converters."""

    trades_written: int = 0
    rows_read: int = 0
    rows_skipped: int = 0
    files_processed: int = 0
    first_ts_ns: int = 0
    last_ts_ns: int = 0
    last_trade_id: int = 0


# ── HTTP / mirror helpers ───────────────────────────────────────────


def _aggtrades_url(symbol: str, market: str, day: date) -> str:
    if market not in _MARKET_PATHS:
        raise ValueError(
            f"unknown market '{market}'. "
            f"expected one of: {sorted(_MARKET_PATHS)}"
        )
    prefix = _MARKET_PATHS[market]
    return f"{_BASE_URL}/{prefix}/{symbol}/{symbol}-aggTrades-{day.isoformat()}.zip"


def _mirror_path(mirror: Path, symbol: str, market: str, day: date) -> Path:
    return mirror / market / symbol / f"{symbol}-aggTrades-{day.isoformat()}.zip"


def _download(url: str, dest: Path, *, retries: int = 3, backoff: float = 2.0) -> None:
    """Download ``url`` to ``dest`` atomically with retry.

    Uses a ``.tmp`` sibling and ``rename`` so a partial fetch does not
    poison a long-running mirror.
    """
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".tmp")
    last_err: Optional[BaseException] = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(url, timeout=60) as resp, open(tmp, "wb") as f:
                shutil.copyfileobj(resp, f)
            tmp.replace(dest)
            return
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            last_err = e
            if tmp.exists():
                try:
                    tmp.unlink()
                except OSError:
                    pass
            if attempt < retries - 1:
                time.sleep(backoff * (2 ** attempt))
    raise RuntimeError(f"failed to download {url}: {last_err}") from last_err


# ── CSV parsing ─────────────────────────────────────────────────────


def _open_csv_member(path: Path) -> Tuple[io.TextIOBase, "object"]:
    """Open the single CSV member inside a Binance aggTrades zip (or a
    bare CSV file). Returns the text stream + a context object the
    caller must close."""
    if path.suffix.lower() == ".zip":
        zf = zipfile.ZipFile(path)
        names = [n for n in zf.namelist() if n.lower().endswith(".csv")]
        if not names:
            zf.close()
            raise ValueError(f"no CSV member inside {path}")
        raw = zf.open(names[0])
        return io.TextIOWrapper(raw, encoding="ascii", newline=""), zf
    fh = open(path, "r", encoding="ascii", newline="")
    return fh, fh


def _iter_rows(path: Path) -> Iterator[Tuple[int, float, float, int, bool]]:
    """Yield ``(agg_id, price, qty, ts_ms, is_buyer_maker)`` rows from a
    Binance aggTrades CSV (zipped or extracted). The first row is
    skipped transparently if it is a header."""
    stream, ctx = _open_csv_member(path)
    try:
        reader = csv.reader(stream)
        for row in reader:
            if not row:
                continue
            try:
                agg_id = int(row[0])
            except ValueError:
                # Header line such as
                # "agg_trade_id,price,quantity,first_trade_id,..."
                continue
            price = float(row[1])
            qty = float(row[2])
            ts_ms = int(row[5])
            maker = row[6].strip().lower() in ("true", "1")
            yield (agg_id, price, qty, ts_ms, maker)
    finally:
        try:
            ctx.close()
        except Exception:
            pass


# ── metadata.json writer ────────────────────────────────────────────


def _ns_to_iso(ns: int) -> str:
    if ns <= 0:
        return ""
    secs, rem = divmod(int(ns), 1_000_000_000)
    dt = datetime.fromtimestamp(secs, tz=timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{rem // 1_000_000:03d}Z"


def _base_quote(name: str) -> Tuple[str, str]:
    for quote in ("USDT", "USDC", "BUSD", "BTC", "ETH", "BNB"):
        if name.endswith(quote) and len(name) > len(quote):
            return name[: -len(quote)], quote
    return "", ""


def _write_metadata(
    out_tape: Path,
    *,
    exchange_name: str,
    market: str,
    symbol_id: int,
    symbol_name: str,
    first_ts_ns: int,
    last_ts_ns: int,
    trades_added: int,
) -> None:
    """Create or merge ``metadata.json`` so MergedTapeReader can key
    this tape by ``(exchange, name)``."""
    base, quote = _base_quote(symbol_name)
    sym_entry = {
        "symbol_id": int(symbol_id),
        "name": symbol_name,
        "base_asset": base,
        "quote_asset": quote,
        "price_precision": 8,
        "qty_precision": 8,
    }
    meta = {
        "recording_id": f"binance-{market}-{symbol_name}",
        "description": f"Binance {market} aggTrades archive for {symbol_name}",
        "exchange": exchange_name,
        "exchange_type": "cex",
        "instrument_type": _INSTRUMENT_TYPE.get(market, ""),
        "connector_version": "binance-archive-import",
        "symbols": [sym_entry],
        "has_trades": trades_added > 0,
        "has_book_snapshots": False,
        "has_book_deltas": False,
        "total_trades": int(trades_added),
        "total_book_updates": 0,
        "book_depth": 0,
        "recording_start": _ns_to_iso(first_ts_ns),
        "recording_end": _ns_to_iso(last_ts_ns),
        "price_scale": 100_000_000,
        "qty_scale": 100_000_000,
        "hostname": "",
        "timezone": "UTC",
        "flox_version": "",
        "custom": {},
    }

    meta_path = out_tape / "metadata.json"
    if meta_path.exists():
        try:
            existing = json.loads(meta_path.read_text())
            # Merge: widen time range, sum trades, dedup symbols.
            old_start = existing.get("recording_start") or meta["recording_start"]
            old_end = existing.get("recording_end") or meta["recording_end"]
            if meta["recording_start"] and (not old_start or meta["recording_start"] < old_start):
                old_start = meta["recording_start"]
            if meta["recording_end"] and meta["recording_end"] > old_end:
                old_end = meta["recording_end"]
            existing["recording_start"] = old_start
            existing["recording_end"] = old_end
            existing["total_trades"] = int(existing.get("total_trades", 0)) + int(trades_added)
            existing["has_trades"] = existing["total_trades"] > 0
            seen = {int(s["symbol_id"]): s for s in existing.get("symbols", [])}
            seen[int(symbol_id)] = sym_entry
            existing["symbols"] = list(seen.values())
            # Keep the rest of existing fields (exchange / exchange_type /
            # instrument_type) untouched if already set sensibly.
            for k in ("exchange", "exchange_type", "instrument_type",
                      "connector_version", "price_scale", "qty_scale",
                      "timezone"):
                if not existing.get(k):
                    existing[k] = meta[k]
            meta = existing
        except (OSError, ValueError, KeyError):
            pass

    out_tape.mkdir(parents=True, exist_ok=True)
    meta_path.write_text(json.dumps(meta, indent=2, sort_keys=True))


# ── Single-day conversion ───────────────────────────────────────────


def _existing_max_trade_id(out_tape: Path, symbol_id: int) -> int:
    """Return the highest existing trade_id for ``symbol_id`` in the
    tape, or 0 if the tape is empty / unreadable. Used for dedup so a
    re-run of the same day is a no-op."""
    if not out_tape.exists():
        return 0
    try:
        import flox_py
        import numpy as np

        reader = flox_py.DataReader(str(out_tape))
        trades = reader.read_trades()
        if trades.size == 0:
            return 0
        ids = np.asarray(trades["trade_id"])
        syms = np.asarray(trades["symbol_id"])
        mask = syms == symbol_id
        return int(ids[mask].max()) if mask.any() else 0
    except Exception:
        return 0


def aggtrades_to_floxlog(
    csv_path: Union[str, Path],
    out_tape: Union[str, Path],
    *,
    symbol_id: int = 1,
    symbol_name: str = "",
    market: str = "um-futures",
    exchange_id: int = 0,
    exchange_name: str = "binance",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    write_metadata: bool = True,
) -> ConvertStats:
    """Convert one Binance aggTrades day (zipped or extracted CSV) into
    a floxlog tape.

    ``symbol_name`` defaults to the file's basename leading token if
    omitted. Dedup keys on ``agg_trade_id`` so re-running a day already
    present in ``out_tape`` returns ``trades_written=0`` and updates
    only ``rows_skipped``.
    """
    import flox_py
    import numpy as np

    csv_path = Path(csv_path).expanduser()
    out_tape = Path(out_tape).expanduser()
    if not csv_path.exists():
        raise FileNotFoundError(csv_path)
    if not symbol_name:
        # "BTCUSDT-aggTrades-2024-01-15.zip" → "BTCUSDT"
        symbol_name = csv_path.stem.split("-", 1)[0]

    last_id = _existing_max_trade_id(out_tape, symbol_id) if append else 0

    # Stream-then-batch: the daily file is small enough (typically
    # <100 MB extracted) that one numpy round-trip beats the per-row
    # call overhead of write_trade.
    rows: List[Tuple[int, float, float, int, bool]] = []
    rows_read = 0
    rows_skipped = 0
    for r in _iter_rows(csv_path):
        rows_read += 1
        if r[0] <= last_id:
            rows_skipped += 1
            continue
        rows.append(r)

    if not rows:
        if write_metadata and rows_read:
            _write_metadata(
                out_tape,
                exchange_name=exchange_name,
                market=market,
                symbol_id=symbol_id,
                symbol_name=symbol_name,
                first_ts_ns=0,
                last_ts_ns=0,
                trades_added=0,
            )
        return ConvertStats(
            trades_written=0,
            rows_read=rows_read,
            rows_skipped=rows_skipped,
            files_processed=1,
            last_trade_id=last_id,
        )

    n = len(rows)
    agg_ids = np.fromiter((r[0] for r in rows), dtype=np.int64, count=n)
    prices = np.fromiter((r[1] for r in rows), dtype=np.float64, count=n)
    qty = np.fromiter((r[2] for r in rows), dtype=np.float64, count=n)
    ts_ms = np.fromiter((r[3] for r in rows), dtype=np.int64, count=n)
    maker = np.fromiter((1 if r[4] else 0 for r in rows), dtype=np.uint8, count=n)
    exchange_ts_ns = ts_ms * 1_000_000
    sides = np.where(maker.astype(bool), _SIDE_SELL, _SIDE_BUY).astype(np.uint8)
    trade_ids = agg_ids.astype(np.uint64)
    sym_ids = np.full(n, int(symbol_id), dtype=np.uint32)

    out_tape.mkdir(parents=True, exist_ok=True)
    writer = flox_py.DataWriter(
        str(out_tape),
        max_segment_mb=int(max_segment_mb),
        exchange_id=int(exchange_id),
        compression=compression,
    )
    try:
        n_written = writer.write_trades(
            exchange_ts_ns=exchange_ts_ns,
            recv_ts_ns=exchange_ts_ns,
            prices=prices,
            quantities=qty,
            trade_ids=trade_ids,
            symbol_ids=sym_ids,
            sides=sides,
        )
    finally:
        writer.close()

    first_ts = int(exchange_ts_ns[0])
    last_ts = int(exchange_ts_ns[-1])

    if write_metadata:
        _write_metadata(
            out_tape,
            exchange_name=exchange_name,
            market=market,
            symbol_id=symbol_id,
            symbol_name=symbol_name,
            first_ts_ns=first_ts,
            last_ts_ns=last_ts,
            trades_added=int(n_written),
        )

    return ConvertStats(
        trades_written=int(n_written),
        rows_read=rows_read,
        rows_skipped=rows_skipped,
        files_processed=1,
        first_ts_ns=first_ts,
        last_ts_ns=last_ts,
        last_trade_id=int(trade_ids[-1]),
    )


# ── Multi-day conversion ────────────────────────────────────────────


def _coerce_day(d: DateLike) -> date:
    return d if isinstance(d, date) else date.fromisoformat(d)


def _iter_days(date_from: DateLike, date_to: DateLike) -> Iterator[date]:
    cur = _coerce_day(date_from)
    end = _coerce_day(date_to)
    while cur <= end:
        yield cur
        cur += timedelta(days=1)


def range_to_floxlog(
    symbol: str,
    market: str,
    date_from: DateLike,
    date_to: DateLike,
    out_tape: Union[str, Path],
    *,
    mirror: Optional[Union[str, Path]] = None,
    parallel: int = 4,
    symbol_id: int = 1,
    exchange_id: int = 0,
    exchange_name: str = "binance",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    skip_missing: bool = False,
) -> ConvertStats:
    """Convert a date range from a local mirror or HTTP into floxlog.

    With ``mirror`` set, missing zips are downloaded into the mirror
    layout (``<mirror>/<market>/<symbol>/<symbol>-aggTrades-<YYYY-MM-DD>.zip``)
    so a follow-up call reuses the cached files. Without ``mirror``, a
    tempdir is used and discarded.

    Downloads run up to ``parallel`` connections in flight; the
    floxlog write step is serial so the writer stays append-safe.
    """
    days = list(_iter_days(date_from, date_to))
    if not days:
        return ConvertStats()

    cleanup = False
    if mirror is None:
        mirror = Path(tempfile.mkdtemp(prefix="binance_archive_"))
        cleanup = True
    mirror = Path(mirror).expanduser()

    def _ensure(day: date) -> Tuple[date, Optional[Path]]:
        local = _mirror_path(mirror, symbol, market, day)
        if local.exists():
            return day, local
        try:
            _download(_aggtrades_url(symbol, market, day), local)
        except Exception:
            if skip_missing:
                return day, None
            raise
        return day, local

    pairs: List[Tuple[date, Optional[Path]]] = []
    with ThreadPoolExecutor(max_workers=max(1, int(parallel))) as ex:
        for pair in ex.map(_ensure, days):
            pairs.append(pair)

    agg = ConvertStats()
    for day, path in pairs:
        if path is None:
            continue
        s = aggtrades_to_floxlog(
            path,
            out_tape,
            symbol_id=symbol_id,
            symbol_name=symbol,
            market=market,
            exchange_id=exchange_id,
            exchange_name=exchange_name,
            append=append,
            max_segment_mb=max_segment_mb,
            compression=compression,
            # Skip per-day metadata; one final write at the end captures
            # the full range range and total count.
            write_metadata=False,
        )
        agg.trades_written += s.trades_written
        agg.rows_read += s.rows_read
        agg.rows_skipped += s.rows_skipped
        agg.files_processed += s.files_processed
        if s.first_ts_ns and (agg.first_ts_ns == 0 or s.first_ts_ns < agg.first_ts_ns):
            agg.first_ts_ns = s.first_ts_ns
        if s.last_ts_ns > agg.last_ts_ns:
            agg.last_ts_ns = s.last_ts_ns
        if s.last_trade_id > agg.last_trade_id:
            agg.last_trade_id = s.last_trade_id

    _write_metadata(
        Path(out_tape),
        exchange_name=exchange_name,
        market=market,
        symbol_id=symbol_id,
        symbol_name=symbol,
        first_ts_ns=agg.first_ts_ns,
        last_ts_ns=agg.last_ts_ns,
        trades_added=agg.trades_written,
    )

    if cleanup:
        shutil.rmtree(mirror, ignore_errors=True)

    return agg


__all__ = [
    "ConvertStats",
    "aggtrades_to_floxlog",
    "range_to_floxlog",
]
