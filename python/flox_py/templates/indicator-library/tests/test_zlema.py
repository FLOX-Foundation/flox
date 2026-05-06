"""Unit tests for the ZLEMA indicator."""
from __future__ import annotations

import csv
import math
import os
from pathlib import Path

import pytest

from __PROJECT_SLUG__ import ZLEMA


HERE = Path(__file__).resolve().parent
SAMPLE_CSV = HERE / "data" / "btcusdt_sample.csv"


def _load_closes() -> list[float]:
    closes: list[float] = []
    with SAMPLE_CSV.open() as f:
        reader = csv.DictReader(f)
        for row in reader:
            closes.append(float(row["close"]))
    return closes


def test_period_must_be_at_least_two() -> None:
    with pytest.raises(ValueError):
        ZLEMA(1)


def test_warmup_returns_none() -> None:
    z = ZLEMA(10)
    # Lag = (10-1)//2 = 4. First lag samples return None.
    for price in [100.0, 101.0, 102.0, 103.0]:
        assert z.update(price) is None
    assert not z.ready
    assert z.value is None


def test_emits_value_after_warmup() -> None:
    z = ZLEMA(10)
    out: list[float] = []
    for price in [100.0, 101.0, 102.0, 103.0, 104.0, 105.0]:
        v = z.update(price)
        if v is not None:
            out.append(v)
    assert len(out) > 0
    assert z.ready
    assert all(math.isfinite(v) for v in out)


def test_constant_input_converges_to_constant() -> None:
    z = ZLEMA(8)
    last: float = float("nan")
    for _ in range(200):
        v = z.update(50.0)
        if v is not None:
            last = v
    # ZLEMA(constant) → constant exactly (corrected = 2c - c = c).
    assert last == pytest.approx(50.0, abs=1e-9)


def test_lower_lag_than_ema() -> None:
    """ZLEMA(period) should respond faster to a step than EMA(period).

    On a step input from 100 → 110, after the same number of post-step
    samples, ZLEMA should be closer to the new level than the equivalent
    EMA (that's the whole point of the indicator).
    """
    period = 10
    alpha = 2.0 / (period + 1)
    # warmup
    z = ZLEMA(period)
    ema_val = 100.0
    for _ in range(60):
        z.update(100.0)
        ema_val = alpha * 100.0 + (1 - alpha) * ema_val

    # apply step
    zlema_val = None
    for _ in range(period):
        zlema_val = z.update(110.0)
        ema_val = alpha * 110.0 + (1 - alpha) * ema_val

    assert zlema_val is not None
    distance_zlema = abs(110.0 - zlema_val)
    distance_ema = abs(110.0 - ema_val)
    assert distance_zlema < distance_ema, (
        f"ZLEMA={zlema_val!r} (gap {distance_zlema:.4f}) should be closer to "
        f"the step level than EMA={ema_val!r} (gap {distance_ema:.4f})"
    )


def test_runs_against_bundled_sample() -> None:
    closes = _load_closes()
    assert len(closes) >= 100, "sample CSV should have at least 100 bars"
    z = ZLEMA(20)
    emitted = 0
    for price in closes:
        v = z.update(price)
        if v is not None:
            emitted += 1
            assert math.isfinite(v)
    assert emitted >= len(closes) - 20
    assert z.ready


def test_reset_returns_to_warmup_state() -> None:
    z = ZLEMA(5)
    for price in [100.0, 101.0, 102.0, 103.0, 104.0, 105.0]:
        z.update(price)
    assert z.ready
    z.reset()
    assert not z.ready
    assert z.value is None
    assert z.update(50.0) is None  # warming up again
