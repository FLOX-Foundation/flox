"""One-shot point-in-time dataset export: tape -> bars -> features -> matrix.

Closes the research loop: the features in the exported dataset are computed
by the same streaming indicator code that runs in production, so a model
trained on this dataset sees exactly what the live strategy will see. The
point-in-time contract holds by construction -- streaming indicators only
ever consume data up to the current bar -- and is enforced by tests, not by
convention.

Typical flow::

    import flox_py
    from flox_py.dataset import build_dataset

    ds = build_dataset(
        data_dir="/data/tapes/bybit",
        interval_seconds=60,
        features=["sma_20", "rsi_14", "parkinson_vol_30"],
        label_horizon_bars=5,
    )
    X, y = ds["X"], ds["y"]          # numpy, aligned, no lookahead
    # ... train, export to ONNX, run the same features live.
"""

from __future__ import annotations

import numpy as np

from . import _flox_py as _core

# Feature-spec DSL: "<indicator>_<period>" resolves to the module-level batch
# function of the same name (which shares its implementation with the
# streaming class -- that is what buys online/offline parity). Close-based
# indicators consume close; the second group consumes high/low(/close).
# Only indicators whose batch function is fn(<inputs>, period) and returns a
# single column belong here. Excluded on purpose: autocorrelation (needs
# window+lag), cvd (needs OHLCV), obv (needs close+volume), adx / stochastic
# (return dicts, not a column) -- listing them made the DSL raise on use.
_CLOSE_BASED = {
    "sma", "ema", "rma", "dema", "tema", "kama", "rsi", "slope",
    "skewness", "kurtosis", "rolling_zscore", "shannon_entropy",
}
_HL_BASED = {"parkinson_vol"}
_HLC_BASED = {"atr", "cci", "chop"}


def _parse_feature(name):
    """Split "sma_20" -> ("sma", 20). Raises on unknown shapes."""
    base, sep, period = name.rpartition("_")
    if not sep or not period.isdigit():
        raise ValueError(
            f"feature '{name}' is not '<indicator>_<period>'; register custom "
            f"nodes via configure=(lambda graph, sym: graph.add_node(...))"
        )
    return base, int(period)


def _register_feature(graph, sym_id, name):
    base, period = _parse_feature(name)
    fn = getattr(_core, base, None)
    if fn is None:
        raise ValueError(f"unknown indicator '{base}' in feature '{name}'")
    if base in _CLOSE_BASED:
        graph.add_node(name, [], lambda g, s, fn=fn, p=period: fn(g.close(s), p))
    elif base in _HL_BASED:
        graph.add_node(name, [], lambda g, s, fn=fn, p=period: fn(g.high(s), g.low(s), p))
    elif base in _HLC_BASED:
        graph.add_node(
            name, [], lambda g, s, fn=fn, p=period: fn(g.high(s), g.low(s), g.close(s), p)
        )
    else:
        raise ValueError(
            f"indicator '{base}' has no default input mapping; register it "
            f"via configure="
        )


def _bars_from_trades(trades, interval_seconds):
    prices = _core.prices_to_double(trades["price_raw"])
    quantities = _core.quantities_to_double(trades["qty_raw"])
    # flox::Side: BUY == 0.
    is_buy = (trades["side"] == 0).astype(np.uint8)
    return _core.aggregate_time_bars(
        trades["exchange_ts_ns"], prices, quantities, is_buy, interval_seconds
    )


def _bars_to_arrays(bars):
    price_scale = float(_core.PRICE_SCALE)
    volume_scale = float(_core.VOLUME_SCALE)
    return {
        "ts_ns": bars["end_time_ns"].astype(np.int64),
        "close": bars["close_raw"].astype(np.float64) / price_scale,
        "high": bars["high_raw"].astype(np.float64) / price_scale,
        "low": bars["low_raw"].astype(np.float64) / price_scale,
        "volume": bars["volume_raw"].astype(np.float64) / volume_scale,
    }


def build_dataset(
    *,
    interval_seconds=None,
    features,
    data_dir=None,
    trades=None,
    bars=None,
    symbol=None,
    from_ns=None,
    to_ns=None,
    label_horizon_bars=None,
    configure=None,
    drop_warmup=True,
):
    """Build an aligned, point-in-time-correct feature dataset.

    Exactly one input source must be given:

    - ``data_dir``: a binary-log tape directory (read via ``DataReader``);
    - ``trades``: a ``PyTrade`` structured array (as from ``read_trades()``);
    - ``bars``: a dict with ``ts_ns``/``close`` (optionally ``high``, ``low``,
      ``volume``) numpy arrays, when bars are already built.

    ``features`` is a list of indicator-graph node names (see
    ``list_indicators()``); ``configure(graph, symbol_id)`` may register
    custom nodes before evaluation. Feature row ``t`` is computed strictly
    from bars ``0..t`` -- the streaming-indicator property that makes
    training data match live values bit for bit.

    ``label_horizon_bars=h`` adds ``y[t] = log(close[t+h] / close[t])``; the
    last ``h`` rows are dropped. ``drop_warmup`` removes leading rows where
    any feature is still NaN.

    Returns a dict: ``ts_ns``, ``close``, ``X`` (n x len(features)),
    ``feature_names``, and ``y`` when a horizon was requested.
    """
    sources = sum(x is not None for x in (data_dir, trades, bars))
    if sources != 1:
        raise ValueError("provide exactly one of data_dir=, trades=, bars=")
    if bars is None and interval_seconds is None:
        raise ValueError("interval_seconds is required when building bars from trades")
    if not features:
        raise ValueError("features must be a non-empty list of node names")

    if data_dir is not None:
        reader = _core.DataReader(data_dir, from_ns, to_ns, None, None)
        trades = reader.read_trades()

    if trades is not None:
        if symbol is not None:
            trades = trades[trades["symbol_id"] == symbol]
        if len(trades) == 0:
            raise ValueError("no trades in the selected range/symbol")
        ext_bars = _bars_from_trades(trades, interval_seconds)
        arrays = _bars_to_arrays(ext_bars)
    else:
        arrays = {
            "ts_ns": np.asarray(bars["ts_ns"], dtype=np.int64),
            "close": np.asarray(bars["close"], dtype=np.float64),
            "high": np.asarray(bars.get("high", bars["close"]), dtype=np.float64),
            "low": np.asarray(bars.get("low", bars["close"]), dtype=np.float64),
            "volume": np.asarray(
                bars.get("volume", np.zeros_like(bars["close"])), dtype=np.float64
            ),
        }

    sym_id = 0 if symbol is None else int(symbol)
    graph = _core.IndicatorGraph()
    graph.set_bars(sym_id, arrays["close"], arrays["high"], arrays["low"], arrays["volume"])
    if configure is not None:
        configure(graph, sym_id)

    columns = []
    for name in features:
        try:
            _register_feature(graph, sym_id, name)
        except ValueError:
            # Not a "<indicator>_<period>" spec: acceptable only when a
            # configure() callback registered the node itself.
            if configure is None:
                raise
        col = np.asarray(graph.require(sym_id, name), dtype=np.float64)
        if len(col) != len(arrays["close"]):
            raise ValueError(
                f"feature '{name}' returned {len(col)} rows for "
                f"{len(arrays['close'])} bars"
            )
        columns.append(col)

    X = np.column_stack(columns)
    ts_ns = arrays["ts_ns"]
    close = arrays["close"]

    y = None
    if label_horizon_bars is not None:
        h = int(label_horizon_bars)
        if h <= 0:
            raise ValueError("label_horizon_bars must be positive")
        if h >= len(close):
            raise ValueError("label horizon exceeds the number of bars")
        y = np.log(close[h:] / close[:-h])
        X, ts_ns, close = X[:-h], ts_ns[:-h], close[:-h]

    if drop_warmup:
        valid = ~np.isnan(X).any(axis=1)
        first = int(np.argmax(valid)) if valid.any() else len(valid)
        X, ts_ns, close = X[first:], ts_ns[first:], close[first:]
        if y is not None:
            y = y[first:]

    out = {
        "ts_ns": ts_ns,
        "close": close,
        "X": X,
        "feature_names": list(features),
    }
    if y is not None:
        out["y"] = y
    return out


def to_arrow(dataset):
    """Convert a ``build_dataset`` result to a ``pyarrow.Table``.

    Requires ``pyarrow`` (not a flox dependency): install it in the research
    environment. Column layout: ``ts_ns``, ``close``, one column per feature,
    and ``y`` when present -- ready for ``pyarrow.parquet.write_table``.
    """
    try:
        import pyarrow as pa
    except ImportError as exc:  # pragma: no cover
        raise ImportError(
            "pyarrow is required for to_arrow(); pip install pyarrow"
        ) from exc

    data = {
        "ts_ns": dataset["ts_ns"],
        "close": dataset["close"],
    }
    for i, name in enumerate(dataset["feature_names"]):
        data[name] = dataset["X"][:, i]
    if "y" in dataset:
        data["y"] = dataset["y"]
    return pa.table(data)
