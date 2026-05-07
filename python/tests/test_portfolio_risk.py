"""Tests for ``flox_py.portfolio_risk.PortfolioRiskAggregator``."""
from __future__ import annotations

import sys
import threading
import unittest
from pathlib import Path
from typing import List

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

from flox_py import portfolio_risk as pr  # noqa: E402


class UpdateAndSnapshotTests(unittest.TestCase):
    def test_empty_aggregator_has_zero_totals(self) -> None:
        agg = pr.PortfolioRiskAggregator()
        snap = agg.snapshot()
        self.assertEqual(snap.total_daily_pnl, 0.0)
        self.assertEqual(snap.accounts, [])
        self.assertFalse(snap.kill_switch_active)

    def test_update_adds_strategy_row(self) -> None:
        agg = pr.PortfolioRiskAggregator(initial_equity=100_000)
        agg.update("a", realized_pnl=50.0, unrealized_pnl=10.0,
                   gross_exposure=1000.0, net_exposure=500.0)
        snap = agg.snapshot()
        self.assertEqual(len(snap.accounts), 1)
        self.assertAlmostEqual(snap.total_daily_pnl, 60.0)
        self.assertAlmostEqual(snap.total_gross_exposure, 1000.0)

    def test_partial_update_keeps_other_fields(self) -> None:
        agg = pr.PortfolioRiskAggregator()
        agg.update("a", realized_pnl=100.0, gross_exposure=500.0)
        agg.update("a", unrealized_pnl=50.0)  # only updates unrealized
        snap = agg.snapshot()
        self.assertEqual(snap.accounts[0].realized_pnl, 100.0)
        self.assertEqual(snap.accounts[0].unrealized_pnl, 50.0)
        self.assertEqual(snap.accounts[0].gross_exposure, 500.0)

    def test_unknown_field_raises(self) -> None:
        agg = pr.PortfolioRiskAggregator()
        with self.assertRaises(AttributeError):
            agg.update("a", bogus_field=1.0)

    def test_remove_strategy(self) -> None:
        agg = pr.PortfolioRiskAggregator()
        agg.update("a", gross_exposure=100.0)
        agg.update("b", gross_exposure=200.0)
        agg.remove("a")
        self.assertEqual(len(agg.snapshot().accounts), 1)


class DrawdownTests(unittest.TestCase):
    def test_drawdown_triggers_kill_switch(self) -> None:
        breaches: List[pr.Breach] = []
        agg = pr.PortfolioRiskAggregator(
            rules=pr.RiskRules(max_drawdown_pct=0.10),
            initial_equity=100_000,
            on_breach=lambda b: breaches.extend(b),
        )
        # Push equity up to 110k, then drop to 95k → 13.6% drawdown
        agg.update("a", realized_pnl=10_000)
        snap1 = agg.snapshot()
        self.assertFalse(snap1.kill_switch_active)
        self.assertAlmostEqual(snap1.peak_equity, 110_000)

        agg.update("a", realized_pnl=-15_000)  # equity 100k - 15k = 85k
        snap2 = agg.snapshot()
        self.assertTrue(snap2.kill_switch_active)
        self.assertGreater(len(breaches), 0)
        self.assertEqual(breaches[0].rule, "max_drawdown_pct")

    def test_drawdown_below_limit_keeps_switch_off(self) -> None:
        agg = pr.PortfolioRiskAggregator(
            rules=pr.RiskRules(max_drawdown_pct=0.20),
            initial_equity=100_000,
        )
        agg.update("a", realized_pnl=-5_000)  # 5% drawdown
        self.assertFalse(agg.snapshot().kill_switch_active)


class DailyLossTests(unittest.TestCase):
    def test_daily_loss_breach(self) -> None:
        agg = pr.PortfolioRiskAggregator(
            rules=pr.RiskRules(max_daily_loss=1_000),
        )
        agg.update("a", realized_pnl=-1_500)
        snap = agg.snapshot()
        self.assertTrue(snap.kill_switch_active)
        rules = [b.rule for b in snap.breaches]
        self.assertIn("max_daily_loss", rules)


class GrossExposureTests(unittest.TestCase):
    def test_gross_exposure_breach(self) -> None:
        agg = pr.PortfolioRiskAggregator(
            rules=pr.RiskRules(max_gross_exposure=10_000),
        )
        agg.update("a", gross_exposure=6_000)
        agg.update("b", gross_exposure=7_000)
        self.assertTrue(agg.snapshot().kill_switch_active)


class ConcentrationTests(unittest.TestCase):
    def test_concentration_breach(self) -> None:
        agg = pr.PortfolioRiskAggregator(
            rules=pr.RiskRules(max_concentration_pct=0.50),
        )
        agg.update("a", gross_exposure=8_000)
        agg.update("b", gross_exposure=2_000)  # a is 80% of gross
        snap = agg.snapshot()
        self.assertTrue(snap.kill_switch_active)
        rules = [b.rule for b in snap.breaches]
        self.assertIn("max_concentration_pct", rules)

    def test_balanced_concentration_clean(self) -> None:
        # Concentration only fires when 2+ strategies carry gross
        # exposure; one strategy alone is by definition 100% but
        # that's not concentration risk, that's "only one strategy".
        agg = pr.PortfolioRiskAggregator(
            rules=pr.RiskRules(max_concentration_pct=0.60),
        )
        agg.update("a", gross_exposure=4_000)
        agg.update("b", gross_exposure=6_000)
        self.assertFalse(agg.snapshot().kill_switch_active)

    def test_single_strategy_does_not_trip_concentration(self) -> None:
        agg = pr.PortfolioRiskAggregator(
            rules=pr.RiskRules(max_concentration_pct=0.50),
        )
        agg.update("a", gross_exposure=10_000)
        self.assertFalse(agg.snapshot().kill_switch_active)


class CheckOrderTests(unittest.TestCase):
    def test_within_gross_cap_returns_none(self) -> None:
        agg = pr.PortfolioRiskAggregator(
            rules=pr.RiskRules(max_gross_exposure=10_000),
        )
        agg.update("a", gross_exposure=4_000)
        result = agg.check_order(strategy="b", notional=2_000, side="buy")
        self.assertIsNone(result)

    def test_exceeds_gross_cap_returns_breach(self) -> None:
        agg = pr.PortfolioRiskAggregator(
            rules=pr.RiskRules(max_gross_exposure=10_000),
        )
        agg.update("a", gross_exposure=8_000)
        result = agg.check_order(strategy="a", notional=5_000, side="buy")
        self.assertIsNotNone(result)
        self.assertEqual(result.rule, "max_gross_exposure")

    def test_kill_switch_active_blocks_orders(self) -> None:
        agg = pr.PortfolioRiskAggregator(
            rules=pr.RiskRules(max_daily_loss=100),
        )
        agg.update("a", realized_pnl=-200)  # trips
        result = agg.check_order(strategy="b", notional=1, side="buy")
        self.assertIsNotNone(result)
        self.assertEqual(result.rule, "kill_switch_active")


class ResetTests(unittest.TestCase):
    def test_reset_clears_switch_until_next_breach(self) -> None:
        agg = pr.PortfolioRiskAggregator(
            rules=pr.RiskRules(max_daily_loss=100),
        )
        agg.update("a", realized_pnl=-200)
        self.assertTrue(agg.snapshot().kill_switch_active)
        agg.reset_kill_switch()
        # PnL hasn't recovered, so the next update re-trips.
        agg.update("a", trade_count=1)
        self.assertTrue(agg.snapshot().kill_switch_active)


class ConcurrencyTests(unittest.TestCase):
    def test_concurrent_updates_do_not_corrupt(self) -> None:
        agg = pr.PortfolioRiskAggregator()

        def worker(prefix: str, n: int) -> None:
            for i in range(n):
                agg.update(f"{prefix}-{i}",
                           realized_pnl=1.0, gross_exposure=10.0)

        threads = [threading.Thread(target=worker, args=(f"t{i}", 50)) for i in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        snap = agg.snapshot()
        self.assertEqual(len(snap.accounts), 200)
        self.assertAlmostEqual(snap.total_daily_pnl, 200.0)


if __name__ == "__main__":
    unittest.main()
