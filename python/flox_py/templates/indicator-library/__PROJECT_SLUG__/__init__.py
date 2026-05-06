"""__PROJECT_NAME__ — custom indicator library for FLOX.

Drop new indicator classes next to ``zlema.py`` and re-export them
here so users can ``from __PROJECT_SLUG__ import MyIndicator``.

Each indicator follows the streaming contract used by FLOX built-ins:

* ``update(value: float) -> Optional[float]`` — feed a new sample,
  return the current indicator value, or ``None`` while warming up.
* ``ready: bool`` — ``True`` once the indicator has produced its first
  non-``None`` value.
* ``value: Optional[float]`` — the most recent computed value.

Strategies consume any object that satisfies that contract — see
``examples/use_in_strategy.py`` for an end-to-end example.
"""
from __future__ import annotations

from .zlema import ZLEMA

__all__ = ["ZLEMA"]
__version__ = "0.1.0"
