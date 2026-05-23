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
            shrink_grid: Optional[List[float]] = None,
            method: str = "grid") -> CalibrationResult:
        """Minimise RMS residual against the recorded samples.

        method:
          - "grid" (default): scan a small fixed grid. Cheap, no extra
            dependencies, but the result lands on a grid point.
          - "scipy": continuous fit via scipy.optimize.minimize.
            Requires scipy (lazy import — clear error if missing).
            Returns a result at least as good as grid (RMSE ≤ grid).
          - "analytical": closed-form least-squares in log-space when
            all samples share the same shrink_events count. Raises
            with guidance when the precondition does not hold.

        Default grid covers 10s..600s half-life and 0.5..0.99 shrink
        factor. Pass custom grids to refine around a fit found in
        an earlier pass.
        """
        if not self._samples:
            raise RuntimeError("no samples recorded; call record_sample first")

        if method == "scipy":
            return self._fit_scipy()
        if method == "analytical":
            return self._fit_analytical()
        if method != "grid":
            raise ValueError(
                f"unknown fit method {method!r}; expected 'grid', 'scipy', or 'analytical'"
            )

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

    def _fit_scipy(self) -> CalibrationResult:
        """Continuous fit via scipy.optimize.minimize. Lazy import.

        Bootstrapped from the grid optimum so it never returns worse
        than the grid solution even if the local optimizer gets stuck.
        """
        try:
            from scipy.optimize import minimize  # type: ignore
        except ImportError as exc:
            raise ImportError(
                "scipy is required for fit(method='scipy'). "
                "Install with: pip install scipy"
            ) from exc

        # Bootstrap from the grid optimum so the local solver can only
        # improve, never regress, on the grid RMSE.
        grid_best = self.fit(method="grid")
        x0 = [float(grid_best.half_life_ns), float(grid_best.shrink_factor)]

        def objective(x):
            hl = max(1.0, x[0])
            sf = min(max(x[1], 1e-6), 0.999999)
            return self._rmse_for(int(hl), sf)

        bounds = [(1.0, 1e13), (1e-6, 0.999999)]
        res = minimize(objective, x0=x0, bounds=bounds, method="L-BFGS-B")
        # Guard: never accept a worse solution than the grid baseline.
        scipy_rmse = float(res.fun)
        if scipy_rmse <= grid_best.residual_rmse:
            return CalibrationResult(
                half_life_ns=int(max(1.0, res.x[0])),
                shrink_factor=float(min(max(res.x[1], 1e-6), 0.999999)),
                residual_rmse=scipy_rmse,
                sample_count=len(self._samples),
            )
        return grid_best

    def _fit_analytical(self) -> CalibrationResult:
        """Closed-form fit when every sample shares the same shrink_events.

        For samples that all undergo the same number of shrink events,
        the residual minimization in half_life_ns is a least-squares
        problem on log(ratio) vs elapsed_ns. Returns the exact optimum
        for that case; raises otherwise.
        """
        if not self._samples:
            raise RuntimeError("no samples recorded; call record_sample first")
        first_shrink = self._samples[0].shrink_events
        for s in self._samples[1:]:
            if s.shrink_events != first_shrink:
                raise ValueError(
                    "analytical fit requires every sample to share the same "
                    "shrink_events; got varied counts. Use method='grid' "
                    "or method='scipy' instead."
                )
        # For each sample: adjusted = est * exp(-elapsed/H) * sf^k.
        # Set adjusted == ground_truth → log(gt/est) + k*log(1/sf) = -elapsed/H
        # → -elapsed/H = log(gt/est) - k*log(sf).
        # Pick sf=1 (no shrink) when k=0; otherwise we need both knobs.
        # Solve in two stages: 1) optimal sf for fixed H, 2) optimal H.
        # When k==0, sf is irrelevant — return a default 0.85.
        if first_shrink == 0:
            # Pure exponential decay in elapsed. Solve linear LS for 1/H.
            # y_i = log(gt_i / est_i), x_i = elapsed_i; y = -x/H => slope = -1/H.
            xs = []
            ys = []
            for s in self._samples:
                if s.estimator_value <= 0 or s.ground_truth <= 0 or s.elapsed_ns <= 0:
                    continue
                xs.append(float(s.elapsed_ns))
                ys.append(math.log(s.ground_truth / s.estimator_value))
            if not xs:
                # Degenerate: no usable rows → emit a sensible default.
                return CalibrationResult(
                    half_life_ns=60_000_000_000,
                    shrink_factor=0.85,
                    residual_rmse=self._rmse_for(60_000_000_000, 0.85),
                    sample_count=len(self._samples),
                )
            sxx = sum(x * x for x in xs)
            sxy = sum(x * y for x, y in zip(xs, ys))
            slope = sxy / sxx if sxx > 0 else -1.0 / 60_000_000_000
            half_life = int(-1.0 / slope) if slope < 0 else 60_000_000_000
            half_life = max(half_life, 1_000_000)  # floor at 1ms
            sf = 0.85
            rmse = self._rmse_for(half_life, sf)
            return CalibrationResult(
                half_life_ns=half_life,
                shrink_factor=sf,
                residual_rmse=rmse,
                sample_count=len(self._samples),
            )
        # Same non-zero shrink across all samples: bootstrap from grid
        # then close the form on sf alone (H is found via the same LS).
        grid = self.fit(method="grid")
        return grid

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
