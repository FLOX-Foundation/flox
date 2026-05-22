"""LiveQueuePositionEstimator calibration toolkit.

`LiveQueuePositionEstimator` (from `flox_py`) carries two
tunable knobs that govern how its `confidence` value drifts: a
`half_life_ns` (time decay) and a `shrink_factor` (per-event
proportional-shrink penalty). Both have sensible defaults (60s,
0.85), but the right values depend on the venue's order-flow
characteristics. Without calibration, `confidence` is a vibe
metric.

This module fits those two knobs against ground-truth samples and
returns a `CalibrationResult` that can be applied back to the
estimator (or exported as JSON for production use).

Two ground-truth sources are supported:

1. **Venue-published queue position** — pass `(estimator_value,
   ground_truth)` pairs via `record_sample`.
2. **Test-order roundtrip** — `TestOrderHelper` ties an order's
   sim-fill timing to the predicted queue depth at submit. Use
   this when no venue-published source is available.

Calibration is offline-fit only. For continuous online calibration
file a follow-up.
"""

from __future__ import annotations

import json
import math
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import List, Optional


@dataclass
class CalibrationResult:
    """Fitted half-life + shrink-factor with residual diagnostics."""

    half_life_ns: int
    shrink_factor: float
    residual_rmse: float
    sample_count: int

    def to_dict(self) -> dict:
        return asdict(self)

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), indent=2)

    @classmethod
    def from_dict(cls, d: dict) -> "CalibrationResult":
        return cls(
            half_life_ns=int(d["half_life_ns"]),
            shrink_factor=float(d["shrink_factor"]),
            residual_rmse=float(d["residual_rmse"]),
            sample_count=int(d["sample_count"]),
        )


@dataclass
class _Sample:
    order_id: int
    estimator_value: float
    ground_truth: float
    elapsed_ns: int
    shrink_events: int


class TestOrderHelper:
    """Tie a known-size test order to its sim-fill outcome.

    Records the estimator's queue-ahead prediction at submit and
    the actual fill outcome later, then registers a calibration
    sample on the parent calibrator.
    """

    def __init__(self, calibrator: "LiveQueueCalibrator", symbol: int, size: float):
        self._calibrator = calibrator
        self._symbol = symbol
        self._size = size
        self._order_id: Optional[int] = None
        self._predicted_ahead: Optional[float] = None
        self._submit_ns: Optional[int] = None

    def place(self, order_id: int, price: float, predicted_ahead: float,
              submit_ns: int) -> None:
        """Record the estimator's prediction at submit time."""
        self._order_id = order_id
        self._predicted_ahead = predicted_ahead
        self._submit_ns = submit_ns

    def record_outcome(self, filled_at_ns: int, fill_qty: float,
                       observed_ahead_at_fill: float) -> None:
        """Match prediction to ground truth observed at fill.

        `observed_ahead_at_fill` is the queue-ahead the test order
        actually experienced (derived from the cumulative volume
        traded at the level between submit and fill). The
        difference between this and the prediction is the
        miscalibration residual the fit minimises.
        """
        if self._order_id is None or self._predicted_ahead is None or self._submit_ns is None:
            raise RuntimeError("TestOrderHelper.place must be called before record_outcome")
        elapsed = max(0, int(filled_at_ns) - int(self._submit_ns))
        self._calibrator.record_sample(
            order_id=self._order_id,
            estimator_value=float(self._predicted_ahead),
            ground_truth=float(observed_ahead_at_fill),
            elapsed_ns=elapsed,
        )


class LiveQueueCalibrator:
    """Fits half-life + shrink-factor for LiveQueuePositionEstimator.

    The model is intentionally simple: each sample carries an
    estimator value, a ground-truth value, an elapsed time since
    submit, and (optionally) the number of proportional-shrink
    events that fed into the estimate. A small grid search over
    `(half_life_ns, shrink_factor)` minimises the RMS residual
    between estimator and ground truth.

    For real research use, swap the grid search for `scipy.optimize`
    by importing scipy in the consuming script — the public surface
    here is deliberately dependency-free so flox_py keeps a small
    install footprint.
    """

    def __init__(self, estimator: Optional[object] = None):
        self._estimator = estimator
        self._samples: List[_Sample] = []

    def record_sample(self, order_id: int, estimator_value: float,
                      ground_truth: float, elapsed_ns: int = 0,
                      shrink_events: int = 0) -> None:
        """Record a single calibration sample."""
        self._samples.append(_Sample(
            order_id=int(order_id),
            estimator_value=float(estimator_value),
            ground_truth=float(ground_truth),
            elapsed_ns=int(elapsed_ns),
            shrink_events=int(shrink_events),
        ))

    def test_order_helper(self, symbol: int, size: float) -> TestOrderHelper:
        return TestOrderHelper(self, symbol, size)

    def sample_count(self) -> int:
        return len(self._samples)

    def clear(self) -> None:
        self._samples.clear()

    def fit(self,
            half_life_grid_ns: Optional[List[int]] = None,
            shrink_grid: Optional[List[float]] = None) -> CalibrationResult:
        """Minimise RMS residual over a small parameter grid.

        Default grid covers 10s..600s half-life and 0.5..0.99 shrink
        factor. Pass custom grids to refine around a fit found in
        an earlier pass.
        """
        if not self._samples:
            raise RuntimeError("no samples recorded; call record_sample first")
        if half_life_grid_ns is None:
            half_life_grid_ns = [
                10_000_000_000,   # 10s
                30_000_000_000,   # 30s
                60_000_000_000,   # 60s
                120_000_000_000,  # 2m
                300_000_000_000,  # 5m
                600_000_000_000,  # 10m
            ]
        if shrink_grid is None:
            shrink_grid = [0.50, 0.60, 0.70, 0.80, 0.85, 0.90, 0.95, 0.99]

        best = None
        for hl in half_life_grid_ns:
            for sf in shrink_grid:
                rmse = self._rmse_for(hl, sf)
                if best is None or rmse < best[2]:
                    best = (hl, sf, rmse)

        assert best is not None
        return CalibrationResult(
            half_life_ns=int(best[0]),
            shrink_factor=float(best[1]),
            residual_rmse=float(best[2]),
            sample_count=len(self._samples),
        )

    def _rmse_for(self, half_life_ns: int, shrink_factor: float) -> float:
        """Score one (half_life, shrink) pair against the recorded samples.

        Adjusts the estimator's reported value by:
        - exp(-elapsed/half_life) confidence decay
        - shrink_factor^shrink_events compounding penalty
        Then compares to ground truth and returns RMS residual.
        """
        sq_sum = 0.0
        for s in self._samples:
            decay = 1.0
            if half_life_ns > 0 and s.elapsed_ns > 0:
                decay = math.exp(-float(s.elapsed_ns) / float(half_life_ns))
            shrink_penalty = shrink_factor ** max(0, s.shrink_events)
            adjusted = s.estimator_value * decay * shrink_penalty
            residual = adjusted - s.ground_truth
            sq_sum += residual * residual
        return math.sqrt(sq_sum / max(1, len(self._samples)))

    def export(self, path: Path | str) -> None:
        """Write the latest fit to disk as JSON."""
        result = self.fit()
        Path(path).write_text(result.to_json())

    @staticmethod
    def load(path: Path | str) -> CalibrationResult:
        return CalibrationResult.from_dict(json.loads(Path(path).read_text()))

    def apply(self, result: CalibrationResult) -> None:
        """Apply a fitted result to the estimator passed at construction.

        No-op when no estimator was attached (research-only mode).
        """
        if self._estimator is None:
            return
        if hasattr(self._estimator, "set_confidence_half_life_ns"):
            self._estimator.set_confidence_half_life_ns(int(result.half_life_ns))
        if hasattr(self._estimator, "set_shrink_attribution_factor"):
            self._estimator.set_shrink_attribution_factor(float(result.shrink_factor))


__all__ = ["LiveQueueCalibrator", "CalibrationResult", "TestOrderHelper"]
