"""Latency models for backtest realism.

Phase 1 sampling primitive backed by the C++ engine. The four
distributions and the LatencySample struct live in
``flox_py._flox_py`` (compiled extension); this module re-exports them
so ``flox_py.latency_models.GaussianLatency`` keeps working unchanged.

Each model samples three independent components in nanoseconds:
``feed`` (event arrival -> engine), ``order`` (engine submit ->
exchange), and ``fill`` (exchange match -> engine notification). The
user app pulls samples and applies them to its observed timestamps
before feeding orders into ``flox_py.SimulatedExecutor``.
"""
from __future__ import annotations

from typing import Optional, Sequence

from flox_py._flox_py import (
    ConstantLatency,
    EmpiricalLatency,
    ExponentialLatency,
    GaussianLatency,
    LatencyModel,
    LatencySample,
)


def calibrate_from_samples(
    *,
    feed_samples: Sequence[int],
    order_samples: Sequence[int],
    fill_samples: Sequence[int],
    seed: Optional[int] = None,
) -> EmpiricalLatency:
    """Wrap measured arrays into an ``EmpiricalLatency``."""
    return EmpiricalLatency(
        feed_samples=list(feed_samples),
        order_samples=list(order_samples),
        fill_samples=list(fill_samples),
        seed=int(seed) if seed is not None else 0,
    )


__all__ = [
    "LatencyModel",
    "LatencySample",
    "ConstantLatency",
    "GaussianLatency",
    "ExponentialLatency",
    "EmpiricalLatency",
    "calibrate_from_samples",
]
