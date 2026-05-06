"""MLflow integration for FLOX backtests.

Optional helpers for logging a FLOX backtest as an MLflow run:
the stats dict becomes metrics, the params dict becomes params, the
equity curve / trades / HTML report attach as artifacts. Two entry
points:

* :func:`log_backtest` — one-shot logging of a finished backtest. The
  caller gives the stats / equity curve / trades, and the function
  starts and ends the MLflow run by itself.
* :func:`log_to_mlflow` — context manager. Use this when you want to
  log additional things (custom params, extra artifacts) inside the
  same run.

`mlflow` is an optional dependency. The functions raise a clear
ImportError if it is not installed.
"""
from __future__ import annotations

import contextlib
import csv
import io
import math
import os
import tempfile
from pathlib import Path
from typing import Any, Iterator, List, Mapping, Optional, Sequence


# Subset of stats keys that are numerically meaningful as MLflow
# metrics. The full dict (which may include strings, dates, nested
# objects) is not logged as metrics; non-numeric keys end up as run
# tags so the MLflow UI still shows them.
_NUMERIC_STAT_KEYS = (
    "return_pct", "sharpe", "sortino", "max_drawdown_pct",
    "calmar", "profit_factor", "win_rate", "expectancy",
    "total_trades", "winning_trades", "losing_trades",
    "net_pnl", "gross_pnl", "fees_paid",
    "avg_win", "avg_loss", "avg_trade_duration_seconds",
    "longest_win_streak", "longest_loss_streak",
)


def _import_mlflow():
    """Import MLflow lazily and surface a friendly error otherwise."""
    try:
        import mlflow  # type: ignore
        return mlflow
    except ImportError as exc:
        raise ImportError(
            "log_backtest / log_to_mlflow require the 'mlflow' package. "
            "Install it with `pip install mlflow`. The integration is "
            "optional; everything else in flox_py works without it."
        ) from exc


def _equity_csv_bytes(equity_curve: Sequence[Mapping[str, Any]]) -> bytes:
    """Serialize the equity curve to CSV.

    Accepts the list-of-dicts shape returned by ``BacktestRunner.equity_curve()``
    (keys: ``timestamp_ns``, ``equity``, ``drawdown_pct``).
    """
    if not equity_curve:
        return b""
    fieldnames = list(equity_curve[0].keys())
    buf = io.StringIO()
    writer = csv.DictWriter(buf, fieldnames=fieldnames)
    writer.writeheader()
    for row in equity_curve:
        writer.writerow(row)
    return buf.getvalue().encode("utf-8")


def _trades_csv_bytes(trades: Sequence[Mapping[str, Any]]) -> bytes:
    if not trades:
        return b""
    # Use a stable column order: any keys in the first trade come first,
    # any extra keys in later rows append in observed order.
    fieldnames: List[str] = list(trades[0].keys())
    seen = set(fieldnames)
    for row in trades[1:]:
        for k in row.keys():
            if k not in seen:
                fieldnames.append(k)
                seen.add(k)
    buf = io.StringIO()
    writer = csv.DictWriter(buf, fieldnames=fieldnames)
    writer.writeheader()
    for row in trades:
        writer.writerow(row)
    return buf.getvalue().encode("utf-8")


def _equity_png_bytes(
    equity_curve: Sequence[Mapping[str, Any]]
) -> Optional[bytes]:
    """Render a small equity-curve PNG via matplotlib if it's available.

    Returns None if matplotlib is not installed — the CSV artifact is
    always logged anyway, so the MLflow run is still useful.
    """
    try:
        import matplotlib  # type: ignore
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt  # type: ignore
    except ImportError:
        return None
    if not equity_curve:
        return None

    xs = [row.get("timestamp_ns", i) for i, row in enumerate(equity_curve)]
    ys = [row.get("equity") for row in equity_curve]
    if any(y is None for y in ys):
        return None

    fig, ax = plt.subplots(figsize=(8, 4))
    ax.plot(xs, ys, linewidth=1.2)
    ax.set_xlabel("time (ns)")
    ax.set_ylabel("equity")
    ax.set_title("Equity curve")
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=120)
    plt.close(fig)
    return buf.getvalue()


def _flush_artifacts_to(
    mlflow,
    *,
    equity_curve: Optional[Sequence[Mapping[str, Any]]],
    trades: Optional[Sequence[Mapping[str, Any]]],
    html_report: Optional[str],
) -> None:
    """Write artifacts via temporary files (``mlflow.log_artifact``
    expects a real path)."""
    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        if equity_curve:
            (td_path / "equity_curve.csv").write_bytes(
                _equity_csv_bytes(equity_curve)
            )
            mlflow.log_artifact(str(td_path / "equity_curve.csv"))
            png = _equity_png_bytes(equity_curve)
            if png is not None:
                (td_path / "equity_curve.png").write_bytes(png)
                mlflow.log_artifact(str(td_path / "equity_curve.png"))
        if trades:
            (td_path / "trades.csv").write_bytes(_trades_csv_bytes(trades))
            mlflow.log_artifact(str(td_path / "trades.csv"))
        if html_report:
            html_path = Path(html_report)
            if html_path.is_file():
                mlflow.log_artifact(str(html_path))


def _log_stats(mlflow, stats: Mapping[str, Any]) -> None:
    """Split stats into MLflow metrics + tags. Numeric keys go to
    log_metric; everything else becomes a tag (so the MLflow UI still
    shows it)."""
    for key, value in stats.items():
        if isinstance(value, bool):
            # mlflow rejects bool as a metric; record as tag.
            mlflow.set_tag(key, "true" if value else "false")
            continue
        if isinstance(value, (int, float)) and key in _NUMERIC_STAT_KEYS:
            if isinstance(value, float) and (
                math.isnan(value) or math.isinf(value)
            ):
                # mlflow refuses NaN/Inf; record as a tag string instead.
                mlflow.set_tag(key, repr(value))
                continue
            mlflow.log_metric(key, float(value))
        elif isinstance(value, (int, float)):
            # Numeric but not in the recognised list — keep, but as tag
            # so people don't get plot-axis surprises in the UI.
            mlflow.set_tag(key, str(value))
        else:
            mlflow.set_tag(key, str(value))


def log_backtest(
    stats: Mapping[str, Any],
    *,
    equity_curve: Optional[Sequence[Mapping[str, Any]]] = None,
    trades: Optional[Sequence[Mapping[str, Any]]] = None,
    params: Optional[Mapping[str, Any]] = None,
    run_name: Optional[str] = None,
    experiment: Optional[str] = None,
    tracking_uri: Optional[str] = None,
    tags: Optional[Mapping[str, str]] = None,
    html_report: Optional[str] = None,
) -> str:
    """Log a finished backtest as one MLflow run.

    Returns the run ID so callers can compose with other tooling
    (e.g. flag the run as a parent of a child sweep).

    ``stats`` keys listed in :data:`_NUMERIC_STAT_KEYS` go to
    ``log_metric``; everything else attaches as a run tag. ``params``
    are logged via ``log_param`` so MLflow displays them in the run
    overview. ``equity_curve`` and ``trades`` are written to CSV
    (plus a PNG of the equity curve when matplotlib is installed) and
    attached as artifacts. ``html_report`` is a path to the file
    written by :func:`flox_py.report.write_html` and is attached as
    an artifact verbatim.
    """
    mlflow = _import_mlflow()
    if tracking_uri is not None:
        mlflow.set_tracking_uri(tracking_uri)
    if experiment is not None:
        mlflow.set_experiment(experiment)

    with mlflow.start_run(run_name=run_name) as active:
        if tags:
            mlflow.set_tags(dict(tags))
        if params:
            mlflow.log_params({k: str(v) for k, v in params.items()})
        _log_stats(mlflow, stats)
        _flush_artifacts_to(
            mlflow,
            equity_curve=equity_curve,
            trades=trades,
            html_report=html_report,
        )
        return active.info.run_id


class MlflowLogger:
    """Lightweight handle returned by :func:`log_to_mlflow`.

    Wraps the active MLflow run and exposes the same logging primitives
    the one-shot :func:`log_backtest` uses, so callers can mix
    auto-handled FLOX artifacts with whatever extra logging they need
    inside the same run.
    """

    def __init__(self, mlflow_module, run_id: str) -> None:
        self._mlflow = mlflow_module
        self._run_id = run_id

    @property
    def run_id(self) -> str:
        return self._run_id

    def log_params(self, params: Mapping[str, Any]) -> None:
        self._mlflow.log_params({k: str(v) for k, v in params.items()})

    def log_tags(self, tags: Mapping[str, str]) -> None:
        self._mlflow.set_tags(dict(tags))

    def log_backtest(
        self,
        stats: Mapping[str, Any],
        *,
        equity_curve: Optional[Sequence[Mapping[str, Any]]] = None,
        trades: Optional[Sequence[Mapping[str, Any]]] = None,
        params: Optional[Mapping[str, Any]] = None,
        html_report: Optional[str] = None,
    ) -> None:
        if params:
            self.log_params(params)
        _log_stats(self._mlflow, stats)
        _flush_artifacts_to(
            self._mlflow,
            equity_curve=equity_curve,
            trades=trades,
            html_report=html_report,
        )

    def log_artifact(self, path: str, artifact_path: Optional[str] = None) -> None:
        self._mlflow.log_artifact(path, artifact_path=artifact_path)


@contextlib.contextmanager
def log_to_mlflow(
    *,
    run_name: Optional[str] = None,
    experiment: Optional[str] = None,
    tracking_uri: Optional[str] = None,
    nested: bool = False,
    tags: Optional[Mapping[str, str]] = None,
) -> Iterator[MlflowLogger]:
    """Context-manager flavour. Opens an MLflow run, yields a logger,
    and closes the run when the block exits (even on exception).

    ``tracking_uri`` defaults to whatever ``mlflow.get_tracking_uri()``
    already returns (driven by the ``MLFLOW_TRACKING_URI`` env var or
    a previous ``mlflow.set_tracking_uri`` call). Pass it explicitly
    when you want a per-block override.
    """
    mlflow = _import_mlflow()
    if tracking_uri is not None:
        mlflow.set_tracking_uri(tracking_uri)
    if experiment is not None:
        mlflow.set_experiment(experiment)
    with mlflow.start_run(run_name=run_name, nested=nested) as active:
        if tags:
            mlflow.set_tags(dict(tags))
        yield MlflowLogger(mlflow, active.info.run_id)


__all__ = ["log_backtest", "log_to_mlflow", "MlflowLogger"]
