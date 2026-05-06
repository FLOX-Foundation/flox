"""Zero-Lag Exponential Moving Average (ZLEMA).

Reference: Ehlers, J. F. (2002). "Zero-Lag (well, Almost)."

ZLEMA reduces the lag of a standard EMA by feeding the EMA a phase-
corrected sample ``error = price + (price - price[lag])`` where
``lag = (period - 1) / 2``. The corrected sample compensates for the
EMA's average lag, producing a smoothed line that tracks turns more
closely than EMA at the same period.

Streaming contract — single ``update(price)`` call per new sample,
``None`` until ``period + lag`` samples have been seen, then a value.
"""
from __future__ import annotations

from collections import deque
from typing import Deque, Optional


class ZLEMA:
    """Streaming Zero-Lag EMA.

    Parameters
    ----------
    period:
        EMA period in samples. Must be >= 2.

    Examples
    --------
    >>> z = ZLEMA(10)
    >>> for price in (100, 101, 102, 103, 104):
    ...     out = z.update(price)
    >>> z.ready  # warming up while period+lag samples accumulate
    False
    """

    __slots__ = ("period", "_lag", "_alpha", "_buf", "_value")

    def __init__(self, period: int):
        if period < 2:
            raise ValueError(f"ZLEMA period must be >= 2 (got {period})")
        self.period: int = int(period)
        self._lag: int = (self.period - 1) // 2
        self._alpha: float = 2.0 / (self.period + 1)
        # ring buffer of size lag + 1 — index 0 is current price,
        # index lag is the lagged price for the error correction.
        self._buf: Deque[float] = deque(maxlen=self._lag + 1)
        self._value: Optional[float] = None

    def update(self, price: float) -> Optional[float]:
        self._buf.append(float(price))
        if len(self._buf) <= self._lag:
            return None
        lagged = self._buf[0]
        corrected = 2.0 * price - lagged
        if self._value is None:
            self._value = corrected
        else:
            self._value = self._alpha * corrected + (1.0 - self._alpha) * self._value
        return self._value

    @property
    def ready(self) -> bool:
        return self._value is not None

    @property
    def value(self) -> Optional[float]:
        return self._value

    def reset(self) -> None:
        self._buf.clear()
        self._value = None
