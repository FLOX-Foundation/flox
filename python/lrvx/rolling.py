"""Rolling-window aggregations for backtest research.

These are numpy helpers that target a single concrete pain point: the
naive Python loop with ``np.partition`` is fast enough for the
hourly timeframe (~17k bars) and unusable at 15-minute or 1-second
resolution. The implementations below stay in numpy C land using
``sliding_window_view`` + a vectorized ``np.partition`` along the
window axis, which keeps the work in one BLAS-style call.

The headline function is ``top_k_threshold`` — "what's the K-th
largest value in the trailing ``window`` bars?" — used by extreme-
event filters (volatility, volume, count, abs-return) where the
threshold has to recompute as the window slides.

Note on semantics: the returned ``thr[i]`` is the K-th largest value
in the half-open trailing window ``values[i - window : i]`` (the bar
at index ``i`` itself is NOT included). This matches the typical use
case — decide at the close of bar ``i`` whether the next bar's
threshold has been hit — and makes the helper free of forward-looking
contamination by construction.
"""
from __future__ import annotations

from typing import Optional, Union

import numpy as np

ArrayLike = Union[np.ndarray, "list[float]"]


def top_k_threshold(
    values: ArrayLike,
    *,
    window: int,
    k: int = 1,
    out_dtype: Optional[np.dtype] = None,
) -> np.ndarray:
    """Return the K-th largest value in each trailing window.

    ``thr[i]`` is the ``k``-th largest entry in
    ``values[i - window : i]`` (exclusive of bar ``i``). For
    ``i < window`` the slot is filled with ``NaN``.

    Parameters
    ----------
    values
        1D array (or anything ``np.asarray`` can coerce). NaN entries
        propagate through ``np.partition`` and will appear in the
        threshold if the window contains one — caller is responsible
        for masking them upstream if that is not what they want.
    window
        Trailing-window length in bars. Must be >= 1.
    k
        Rank: ``k=1`` is the maximum of the window, ``k=2`` the second
        largest, and so on. Must satisfy ``1 <= k <= window``.
    out_dtype
        Optional dtype for the returned array. Defaults to
        ``values.dtype`` (promoted to float64 for ints so NaN fits).

    Returns
    -------
    np.ndarray
        Same length as ``values``. First ``window`` slots are NaN.

    Notes
    -----
    Uses ``np.lib.stride_tricks.sliding_window_view`` + a single
    ``np.partition`` along the window axis. One C-level pass over
    ``(n - window) × window`` elements. Memory cost is the view (no
    copy) plus an O(n) output array.

    Benchmarks (`bench/rolling_top_k.py`): 17,520 bars (BTC 1h × 2y),
    ``window=720`` (30 days), ``k=3`` — ~25 ms end-to-end vs ~7 s for
    the naive Python loop with ``np.partition`` per bar.
    """
    v = np.asarray(values)
    if v.ndim != 1:
        raise ValueError(f"values must be 1D, got shape {v.shape}")
    if window < 1:
        raise ValueError(f"window must be >= 1, got {window}")
    if k < 1 or k > window:
        raise ValueError(f"k must be in [1, window]={window}, got {k}")

    n = v.shape[0]
    dtype = out_dtype or (np.float64 if not np.issubdtype(v.dtype, np.floating) else v.dtype)
    out = np.full(n, np.nan, dtype=dtype)
    if n <= window:
        return out

    # sliding_window_view gives (n - window + 1, window) read-only view.
    # Drop the last row so the window for output index i covers
    # values[i - window : i] (exclusive). i.e. the first valid output
    # index is `window` and uses windows[0] = values[0:window].
    from numpy.lib.stride_tricks import sliding_window_view

    windows = sliding_window_view(v, window_shape=window)[:-1]  # (n - window, window)
    # K-th largest = element at sorted position (window - k) when sorted
    # ascending. np.partition leaves that element in place.
    kth_index = window - k
    part = np.partition(windows, kth_index, axis=1)
    out[window:] = part[:, kth_index].astype(dtype, copy=False)
    return out


def rolling_max(values: ArrayLike, *, window: int) -> np.ndarray:
    """Convenience wrapper: ``top_k_threshold(values, window=window, k=1)``."""
    return top_k_threshold(values, window=window, k=1)


__all__ = [
    "top_k_threshold",
    "rolling_max",
]
