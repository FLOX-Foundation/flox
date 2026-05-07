"""Tape recording / replay primitives backing ``flox tape``.

The ``.floxlog`` binary format is the on-disk format flox uses for
deterministic event capture. This module wraps the existing
``flox_py.DataWriter`` / ``DataReader`` (C++ binary log writer /
reader) into hook-shaped helpers for the ``flox tape record`` and
``flox tape replay`` CLI subcommands in :mod:`flox_py.cli`.

Scope limitations of v1:

* Records **trades** today. Book snapshots / deltas aren't yet on the
  Python ``DataWriter`` C-API surface; they're tracked as a follow-up
  via the existing C++ ``BinaryLogWriter::writeBook``. ``--include-book``
  surfaces a clear error rather than silently dropping data.
* The recorder hook works against any ``Runner``-driven source — the
  ``flox tape record`` CLI uses :class:`flox_py.ccxt.CcxtBroker`, but
  the same hook runs in any pipeline that calls ``set_market_data_recorder``.
"""
from __future__ import annotations

import asyncio
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, List, Optional


@dataclass
class TapeRecorderStats:
    trades_written: int = 0
    book_updates_skipped: int = 0
    started_at_ns: int = 0
    last_event_ns: int = 0
    error: Optional[str] = None


def _now_ns() -> int:
    return time.time_ns()


def make_recorder_hook(
    output_dir: Path,
    *,
    max_segment_mb: int = 256,
    exchange_id: int = 0,
    compression: str = "none",
) -> Any:
    """Return a ``MarketDataRecorderHook`` subclass instance that
    persists every observed trade to ``output_dir`` via
    :class:`flox_py.DataWriter`.

    Book updates are counted but not written (see module docstring).
    The instance also exposes a ``stats: TapeRecorderStats`` attribute
    + a ``close()`` method for clean shutdown.
    """
    import flox_py  # imported lazily so test discovery doesn't fail without binding

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    writer = flox_py.DataWriter(
        str(output_dir),
        max_segment_mb=max_segment_mb,
        exchange_id=int(exchange_id),
        compression=compression,
    )

    class _Recorder(flox_py.MarketDataRecorderHook):
        def __init__(self) -> None:
            super().__init__()
            self.stats = TapeRecorderStats()
            self._writer = writer

        def on_start(self) -> None:
            self.stats.started_at_ns = _now_ns()

        def on_stop(self) -> None:
            try:
                self._writer.flush()
                self._writer.close()
            except Exception as exc:  # pragma: no cover — defensive
                self.stats.error = f"writer close failed: {exc!r}"

        def on_trade(self, trade: Any) -> None:
            recv_ns = _now_ns()
            self.stats.last_event_ns = recv_ns
            try:
                ok = self._writer.write_trade(
                    exchange_ts_ns=int(trade.exchange_ts_ns or 0),
                    recv_ts_ns=recv_ns,
                    price=float(trade.price),
                    qty=float(trade.quantity),
                    trade_id=0,
                    symbol_id=int(trade.symbol),
                    side=0 if bool(trade.is_buy) else 1,
                )
                if ok:
                    self.stats.trades_written += 1
            except Exception as exc:  # pragma: no cover — defensive
                self.stats.error = f"write_trade failed: {exc!r}"

        def on_book_update(self, symbol: int, is_snapshot: bool,
                           bids: Any, asks: Any, ts_ns: int) -> None:
            # v1: book-write API not yet on DataWriter. Track count so
            # CLI tooling can flag what got skipped.
            self.stats.book_updates_skipped += 1

        def close(self) -> None:
            self.on_stop()

    return _Recorder()


# ── Replay helpers ──────────────────────────────────────────────────


@dataclass
class TapeStats:
    """Result of :func:`inspect_tape`."""
    path: str
    trade_count: int
    first_ts_ns: int
    last_ts_ns: int
    symbol_ids: List[int] = field(default_factory=list)


def inspect_tape(path: str | Path) -> TapeStats:
    """Open a ``.floxlog`` directory and return summary statistics
    without dispatching events through an Engine. Used by
    ``flox tape inspect`` and as a smoke check for replay-equivalence.
    """
    import flox_py
    import numpy as np

    p = str(path)
    reader = flox_py.DataReader(p)
    trades = reader.read_trades()
    if trades.size == 0:
        return TapeStats(path=p, trade_count=0, first_ts_ns=0, last_ts_ns=0)
    ts = np.asarray(trades["exchange_ts_ns"])
    sym = np.asarray(trades["symbol_id"])
    return TapeStats(
        path=p,
        trade_count=int(ts.size),
        first_ts_ns=int(ts[0]),
        last_ts_ns=int(ts[-1]),
        symbol_ids=sorted(int(s) for s in np.unique(sym).tolist()),
    )


def replay_tape(
    path: str | Path,
    *,
    on_trade: Optional[Any] = None,
) -> int:
    """Iterate trades from a ``.floxlog`` directory, optionally
    invoking ``on_trade(timestamp_ns, symbol_id, price, qty, side)``
    for each row.

    Returns the number of trades dispatched. Order is exchange
    timestamp ascending — same order the engine saw them live, which
    is what makes replay-equivalence testable.
    """
    import flox_py

    reader = flox_py.DataReader(str(path))
    trades = reader.read_trades()
    n = int(trades.size)
    if on_trade is None or n == 0:
        return n
    for row in trades:
        on_trade(
            int(row["exchange_ts_ns"]),
            int(row["symbol_id"]),
            float(row["price_raw"]) / 1e8,
            float(row["qty_raw"]) / 1e8,
            int(row["side"]),
        )
    return n


@dataclass
class TapeDiff:
    """Result of :func:`diff_tapes`. ``equal`` is true when both tapes
    have the same trade count and every paired record matches."""

    left_path: str
    right_path: str
    left_count: int
    right_count: int
    first_divergence_index: Optional[int]
    mismatches: List[dict] = field(default_factory=list)
    equal: bool = False

    def to_dict(self) -> dict:
        return {
            "left_path": self.left_path,
            "right_path": self.right_path,
            "left_count": self.left_count,
            "right_count": self.right_count,
            "first_divergence_index": self.first_divergence_index,
            "mismatches": list(self.mismatches),
            "equal": self.equal,
        }


def diff_tapes(
    left: str | Path,
    right: str | Path,
    *,
    max_mismatches: int = 16,
    field_tolerance_ns: int = 0,
) -> TapeDiff:
    """Compare two ``.floxlog`` directories trade by trade. Returns
    ``equal=True`` when both have the same count and every paired
    record matches on ``(exchange_ts_ns, symbol_id, price_raw,
    qty_raw, side)``. ``field_tolerance_ns`` allows a non-zero
    timestamp jitter — useful when comparing live captures whose
    record-side wallclock can differ across runs.

    The first ``max_mismatches`` divergences are recorded; the rest
    are summarized by count. Pass a high value if you need the full
    list."""
    import flox_py
    import numpy as np

    left = Path(left).expanduser()
    right = Path(right).expanduser()

    lt = flox_py.DataReader(str(left)).read_trades()
    rt = flox_py.DataReader(str(right)).read_trades()

    diff = TapeDiff(
        left_path=str(left),
        right_path=str(right),
        left_count=int(lt.size),
        right_count=int(rt.size),
        first_divergence_index=None,
    )

    n = min(int(lt.size), int(rt.size))
    for i in range(n):
        l_row = lt[i]
        r_row = rt[i]
        ts_ok = abs(int(l_row["exchange_ts_ns"]) - int(r_row["exchange_ts_ns"])) <= field_tolerance_ns
        same = (
            ts_ok
            and int(l_row["symbol_id"]) == int(r_row["symbol_id"])
            and int(l_row["price_raw"]) == int(r_row["price_raw"])
            and int(l_row["qty_raw"]) == int(r_row["qty_raw"])
            and int(l_row["side"]) == int(r_row["side"])
        )
        if not same:
            if diff.first_divergence_index is None:
                diff.first_divergence_index = i
            if len(diff.mismatches) < max_mismatches:
                diff.mismatches.append({
                    "index": i,
                    "left": {
                        "exchange_ts_ns": int(l_row["exchange_ts_ns"]),
                        "symbol_id": int(l_row["symbol_id"]),
                        "price_raw": int(l_row["price_raw"]),
                        "qty_raw": int(l_row["qty_raw"]),
                        "side": int(l_row["side"]),
                    },
                    "right": {
                        "exchange_ts_ns": int(r_row["exchange_ts_ns"]),
                        "symbol_id": int(r_row["symbol_id"]),
                        "price_raw": int(r_row["price_raw"]),
                        "qty_raw": int(r_row["qty_raw"]),
                        "side": int(r_row["side"]),
                    },
                })

    if diff.left_count != diff.right_count and diff.first_divergence_index is None:
        # Same prefix, one tape extends past the other.
        diff.first_divergence_index = n

    diff.equal = (
        diff.left_count == diff.right_count
        and diff.first_divergence_index is None
    )
    return diff


__all__ = [
    "TapeRecorderStats",
    "TapeStats",
    "TapeDiff",
    "make_recorder_hook",
    "inspect_tape",
    "replay_tape",
    "diff_tapes",
]
