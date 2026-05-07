"""Latency models for backtest realism.

Per-trade fills in flox today work in instant mode: an order created
at time T sees the next observed trade as its fill. That is fine
for bar-driven strategies on minute-or-larger timeframes. It is
unrealistic for market-making, latency arbitrage, and any HFT-style
work where the gap between event arrival, decision, and round-trip
to the exchange determines whether a fill happens at all.

This module exposes a small ``LatencyModel`` interface plus four
implementations: ``ConstantLatency``, ``GaussianLatency``,
``ExponentialLatency``, and ``EmpiricalLatency``. Each samples three
independent components in nanoseconds: ``feed`` (event arrival ->
engine), ``order`` (engine submit -> exchange), and ``fill``
(exchange match -> engine notification).

Phase 1 scope is the sampling primitive itself. The user app pulls
samples and applies them to its observed-trade timestamps and
order-submit timestamps before feeding them through
``flox_py.SimulatedExecutor``. Engine-level integration (where
``BacktestConfig.latency`` plumbs the same primitive deeper into
``SimulatedExecutor`` automatically) is Phase 2 follow-up.
"""
from __future__ import annotations

import random
from dataclasses import dataclass, field
from typing import List, Optional, Sequence


@dataclass
class LatencySample:
    """One draw from a ``LatencyModel`` covering all three components."""

    feed_ns: int
    order_ns: int
    fill_ns: int

    def to_dict(self) -> dict:
        return {
            "feed_ns": int(self.feed_ns),
            "order_ns": int(self.order_ns),
            "fill_ns": int(self.fill_ns),
        }


class LatencyModel:
    """Abstract base. Sub-classes implement
    ``feed_delay() / order_delay() / fill_delay()`` returning a
    non-negative integer number of nanoseconds. ``sample()`` is
    provided as a convenience for batch use."""

    def feed_delay(self) -> int:
        raise NotImplementedError

    def order_delay(self) -> int:
        raise NotImplementedError

    def fill_delay(self) -> int:
        raise NotImplementedError

    def sample(self) -> LatencySample:
        return LatencySample(
            feed_ns=int(self.feed_delay()),
            order_ns=int(self.order_delay()),
            fill_ns=int(self.fill_delay()),
        )

    def reset(self, seed: Optional[int] = None) -> None:
        """Re-seed the underlying RNG for reproducible sampling.
        The default base does not own an RNG; sub-classes that do
        override this."""
        # Intentional no-op; ConstantLatency has no RNG either.
        pass


@dataclass
class ConstantLatency(LatencyModel):
    """Returns the same nanoseconds every call. Useful as a baseline."""

    feed_ns: int = 0
    order_ns: int = 0
    fill_ns: int = 0

    def __post_init__(self) -> None:
        for k in ("feed_ns", "order_ns", "fill_ns"):
            v = getattr(self, k)
            if v < 0:
                raise ValueError(f"{k} must be non-negative; got {v}")

    def feed_delay(self) -> int:
        return int(self.feed_ns)

    def order_delay(self) -> int:
        return int(self.order_ns)

    def fill_delay(self) -> int:
        return int(self.fill_ns)


@dataclass
class GaussianLatency(LatencyModel):
    """Independent normal samples per component, clamped to non-negative.

    Pass ``mean`` and ``stddev`` per component in nanoseconds. Negative
    samples are clamped to zero (latencies cannot be negative; if your
    measured distribution wants negative tails you are not modelling
    latency)."""

    feed_mean_ns: float = 0.0
    feed_stddev_ns: float = 0.0
    order_mean_ns: float = 0.0
    order_stddev_ns: float = 0.0
    fill_mean_ns: float = 0.0
    fill_stddev_ns: float = 0.0
    seed: Optional[int] = None
    _rng: random.Random = field(init=False, repr=False)

    def __post_init__(self) -> None:
        self._rng = random.Random(self.seed)

    def reset(self, seed: Optional[int] = None) -> None:
        self._rng = random.Random(seed if seed is not None else self.seed)

    def _draw(self, mean: float, stddev: float) -> int:
        if stddev <= 0.0:
            return max(0, int(mean))
        return max(0, int(self._rng.gauss(mean, stddev)))

    def feed_delay(self) -> int:
        return self._draw(self.feed_mean_ns, self.feed_stddev_ns)

    def order_delay(self) -> int:
        return self._draw(self.order_mean_ns, self.order_stddev_ns)

    def fill_delay(self) -> int:
        return self._draw(self.fill_mean_ns, self.fill_stddev_ns)


@dataclass
class ExponentialLatency(LatencyModel):
    """Exponential distribution per component. Rate is ``1 / mean``;
    pass ``feed_mean_ns`` etc. Heavy right tail is the default for
    network-bound latency; use this when the empirical histogram
    looks one-sided."""

    feed_mean_ns: float = 0.0
    order_mean_ns: float = 0.0
    fill_mean_ns: float = 0.0
    seed: Optional[int] = None
    _rng: random.Random = field(init=False, repr=False)

    def __post_init__(self) -> None:
        for k in ("feed_mean_ns", "order_mean_ns", "fill_mean_ns"):
            v = getattr(self, k)
            if v < 0.0:
                raise ValueError(f"{k} must be non-negative; got {v}")
        self._rng = random.Random(self.seed)

    def reset(self, seed: Optional[int] = None) -> None:
        self._rng = random.Random(seed if seed is not None else self.seed)

    def _draw(self, mean: float) -> int:
        if mean <= 0.0:
            return 0
        # Exponential with rate 1/mean.
        return max(0, int(self._rng.expovariate(1.0 / mean)))

    def feed_delay(self) -> int:
        return self._draw(self.feed_mean_ns)

    def order_delay(self) -> int:
        return self._draw(self.order_mean_ns)

    def fill_delay(self) -> int:
        return self._draw(self.fill_mean_ns)


@dataclass
class EmpiricalLatency(LatencyModel):
    """Resample with replacement from observed values. Pass three
    arrays of measured latencies (one per component); each call
    draws a uniform random index. This is the right model when you
    have a recording of live latencies and want backtest realism
    that matches the actual distribution shape, including any
    bimodality or fat tails."""

    feed_samples: Sequence[int] = field(default_factory=list)
    order_samples: Sequence[int] = field(default_factory=list)
    fill_samples: Sequence[int] = field(default_factory=list)
    seed: Optional[int] = None
    _rng: random.Random = field(init=False, repr=False)

    def __post_init__(self) -> None:
        if not (
            self.feed_samples or self.order_samples or self.fill_samples
        ):
            raise ValueError(
                "EmpiricalLatency needs at least one of "
                "feed_samples / order_samples / fill_samples"
            )
        for name in ("feed_samples", "order_samples", "fill_samples"):
            arr = getattr(self, name)
            for v in arr:
                if int(v) < 0:
                    raise ValueError(f"{name} must contain non-negative ints; got {v}")
        self._rng = random.Random(self.seed)

    def reset(self, seed: Optional[int] = None) -> None:
        self._rng = random.Random(seed if seed is not None else self.seed)

    def _draw(self, samples: Sequence[int]) -> int:
        if not samples:
            return 0
        idx = self._rng.randrange(len(samples))
        return max(0, int(samples[idx]))

    def feed_delay(self) -> int:
        return self._draw(self.feed_samples)

    def order_delay(self) -> int:
        return self._draw(self.order_samples)

    def fill_delay(self) -> int:
        return self._draw(self.fill_samples)


def calibrate_from_samples(
    *,
    feed_samples: Sequence[int],
    order_samples: Sequence[int],
    fill_samples: Sequence[int],
    seed: Optional[int] = None,
) -> EmpiricalLatency:
    """Convenience constructor: wrap measured arrays into an
    ``EmpiricalLatency``. The arrays are stored by reference, so do
    not mutate them after handing them over."""
    return EmpiricalLatency(
        feed_samples=list(feed_samples),
        order_samples=list(order_samples),
        fill_samples=list(fill_samples),
        seed=seed,
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
