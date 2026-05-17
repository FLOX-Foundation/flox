"""Cross-sectional (T × S) panel builder over multiple floxlog tapes.

Cross-symbol research (XS momentum, pair stat-arb, rank-based long /
short, dispersion trading) needs a single aligned 2D array indexed by
(time bucket, symbol). The hand-rolled version is shared bug surface:
intersection vs union, NaN handling, symbol ordering drift between
calls, off-by-one in the alignment join.

The helpers in this module collapse the pattern to one call:

* ``build_close_panel`` — close-only panel, returned as a ``Panel``.
* ``build_ohlc_panel`` — open / high / low / close, each (T, S).
* ``build_returns_panel`` — log-period returns over a ``lookback_n``
  bar window, derived from the close panel.

Three alignment modes:

* ``intersection`` — keep only timestamps present in every input tape.
* ``union_nan``   — union of timestamps; missing values are ``NaN``.
* ``union_ffill`` — union of timestamps; missing values are
  forward-filled from the most recent prior bar for that symbol.

Input layout assumption: per-symbol floxlog directories under
``tape_root/<SYMBOL>``. This matches the layout `flox archive binance`
writes and the existing single-symbol tape recorder.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import List, Mapping, Optional, Sequence, Union

import numpy as np

AlignMode = str  # "intersection" | "union_nan" | "union_ffill"

_VALID_MODES = ("intersection", "union_nan", "union_ffill")

PathLike = Union[str, Path]


@dataclass
class Panel:
    """Result of :func:`build_close_panel`.

    ``values[t, j]`` is the close for symbol ``symbols[j]`` at bucket
    ``ts[t]``. Shape: ``(T, S)``. ``ts`` is monotonic non-decreasing.
    """

    ts: np.ndarray            # int64, shape (T,)
    values: np.ndarray        # float64, shape (T, S)
    symbols: List[str]        # length S, aligned to columns
    mode: AlignMode


@dataclass
class OHLCPanel:
    """Result of :func:`build_ohlc_panel`. Each of open/high/low/close
    is shape ``(T, S)`` float64 (NaN where missing). Volume is not
    surfaced because the underlying ``OHLCBinAggregator`` does not
    emit it as of this release; add a panel variant when the engine
    exposes it."""

    ts: np.ndarray
    open: np.ndarray
    high: np.ndarray
    low: np.ndarray
    close: np.ndarray
    symbols: List[str]
    mode: AlignMode


@dataclass
class ReturnsPanel:
    """Result of :func:`build_returns_panel`. ``values[t, j]`` is the
    simple return over the trailing ``lookback_n`` bars."""

    ts: np.ndarray
    values: np.ndarray
    symbols: List[str]
    lookback_n: int
    mode: AlignMode


# ── Per-symbol OHLC fetch ───────────────────────────────────────────


def _resolve_tape_path(
    tape_root: Optional[PathLike],
    tape_paths: Optional[Mapping[str, PathLike]],
    symbol: str,
) -> Path:
    if tape_paths is not None and symbol in tape_paths:
        return Path(tape_paths[symbol]).expanduser()
    if tape_root is None:
        raise ValueError(
            f"no tape path for {symbol!r}: pass either tape_root with a "
            f"per-symbol subdirectory layout, or tape_paths={{symbol: path}}."
        )
    return Path(tape_root).expanduser() / symbol


def _ohlc_for_symbol(
    tape_path: Path,
    *,
    bucket_ns: int,
    t_from: Optional[int],
    t_to: Optional[int],
) -> np.ndarray:
    """Return the OHLCBinAggregator structured-array result for one
    tape directory. Optional time bounds clip the result before
    alignment so we never align empty rows."""
    import flox_py

    if not tape_path.exists():
        raise FileNotFoundError(tape_path)
    reader = flox_py.DataReader(str(tape_path))
    agg = flox_py.OHLCBinAggregator(
        bucket_ns=int(bucket_ns),
        by_symbol=False,
    )
    reader.run([agg])
    out = agg.result()
    if out.size == 0:
        return out
    if t_from is not None or t_to is not None:
        ts = np.asarray(out["bucket_ts_ns"])
        mask = np.ones(ts.size, dtype=bool)
        if t_from is not None:
            mask &= ts >= int(t_from)
        if t_to is not None:
            mask &= ts < int(t_to)
        out = out[mask]
    return out


# ── Alignment primitive ─────────────────────────────────────────────


def _validate_mode(mode: str) -> None:
    if mode not in _VALID_MODES:
        raise ValueError(
            f"unknown align mode {mode!r}. expected one of {_VALID_MODES}"
        )


def _align(
    per_symbol_ts: List[np.ndarray],
    per_symbol_values: List[List[np.ndarray]],
    *,
    mode: str,
) -> tuple[np.ndarray, List[np.ndarray]]:
    """Align N per-symbol time series onto a common index.

    ``per_symbol_ts``: N arrays of int64 timestamps (each sorted asc).
    ``per_symbol_values``: list-of-lists; one entry per metric (e.g.
    [closes], or [opens, highs, lows, closes]), each entry is N arrays
    aligned to per_symbol_ts.

    Returns the common timestamp index and one (T × N) float64 array
    per metric.
    """
    n = len(per_symbol_ts)
    if n == 0:
        return np.empty(0, dtype=np.int64), [np.empty((0, 0)) for _ in per_symbol_values]

    if mode == "intersection":
        common = per_symbol_ts[0]
        for ts in per_symbol_ts[1:]:
            common = np.intersect1d(common, ts, assume_unique=True)
    else:  # union variants
        common = per_symbol_ts[0]
        for ts in per_symbol_ts[1:]:
            common = np.union1d(common, ts)

    t = common.size
    metrics_out: List[np.ndarray] = []
    for vals_per_sym in per_symbol_values:
        out = np.full((t, n), np.nan, dtype=np.float64)
        for j, (ts_j, vals_j) in enumerate(zip(per_symbol_ts, vals_per_sym)):
            if ts_j.size == 0:
                continue
            # searchsorted gives the matched indices in `common` for
            # each timestamp in ts_j.
            idx = np.searchsorted(common, ts_j)
            # On intersection mode, every ts_j entry is in common, but
            # some may be filtered out — keep only valid hits.
            ok = (idx < t) & (common[np.clip(idx, 0, t - 1)] == ts_j)
            out[idx[ok], j] = vals_j[ok]
        if mode == "union_ffill":
            # Forward-fill NaN down each column. Vectorised via the
            # canonical "running max of valid-index" trick.
            for j in range(n):
                col = out[:, j]
                valid = ~np.isnan(col)
                if not valid.any():
                    continue
                idx = np.where(valid, np.arange(t), -1)
                np.maximum.accumulate(idx, out=idx)
                # idx < 0 means no prior observation; leave NaN.
                mask = idx >= 0
                out[mask, j] = col[idx[mask]]
        metrics_out.append(out)
    return common, metrics_out


# ── Public builders ─────────────────────────────────────────────────


def build_close_panel(
    symbols: Sequence[str],
    *,
    bucket_ns: int,
    tape_root: Optional[PathLike] = None,
    tape_paths: Optional[Mapping[str, PathLike]] = None,
    align: AlignMode = "intersection",
    t_from: Optional[int] = None,
    t_to: Optional[int] = None,
) -> Panel:
    """Aligned (T × S) close panel over multiple floxlog tapes."""
    _validate_mode(align)
    syms = list(symbols)

    ts_list: List[np.ndarray] = []
    close_list: List[np.ndarray] = []
    for sym in syms:
        tape = _resolve_tape_path(tape_root, tape_paths, sym)
        rows = _ohlc_for_symbol(tape, bucket_ns=bucket_ns,
                                t_from=t_from, t_to=t_to)
        if rows.size == 0:
            ts_list.append(np.empty(0, dtype=np.int64))
            close_list.append(np.empty(0, dtype=np.float64))
            continue
        ts_list.append(np.asarray(rows["bucket_ts_ns"], dtype=np.int64))
        close_list.append(
            np.asarray(rows["close_raw"], dtype=np.float64) / 1e8
        )

    common, metrics = _align(ts_list, [close_list], mode=align)
    return Panel(ts=common, values=metrics[0], symbols=syms, mode=align)


def build_ohlc_panel(
    symbols: Sequence[str],
    *,
    bucket_ns: int,
    tape_root: Optional[PathLike] = None,
    tape_paths: Optional[Mapping[str, PathLike]] = None,
    align: AlignMode = "intersection",
    t_from: Optional[int] = None,
    t_to: Optional[int] = None,
) -> OHLCPanel:
    """Aligned (T × S) open / high / low / close / volume panels."""
    _validate_mode(align)
    syms = list(symbols)

    ts_list: List[np.ndarray] = []
    o: List[np.ndarray] = []
    h: List[np.ndarray] = []
    lo: List[np.ndarray] = []
    c: List[np.ndarray] = []
    for sym in syms:
        tape = _resolve_tape_path(tape_root, tape_paths, sym)
        rows = _ohlc_for_symbol(tape, bucket_ns=bucket_ns,
                                t_from=t_from, t_to=t_to)
        if rows.size == 0:
            ts_list.append(np.empty(0, dtype=np.int64))
            o.append(np.empty(0, dtype=np.float64))
            h.append(np.empty(0, dtype=np.float64))
            lo.append(np.empty(0, dtype=np.float64))
            c.append(np.empty(0, dtype=np.float64))
            continue
        ts_list.append(np.asarray(rows["bucket_ts_ns"], dtype=np.int64))
        o.append(np.asarray(rows["open_raw"], dtype=np.float64) / 1e8)
        h.append(np.asarray(rows["high_raw"], dtype=np.float64) / 1e8)
        lo.append(np.asarray(rows["low_raw"], dtype=np.float64) / 1e8)
        c.append(np.asarray(rows["close_raw"], dtype=np.float64) / 1e8)

    common, metrics = _align(ts_list, [o, h, lo, c], mode=align)
    return OHLCPanel(
        ts=common,
        open=metrics[0],
        high=metrics[1],
        low=metrics[2],
        close=metrics[3],
        symbols=syms,
        mode=align,
    )


def build_returns_panel(
    symbols: Sequence[str],
    *,
    bucket_ns: int,
    lookback_n: int,
    tape_root: Optional[PathLike] = None,
    tape_paths: Optional[Mapping[str, PathLike]] = None,
    align: AlignMode = "intersection",
    t_from: Optional[int] = None,
    t_to: Optional[int] = None,
) -> ReturnsPanel:
    """Aligned (T × S) simple-return panel over ``lookback_n`` bars."""
    if lookback_n < 1:
        raise ValueError(f"lookback_n must be >= 1, got {lookback_n}")
    p = build_close_panel(
        symbols,
        bucket_ns=bucket_ns,
        tape_root=tape_root,
        tape_paths=tape_paths,
        align=align,
        t_from=t_from,
        t_to=t_to,
    )
    closes = p.values
    if closes.shape[0] <= lookback_n:
        rets = np.full_like(closes, np.nan, dtype=np.float64)
    else:
        prev = closes[:-lookback_n]
        cur = closes[lookback_n:]
        with np.errstate(invalid="ignore", divide="ignore"):
            ret_body = (cur - prev) / prev
        rets = np.full_like(closes, np.nan, dtype=np.float64)
        rets[lookback_n:] = ret_body
    return ReturnsPanel(
        ts=p.ts,
        values=rets,
        symbols=p.symbols,
        lookback_n=int(lookback_n),
        mode=align,
    )


__all__ = [
    "Panel",
    "OHLCPanel",
    "ReturnsPanel",
    "build_close_panel",
    "build_ohlc_panel",
    "build_returns_panel",
]
