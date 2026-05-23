"""Tests for the live-queue calibration toolkit."""
import math
import tempfile
from pathlib import Path

import pytest

import flox_py


def _synthetic_samples(calibrator, true_half_life_ns, true_shrink, n=200):
    """Feed samples that follow the true model exactly: ground truth =
    estimator * exp(-elapsed/true_half_life) * true_shrink^events."""
    rng = list(range(n))
    for i in rng:
        estimator_value = 100.0 + (i % 7) * 5.0
        elapsed = (i % 60) * 1_000_000_000  # 0..59s in ns
        events = i % 5
        decay = math.exp(-float(elapsed) / float(true_half_life_ns))
        ground = estimator_value * decay * (true_shrink ** events)
        calibrator.record_sample(
            order_id=i,
            estimator_value=estimator_value,
            ground_truth=ground,
            elapsed_ns=elapsed,
            shrink_events=events,
        )


def test_fit_recovers_synthetic_parameters_within_10_percent():
    cal = flox_py.LiveQueueCalibrator()
    true_hl = 60_000_000_000  # 60s
    true_sf = 0.85
    _synthetic_samples(cal, true_hl, true_sf, n=300)

    result = cal.fit()
    # On the default grid, both half-life and shrink should land on the
    # exact grid points (the synthetic model uses them).
    assert result.half_life_ns == true_hl
    assert abs(result.shrink_factor - true_sf) < 0.06
    # Residual should be tiny because the synthetic model is exact.
    assert result.residual_rmse < 1e-6
    assert result.sample_count == 300


def test_fit_raises_on_empty_samples():
    cal = flox_py.LiveQueueCalibrator()
    with pytest.raises(RuntimeError):
        cal.fit()


def test_clear_resets_sample_count():
    cal = flox_py.LiveQueueCalibrator()
    cal.record_sample(order_id=1, estimator_value=10.0, ground_truth=10.0)
    assert cal.sample_count() == 1
    cal.clear()
    assert cal.sample_count() == 0


def test_export_and_load_roundtrip(tmp_path: Path):
    cal = flox_py.LiveQueueCalibrator()
    _synthetic_samples(cal, 60_000_000_000, 0.85, n=50)
    out = tmp_path / "calibration.json"
    cal.export(out)
    loaded = flox_py.LiveQueueCalibrator.load(out)
    assert loaded.half_life_ns > 0
    assert 0.0 < loaded.shrink_factor < 1.0
    assert loaded.sample_count == 50


def test_apply_pushes_into_attached_estimator():
    est = flox_py.LiveQueuePositionEstimator()
    cal = flox_py.LiveQueueCalibrator(est)
    _synthetic_samples(cal, 30_000_000_000, 0.70, n=50)
    result = cal.fit()
    # apply forwards to set_* on the estimator; the bindings only expose
    # the setters, so verify the call goes through without throwing.
    cal.apply(result)


def test_apply_is_noop_without_estimator():
    cal = flox_py.LiveQueueCalibrator()
    cal.record_sample(order_id=1, estimator_value=10.0, ground_truth=10.0)
    result = cal.fit()
    cal.apply(result)  # no estimator attached: must not raise


def test_test_order_helper_routes_samples_to_calibrator():
    cal = flox_py.LiveQueueCalibrator()
    helper = cal.test_order_helper(symbol=1, size=0.001)
    helper.place(order_id=42, price=50000.0, predicted_ahead=12.5,
                 submit_ns=1_000_000_000)
    helper.record_outcome(filled_at_ns=1_500_000_000, fill_qty=0.001,
                          observed_ahead_at_fill=10.0)
    assert cal.sample_count() == 1


# === T048: scipy + analytical fit methods ===

def test_fit_default_method_is_grid():
    cal = flox_py.LiveQueueCalibrator()
    _synthetic_samples(cal, 60_000_000_000, 0.85, n=50)
    grid = cal.fit()
    explicit_grid = cal.fit(method="grid")
    assert grid.half_life_ns == explicit_grid.half_life_ns
    assert grid.shrink_factor == explicit_grid.shrink_factor


def test_fit_unknown_method_raises():
    cal = flox_py.LiveQueueCalibrator()
    _synthetic_samples(cal, 60_000_000_000, 0.85, n=10)
    with pytest.raises(ValueError):
        cal.fit(method="bogus")


def test_fit_scipy_matches_or_beats_grid_rmse():
    try:
        import scipy  # noqa: F401
    except ImportError:
        pytest.skip("scipy not installed")
    cal = flox_py.LiveQueueCalibrator()
    # Off-grid parameters so scipy has room to improve.
    _synthetic_samples(cal, 45_000_000_000, 0.72, n=300)
    grid = cal.fit(method="grid")
    scipy_res = cal.fit(method="scipy")
    assert scipy_res.residual_rmse <= grid.residual_rmse + 1e-9


def test_fit_scipy_missing_dependency_error_message():
    cal = flox_py.LiveQueueCalibrator()
    cal.record_sample(order_id=1, estimator_value=10.0, ground_truth=10.0,
                       elapsed_ns=1_000_000_000, shrink_events=0)
    try:
        import scipy  # noqa: F401
        # scipy is installed — this test only validates the error
        # message when it's missing; skip otherwise.
        pytest.skip("scipy is installed; error path not exercised")
    except ImportError:
        pass
    with pytest.raises(ImportError, match="scipy"):
        cal.fit(method="scipy")


def test_fit_analytical_zero_shrink_recovers_half_life():
    # All samples zero shrink → closed-form fit on half-life.
    cal = flox_py.LiveQueueCalibrator()
    true_hl = 45_000_000_000  # 45s, off-grid
    for i in range(200):
        elapsed = (i % 50 + 1) * 1_000_000_000
        ground = 100.0 * math.exp(-float(elapsed) / float(true_hl))
        cal.record_sample(order_id=i, estimator_value=100.0,
                           ground_truth=ground, elapsed_ns=elapsed,
                           shrink_events=0)
    res = cal.fit(method="analytical")
    # The closed form should recover the synthetic half-life within a
    # few percent (it's exact on noiseless data; allow a tiny slack
    # for the bound floor and finite samples).
    assert abs(res.half_life_ns - true_hl) / true_hl < 0.05


def test_fit_analytical_mixed_shrink_raises():
    cal = flox_py.LiveQueueCalibrator()
    cal.record_sample(order_id=1, estimator_value=10.0, ground_truth=9.0,
                       elapsed_ns=1_000_000_000, shrink_events=0)
    cal.record_sample(order_id=2, estimator_value=10.0, ground_truth=8.0,
                       elapsed_ns=1_000_000_000, shrink_events=2)
    with pytest.raises(ValueError, match="shrink_events"):
        cal.fit(method="analytical")
