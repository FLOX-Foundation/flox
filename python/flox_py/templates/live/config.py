"""Pydantic config schema for __PROJECT_NAME__.

Loads ``config.toml`` (or the path set via ``__PROJECT_PREFIX___CONFIG``)
into a typed ``LiveConfig`` model. Validation errors fail fast at
startup rather than midway through a live session.

The ``dry_run`` and ``sandbox`` defaults are intentionally
conservative — flip both off only after you have verified the
strategy works on testnet keys.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import List, Literal

try:
    import tomllib  # Python 3.11+
except ModuleNotFoundError:  # pragma: no cover
    import tomli as tomllib  # type: ignore[no-redef]

from pydantic import BaseModel, Field, field_validator


class LiveConfig(BaseModel):
    """Validated live-trading config. See config.toml for defaults."""

    # ── Connection ─────────────────────────────────────────────────────
    exchange: str = "binance"
    sandbox: bool = True
    env_prefix: str = "__PROJECT_PREFIX__"

    # ── Trading control ────────────────────────────────────────────────
    dry_run: bool = True
    symbols: List[str] = Field(default_factory=lambda: ["BTC/USDT"])
    streams: List[Literal["trades", "book", "orders"]] = Field(
        default_factory=lambda: ["trades", "orders"]
    )
    book_depth: int = 20
    reconcile_positions: bool = True

    # ── Strategy params ────────────────────────────────────────────────
    fast_period: int = 10
    slow_period: int = 30
    order_qty: float = 0.001

    # ── Backoff (passed to CcxtBroker) ─────────────────────────────────
    retry_initial_delay: float = 1.0
    retry_max_delay: float = 60.0
    retry_multiplier: float = 2.0

    # ── Observability ──────────────────────────────────────────────────
    log_level: str = "INFO"

    @field_validator("symbols")
    @classmethod
    def _non_empty_symbols(cls, v):
        if not v:
            raise ValueError("symbols must not be empty")
        return v

    @field_validator("fast_period", "slow_period")
    @classmethod
    def _positive_period(cls, v):
        if v <= 0:
            raise ValueError("period must be > 0")
        return v

    @field_validator("order_qty")
    @classmethod
    def _positive_qty(cls, v):
        if v <= 0:
            raise ValueError("order_qty must be > 0")
        return v


def load_config(path: str | Path | None = None) -> LiveConfig:
    """Load config from ``path`` (or ``__PROJECT_PREFIX___CONFIG``, or
    ``./config.toml``).
    """
    if path is None:
        path = os.environ.get("__PROJECT_PREFIX___CONFIG", "config.toml")
    p = Path(path)
    if not p.exists():
        print(f"config file not found: {p}\n"
              f"copy config.toml.example to config.toml or set "
              f"__PROJECT_PREFIX___CONFIG", file=sys.stderr)
        sys.exit(2)

    with p.open("rb") as f:
        data = tomllib.load(f)

    # Honor FLOX_LIVE env var as a global override on dry_run.
    if os.environ.get("FLOX_LIVE", "0") == "1":
        data["dry_run"] = False

    try:
        return LiveConfig(**data)
    except Exception as e:
        print(f"config validation failed:\n{e}", file=sys.stderr)
        sys.exit(2)
