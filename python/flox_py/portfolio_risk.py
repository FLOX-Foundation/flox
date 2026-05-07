"""Portfolio-level risk aggregator for multi-strategy setups.

Per-strategy risk hooks are first-class in flox; this module is the
layer above. It aggregates PnL, gross / net exposure, and trade
counts across N registered strategies, applies portfolio-level rules
(max drawdown, max gross exposure, max position concentration),
and publishes a single kill-switch signal back to the engine.

Phase 1 is single-process and in-memory. The user's app instantiates
one ``PortfolioRiskAggregator``, registers each strategy's view, and
either polls ``snapshot()`` or wires the aggregator to call back on
breach. Multi-process aggregation through shared state is a Phase 2
concern; the API is shaped so the same call-site code works once a
shared backend lands.

Why this exists in flox: the open-source gap is real. Per-strategy
daily-loss limits are common; portfolio-level caps that span
strategies and accounts are commercial-only.
"""
from __future__ import annotations

import threading
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Mapping, Optional


@dataclass
class StrategyAccount:
    """One row in the aggregator's view. The user's app fills these
    in from whichever bookkeeping it already does (PnLTracker hook,
    position state, etc) and feeds them through ``update``."""

    name: str
    realized_pnl: float = 0.0
    unrealized_pnl: float = 0.0
    fees: float = 0.0
    gross_exposure: float = 0.0   # sum of |qty * price| across positions
    net_exposure: float = 0.0     # signed: long minus short
    trade_count: int = 0

    @property
    def daily_pnl(self) -> float:
        return self.realized_pnl + self.unrealized_pnl + self.fees


@dataclass
class RiskRules:
    """Portfolio-level limits. Any breach flips the kill switch."""

    max_drawdown_pct: Optional[float] = None
    """Trip when ``(peak_equity - current_equity) / peak_equity`` exceeds
    this value. Pass ``0.20`` for 20 percent drawdown."""

    max_daily_loss: Optional[float] = None
    """Absolute loss cap on combined daily PnL. Pass a negative
    threshold or a positive number; the magnitude is what matters."""

    max_gross_exposure: Optional[float] = None
    """Total gross exposure across every registered strategy."""

    max_concentration_pct: Optional[float] = None
    """Trip when any single strategy contributes more than this share
    of the gross exposure. Useful for catching one strategy that
    silently doubles up."""


@dataclass
class Breach:
    """Why the kill switch tripped."""

    rule: str
    value: float
    limit: float
    detail: str = ""


@dataclass
class PortfolioSnapshot:
    """Read-only view of the current aggregate."""

    total_realized_pnl: float
    total_unrealized_pnl: float
    total_fees: float
    total_daily_pnl: float
    total_gross_exposure: float
    total_net_exposure: float
    peak_equity: float
    drawdown_pct: float
    accounts: List[StrategyAccount]
    kill_switch_active: bool
    breaches: List[Breach] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {
            "total_realized_pnl": self.total_realized_pnl,
            "total_unrealized_pnl": self.total_unrealized_pnl,
            "total_fees": self.total_fees,
            "total_daily_pnl": self.total_daily_pnl,
            "total_gross_exposure": self.total_gross_exposure,
            "total_net_exposure": self.total_net_exposure,
            "peak_equity": self.peak_equity,
            "drawdown_pct": self.drawdown_pct,
            "accounts": [
                {
                    "name": a.name,
                    "realized_pnl": a.realized_pnl,
                    "unrealized_pnl": a.unrealized_pnl,
                    "fees": a.fees,
                    "daily_pnl": a.daily_pnl,
                    "gross_exposure": a.gross_exposure,
                    "net_exposure": a.net_exposure,
                    "trade_count": a.trade_count,
                }
                for a in self.accounts
            ],
            "kill_switch_active": self.kill_switch_active,
            "breaches": [
                {"rule": b.rule, "value": b.value,
                 "limit": b.limit, "detail": b.detail}
                for b in self.breaches
            ],
        }


KillSwitchCallback = Callable[[List[Breach]], None]


class PortfolioRiskAggregator:
    """Single-process, in-memory portfolio risk aggregator.

    Typical usage::

        aggregator = PortfolioRiskAggregator(
            rules=RiskRules(max_drawdown_pct=0.20, max_daily_loss=10_000),
            initial_equity=100_000,
            on_breach=my_kill_switch.activate,
        )
        aggregator.update("ema-trend", realized_pnl=120.0,
                          unrealized_pnl=-30.0, gross_exposure=5000.0,
                          net_exposure=4500.0, trade_count=12)
        snap = aggregator.snapshot()
        if snap.kill_switch_active:
            ...

    All methods are thread-safe. ``snapshot()`` is the canonical read
    primitive; it returns a deep copy so the caller can JSON-serialize
    or hand it to a UI without holding the lock.
    """

    def __init__(
        self,
        *,
        rules: Optional[RiskRules] = None,
        initial_equity: float = 0.0,
        on_breach: Optional[KillSwitchCallback] = None,
    ) -> None:
        self._rules = rules or RiskRules()
        self._initial_equity = float(initial_equity)
        self._peak_equity = float(initial_equity)
        self._accounts: Dict[str, StrategyAccount] = {}
        self._kill_switch_active = False
        self._on_breach = on_breach
        self._lock = threading.Lock()

    # ── Mutators (thread-safe) ────────────────────────────────────

    def update(self, name: str, **fields: float) -> None:
        """Upsert a strategy's view. Pass any subset of
        ``StrategyAccount`` fields by keyword; missing fields keep
        their previous value, missing strategy creates a new row."""
        with self._lock:
            row = self._accounts.get(name)
            if row is None:
                row = StrategyAccount(name=name)
                self._accounts[name] = row
            for k, v in fields.items():
                if not hasattr(row, k):
                    raise AttributeError(
                        f"unknown StrategyAccount field {k!r}"
                    )
                setattr(row, k, type(getattr(row, k))(v))
            self._reevaluate_locked()

    def remove(self, name: str) -> None:
        with self._lock:
            self._accounts.pop(name, None)
            self._reevaluate_locked()

    def reset_kill_switch(self) -> None:
        """Manual override after a breach has been investigated. The
        next ``update`` re-evaluates and re-trips if the cause is
        still present."""
        with self._lock:
            self._kill_switch_active = False

    # ── Pre-trade check ──────────────────────────────────────────

    def check_order(
        self,
        *,
        strategy: str,
        notional: float,
        side: str,
    ) -> Optional[Breach]:
        """Inspect a candidate order against the aggregated state.
        Returns ``None`` if the order is allowed, or a ``Breach``
        describing why it must be rejected. Does not mutate state."""
        with self._lock:
            if self._kill_switch_active:
                return Breach(
                    rule="kill_switch_active",
                    value=1.0,
                    limit=0.0,
                    detail="portfolio kill switch is engaged",
                )
            if self._rules.max_gross_exposure is not None:
                proposed = self._total_gross_locked() + abs(float(notional))
                if proposed > self._rules.max_gross_exposure:
                    return Breach(
                        rule="max_gross_exposure",
                        value=proposed,
                        limit=self._rules.max_gross_exposure,
                        detail=(
                            f"order would push gross exposure to "
                            f"{proposed:.2f} (cap {self._rules.max_gross_exposure:.2f})"
                        ),
                    )
        return None

    # ── Read primitive ───────────────────────────────────────────

    def snapshot(self) -> PortfolioSnapshot:
        with self._lock:
            return self._build_snapshot_locked()

    # ── Internal ─────────────────────────────────────────────────

    def _total_gross_locked(self) -> float:
        return sum(a.gross_exposure for a in self._accounts.values())

    def _total_daily_pnl_locked(self) -> float:
        return sum(a.daily_pnl for a in self._accounts.values())

    def _current_equity_locked(self) -> float:
        return self._initial_equity + self._total_daily_pnl_locked()

    def _drawdown_pct_locked(self) -> float:
        if self._peak_equity <= 0:
            return 0.0
        return max(0.0, (self._peak_equity - self._current_equity_locked()) / self._peak_equity)

    def _breaches_locked(self) -> List[Breach]:
        breaches: List[Breach] = []
        cur_equity = self._current_equity_locked()
        if cur_equity > self._peak_equity:
            self._peak_equity = cur_equity

        if self._rules.max_drawdown_pct is not None:
            dd = self._drawdown_pct_locked()
            if dd > self._rules.max_drawdown_pct:
                breaches.append(Breach(
                    rule="max_drawdown_pct",
                    value=dd,
                    limit=self._rules.max_drawdown_pct,
                    detail=(
                        f"drawdown {dd:.4f} exceeds limit "
                        f"{self._rules.max_drawdown_pct:.4f}"
                    ),
                ))

        if self._rules.max_daily_loss is not None:
            cap = abs(self._rules.max_daily_loss)
            pnl = self._total_daily_pnl_locked()
            if pnl < 0 and abs(pnl) > cap:
                breaches.append(Breach(
                    rule="max_daily_loss",
                    value=pnl,
                    limit=-cap,
                    detail=f"daily loss {pnl:.2f} exceeds cap {cap:.2f}",
                ))

        if self._rules.max_gross_exposure is not None:
            gross = self._total_gross_locked()
            if gross > self._rules.max_gross_exposure:
                breaches.append(Breach(
                    rule="max_gross_exposure",
                    value=gross,
                    limit=self._rules.max_gross_exposure,
                    detail=(
                        f"gross exposure {gross:.2f} exceeds cap "
                        f"{self._rules.max_gross_exposure:.2f}"
                    ),
                ))

        # Concentration only makes sense when more than one strategy
        # carries gross exposure. With a single contributor the share
        # is by definition 1.0 and the check would trip at startup
        # before any second strategy registers.
        if self._rules.max_concentration_pct is not None:
            contributors = [
                a for a in self._accounts.values() if a.gross_exposure > 0
            ]
            if len(contributors) >= 2:
                total = sum(a.gross_exposure for a in contributors)
                worst = max(contributors, key=lambda a: a.gross_exposure)
                share = worst.gross_exposure / total
                if share > self._rules.max_concentration_pct:
                    breaches.append(Breach(
                        rule="max_concentration_pct",
                        value=share,
                        limit=self._rules.max_concentration_pct,
                        detail=(
                            f"strategy {worst.name!r} holds "
                            f"{share:.4f} of gross (cap "
                            f"{self._rules.max_concentration_pct:.4f})"
                        ),
                    ))
        return breaches

    def _reevaluate_locked(self) -> None:
        breaches = self._breaches_locked()
        if breaches and not self._kill_switch_active:
            self._kill_switch_active = True
            if self._on_breach is not None:
                # Hold the lock briefly; user callback should be
                # cheap. Heavy work belongs in a separate consumer.
                try:
                    self._on_breach(list(breaches))
                except Exception:  # noqa: BLE001 — never let user callback crash the aggregator
                    pass

    def _build_snapshot_locked(self) -> PortfolioSnapshot:
        accounts = [
            StrategyAccount(
                name=a.name,
                realized_pnl=a.realized_pnl,
                unrealized_pnl=a.unrealized_pnl,
                fees=a.fees,
                gross_exposure=a.gross_exposure,
                net_exposure=a.net_exposure,
                trade_count=a.trade_count,
            )
            for a in self._accounts.values()
        ]
        return PortfolioSnapshot(
            total_realized_pnl=sum(a.realized_pnl for a in accounts),
            total_unrealized_pnl=sum(a.unrealized_pnl for a in accounts),
            total_fees=sum(a.fees for a in accounts),
            total_daily_pnl=self._total_daily_pnl_locked(),
            total_gross_exposure=self._total_gross_locked(),
            total_net_exposure=sum(a.net_exposure for a in accounts),
            peak_equity=self._peak_equity,
            drawdown_pct=self._drawdown_pct_locked(),
            accounts=accounts,
            kill_switch_active=self._kill_switch_active,
            breaches=self._breaches_locked(),
        )


__all__ = [
    "Breach",
    "KillSwitchCallback",
    "PortfolioRiskAggregator",
    "PortfolioSnapshot",
    "RiskRules",
    "StrategyAccount",
]
