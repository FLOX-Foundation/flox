"""Tests for the MLflow integration in ``flox_py.mlflow``.

Each test uses a temp directory as the MLflow tracking root (a
``file:`` URI) so the tests are hermetic — no shared MLflow server,
no env-var leakage. ``MlflowClient`` is the canonical way to read
runs back out, so the assertions hit it directly.

`mlflow` is an optional dependency. When it isn't installed, every
test in this module is skipped with a clear reason.
"""
from __future__ import annotations

import csv
import os
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
PY_PKG = HERE.parent
sys.path.insert(0, str(PY_PKG))

try:
    import mlflow  # noqa: F401
    from mlflow.tracking import MlflowClient  # noqa: F401
    HAS_MLFLOW = True
except ImportError:
    HAS_MLFLOW = False


@unittest.skipUnless(HAS_MLFLOW, "mlflow not installed")
class MlflowIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="flox-mlflow-"))
        self.uri = f"file:{self.tmp}"
        # Reset the in-process default so each test sees a fresh root.
        import mlflow as _ml
        _ml.set_tracking_uri(self.uri)
        _ml.set_experiment("flox-test")
        self._mlflow = _ml

    def tearDown(self) -> None:
        # Best-effort cleanup; missing dirs are fine.
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _client(self):
        from mlflow.tracking import MlflowClient
        return MlflowClient(tracking_uri=self.uri)

    # ── log_backtest ─────────────────────────────────────────────

    def test_log_backtest_records_metrics_and_params(self) -> None:
        from flox_py import mlflow as flox_mlflow

        stats = {
            "return_pct": 12.34,
            "sharpe": 1.42,
            "max_drawdown_pct": 5.7,
            "total_trades": 42,
            "win_rate": 0.66,
            # extra non-numeric:
            "first_trade": "2024-01-01T00:00:00",
        }
        run_id = flox_mlflow.log_backtest(
            stats=stats,
            params={"fast": 10, "slow": 30, "fee": 0.0004},
            run_name="test-1",
            tracking_uri=self.uri,
            experiment="flox-test",
        )
        self.assertTrue(run_id)
        run = self._client().get_run(run_id)
        # Numeric stats land in metrics:
        self.assertAlmostEqual(run.data.metrics["return_pct"], 12.34, places=4)
        self.assertAlmostEqual(run.data.metrics["sharpe"], 1.42, places=4)
        self.assertEqual(int(run.data.metrics["total_trades"]), 42)
        # Non-numeric stat goes to tags:
        self.assertIn("first_trade", run.data.tags)
        # Params land as MLflow params:
        self.assertEqual(run.data.params["fast"], "10")
        self.assertEqual(run.data.params["slow"], "30")

    def test_log_backtest_writes_equity_and_trades_artifacts(self) -> None:
        from flox_py import mlflow as flox_mlflow

        equity_curve = [
            {"timestamp_ns": 1, "equity": 10000.0, "drawdown_pct": 0.0},
            {"timestamp_ns": 2, "equity": 10100.0, "drawdown_pct": 0.0},
            {"timestamp_ns": 3, "equity": 10050.0, "drawdown_pct": 0.5},
        ]
        trades = [
            {"symbol": "BTCUSDT", "side": "BUY",
             "entry_price": 100.0, "exit_price": 101.0,
             "quantity": 0.01, "pnl": 0.01,
             "fee": 0.001, "entry_time_ns": 1, "exit_time_ns": 2},
        ]
        run_id = flox_mlflow.log_backtest(
            stats={"return_pct": 0.5, "total_trades": 1},
            equity_curve=equity_curve,
            trades=trades,
            run_name="test-2",
            tracking_uri=self.uri,
            experiment="flox-test",
        )
        names = {a.path for a in self._client().list_artifacts(run_id)}
        self.assertIn("equity_curve.csv", names)
        self.assertIn("trades.csv", names)
        # PNG is best-effort (matplotlib may be absent); only assert if
        # matplotlib imports cleanly.
        try:
            import matplotlib  # noqa: F401
            self.assertIn("equity_curve.png", names)
        except ImportError:
            pass

        # Verify equity_curve.csv content.
        run = self._client().get_run(run_id)
        artifact_dir = Path(run.info.artifact_uri.replace("file:", ""))
        with (artifact_dir / "equity_curve.csv").open() as f:
            rows = list(csv.DictReader(f))
        self.assertEqual(len(rows), 3)
        self.assertEqual(rows[1]["equity"], "10100.0")

    def test_log_backtest_attaches_html_report(self) -> None:
        from flox_py import mlflow as flox_mlflow

        report_path = self.tmp / "report.html"
        report_path.write_text("<html><body>FLOX</body></html>")
        run_id = flox_mlflow.log_backtest(
            stats={"return_pct": 1.0},
            html_report=str(report_path),
            run_name="test-html",
            tracking_uri=self.uri,
            experiment="flox-test",
        )
        names = {a.path for a in self._client().list_artifacts(run_id)}
        self.assertIn("report.html", names)

    def test_log_backtest_handles_nan_metrics_as_tags(self) -> None:
        """NaN / Inf are not loggable as MLflow metrics. They must be
        retained as tag strings so the value is still visible in the UI
        rather than silently dropped."""
        import math

        from flox_py import mlflow as flox_mlflow

        run_id = flox_mlflow.log_backtest(
            stats={"sharpe": float("nan"),
                   "max_drawdown_pct": float("inf"),
                   "return_pct": 5.0},
            run_name="test-nan",
            tracking_uri=self.uri,
            experiment="flox-test",
        )
        run = self._client().get_run(run_id)
        self.assertIn("sharpe", run.data.tags)
        self.assertIn("max_drawdown_pct", run.data.tags)
        self.assertAlmostEqual(run.data.metrics["return_pct"], 5.0, places=4)

    # ── log_to_mlflow context manager ──────────────────────────────

    def test_context_manager_yields_logger_and_closes_run(self) -> None:
        from flox_py import mlflow as flox_mlflow

        with flox_mlflow.log_to_mlflow(
            run_name="ctx-1",
            tracking_uri=self.uri,
            experiment="flox-test",
        ) as run:
            self.assertTrue(run.run_id)
            run.log_params({"fast": 5})
            run.log_backtest(
                stats={"return_pct": 7.0, "total_trades": 3},
            )
            run.log_tags({"strategy": "sma"})
        # Run is FINISHED on context exit.
        info = self._client().get_run(run.run_id).info
        self.assertEqual(info.status, "FINISHED")

    def test_context_manager_finishes_run_on_exception(self) -> None:
        """Uncaught exception inside the block must still close the run
        (mlflow's start_run handles this; we just confirm we didn't
        break it)."""
        from flox_py import mlflow as flox_mlflow

        run_id = None
        with self.assertRaises(RuntimeError):
            with flox_mlflow.log_to_mlflow(
                run_name="ctx-fail",
                tracking_uri=self.uri,
                experiment="flox-test",
            ) as run:
                run_id = run.run_id
                raise RuntimeError("boom")
        self.assertIsNotNone(run_id)
        info = self._client().get_run(run_id).info
        self.assertNotEqual(info.status, "RUNNING")

    def test_context_manager_log_artifact(self) -> None:
        from flox_py import mlflow as flox_mlflow

        extra = self.tmp / "notes.txt"
        extra.write_text("custom artifact\n")
        with flox_mlflow.log_to_mlflow(
            run_name="ctx-art",
            tracking_uri=self.uri,
            experiment="flox-test",
        ) as run:
            run.log_artifact(str(extra))
            run_id = run.run_id
        names = {a.path for a in self._client().list_artifacts(run_id)}
        self.assertIn("notes.txt", names)


class OptionalDependencyTests(unittest.TestCase):
    """Verify the mlflow module presents a clean error when mlflow is
    not installed. We can't uninstall it inside one process, so this
    test mocks the import."""

    def test_clear_error_when_mlflow_missing(self) -> None:
        from flox_py import mlflow as flox_mlflow
        import builtins

        real_import = builtins.__import__

        def _fake_import(name, *args, **kwargs):
            if name == "mlflow":
                raise ImportError("simulated absence")
            return real_import(name, *args, **kwargs)

        builtins.__import__ = _fake_import
        try:
            with self.assertRaises(ImportError) as ctx:
                flox_mlflow.log_backtest(stats={"return_pct": 1.0})
            self.assertIn("mlflow", str(ctx.exception))
            self.assertIn("optional", str(ctx.exception).lower())
        finally:
            builtins.__import__ = real_import


if __name__ == "__main__":
    unittest.main()
