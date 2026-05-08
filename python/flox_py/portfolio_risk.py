"""Portfolio-level risk aggregator for multi-strategy setups.

Per-strategy risk hooks are first-class in flox; this module is the
layer above. It aggregates PnL, gross / net exposure, and trade
counts across N registered strategies, applies portfolio-level rules
(max drawdown, max gross exposure, max position concentration),
and publishes a single kill-switch signal back to the engine.

Phase 1 is single-process and in-memory. The aggregator now lives in
the C++ engine; this module is the Python surface, with the same
dataclass shape it always had.
"""
from __future__ import annotations

import threading
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional


_FIELD_TO_BIT: Dict[str, int] = {
    "realized_pnl": 1 << 0,
    "unrealized_pnl": 1 << 1,
    "fees": 1 << 2,
    "gross_exposure": 1 << 3,
    "net_exposure": 1 << 4,
    "trade_count": 1 << 5,
}


@dataclass
class StrategyAccount:
    name: str
    realized_pnl: float = 0.0
    unrealized_pnl: float = 0.0
    fees: float = 0.0
    gross_exposure: float = 0.0
    net_exposure: float = 0.0
    trade_count: int = 0

    @property
    def daily_pnl(self) -> float:
        return self.realized_pnl + self.unrealized_pnl + self.fees


@dataclass
class RiskRules:
    max_drawdown_pct: Optional[float] = None
    max_daily_loss: Optional[float] = None
    max_gross_exposure: Optional[float] = None
    max_concentration_pct: Optional[float] = None


@dataclass
class Breach:
    rule: str
    value: float
    limit: float
    detail: str = ""


@dataclass
class PortfolioSnapshot:
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


def _account_from_dict(d: dict) -> StrategyAccount:
    return StrategyAccount(
        name=d["name"],
        realized_pnl=float(d["realized_pnl"]),
        unrealized_pnl=float(d["unrealized_pnl"]),
        fees=float(d["fees"]),
        gross_exposure=float(d["gross_exposure"]),
        net_exposure=float(d["net_exposure"]),
        trade_count=int(d["trade_count"]),
    )


def _breach_from_dict(d: dict) -> Breach:
    return Breach(
        rule=str(d["rule"]),
        value=float(d["value"]),
        limit=float(d["limit"]),
        detail=str(d.get("detail", "")),
    )


def _build_native_rules(rules: RiskRules) -> Any:
    from flox_py._flox_py import _PortfolioRiskRules  # type: ignore[attr-defined]

    n = _PortfolioRiskRules()
    n.max_drawdown_pct = rules.max_drawdown_pct
    n.max_daily_loss = rules.max_daily_loss
    n.max_gross_exposure = rules.max_gross_exposure
    n.max_concentration_pct = rules.max_concentration_pct
    return n


class PortfolioRiskAggregator:
    """Single-process portfolio risk aggregator. Delegates to the
    C++-backed implementation in ``flox_py._flox_py`` and preserves
    the dataclass-friendly Python surface."""

    def __init__(
        self,
        *,
        rules: Optional[RiskRules] = None,
        initial_equity: float = 0.0,
        on_breach: Optional[KillSwitchCallback] = None,
    ) -> None:
        from flox_py._flox_py import _PortfolioRiskAggregator  # type: ignore[attr-defined]

        self._rules = rules or RiskRules()
        self._on_breach = on_breach
        self._native = _PortfolioRiskAggregator(
            _build_native_rules(self._rules), float(initial_equity),
        )
        self._kill_switch_seen = False
        self._callback_lock = threading.Lock()

    def update(self, name: str, **fields: float) -> None:
        valid = set(_FIELD_TO_BIT.keys())
        unknown = set(fields.keys()) - valid
        if unknown:
            raise AttributeError(
                f"unknown StrategyAccount field {next(iter(unknown))!r}"
            )
        mask = 0
        for k in fields:
            mask |= _FIELD_TO_BIT[k]

        kwargs: Dict[str, float] = {
            "realized_pnl": float(fields.get("realized_pnl", 0.0)),
            "unrealized_pnl": float(fields.get("unrealized_pnl", 0.0)),
            "fees": float(fields.get("fees", 0.0)),
            "gross_exposure": float(fields.get("gross_exposure", 0.0)),
            "net_exposure": float(fields.get("net_exposure", 0.0)),
            "trade_count": int(fields.get("trade_count", 0)),
        }
        self._native.update(name=name, field_mask=mask, **kwargs)
        self._maybe_fire_breach()

    def remove(self, name: str) -> None:
        self._native.remove(name)
        self._maybe_fire_breach()

    def reset_kill_switch(self) -> None:
        self._native.reset_kill_switch()
        with self._callback_lock:
            self._kill_switch_seen = False

    def check_order(
        self,
        *,
        strategy: str,
        notional: float,
        side: str,
    ) -> Optional[Breach]:
        result = self._native.check_order(strategy, float(notional), side)
        if result is None:
            return None
        return _breach_from_dict(result)

    def snapshot(self) -> PortfolioSnapshot:
        d = self._native.snapshot()
        accounts = [_account_from_dict(a) for a in d["accounts"]]
        breaches = [_breach_from_dict(b) for b in d["active_breaches"]]
        return PortfolioSnapshot(
            total_realized_pnl=float(d["total_realized_pnl"]),
            total_unrealized_pnl=float(d["total_unrealized_pnl"]),
            total_fees=float(d["total_fees"]),
            total_daily_pnl=float(d["total_daily_pnl"]),
            total_gross_exposure=float(d["total_gross_exposure"]),
            total_net_exposure=float(d["total_net_exposure"]),
            peak_equity=float(d["peak_equity"]),
            drawdown_pct=float(d["drawdown_pct"]),
            accounts=accounts,
            kill_switch_active=bool(d["kill_switch_active"]),
            breaches=breaches,
        )

    def _maybe_fire_breach(self) -> None:
        if self._on_breach is None:
            return
        snap = self.snapshot()
        if not snap.kill_switch_active or not snap.breaches:
            return
        with self._callback_lock:
            if self._kill_switch_seen:
                return
            self._kill_switch_seen = True
        try:
            self._on_breach(list(snap.breaches))
        except Exception:
            pass


__all__ = [
    "Breach",
    "KillSwitchCallback",
    "PortfolioRiskAggregator",
    "PortfolioSnapshot",
    "RiskRules",
    "StrategyAccount",
]
