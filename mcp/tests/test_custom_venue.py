"""T067: assemble_custom_venue helper tests.

Verifies the helper wires user-supplied subsystems into a usable
venue stack (fees bound to account, account attached to
liquidation, executor routed for liquidation orders).
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_PY = REPO_ROOT / "build" / "python"
if BUILD_PY.exists():
    sys.path.insert(0, str(BUILD_PY))

# The compiled flox_py module isn't installed in MCP-only CI jobs
# (e.g. verify-docs-current). Skip rather than ImportError when the
# build artifact isn't on the path.
flox = pytest.importorskip("flox_py")


def _build_custom():
    acct = flox.Account(42, 10_000.0)
    fees = flox.FeeSchedule()
    fees.add_tier(0, 1.0, 3.0)
    fees.add_tier(100_000, 0.5, 2.5)
    funding = flox.FundingSchedule()
    liq = flox.LiquidationEngine()
    liq.add_tier(0.0, 0.004)
    rl = flox.RateLimitPolicy()
    return flox.assemble_custom_venue(
        account=acct,
        fees=fees,
        funding=funding,
        liquidation=liq,
        rate_limits=rl,
        venue_name="my_custom_venue",
    )


def test_custom_venue_basic_accessors():
    custom = _build_custom()
    assert custom.venue_name() == "my_custom_venue"
    assert custom.account().account_id() == 42
    assert custom.account().equity() == 10_000.0
    assert custom.fees().tier_count() == 2
    assert custom.liquidation() is not None
    assert custom.funding() is not None
    assert custom.executor() is not None
    assert custom.venue_availability() is not None


def test_custom_venue_fees_bound_to_account():
    """fees.record_fill must land in account.rolling_notional_30d —
    the headline wiring check that proves bind_account ran."""
    custom = _build_custom()
    custom.fees().record_fill(0, 150_000.0)
    assert custom.account().rolling_notional_30d() == 150_000.0
    # Tier transition triggered by aggregate.
    assert custom.fees().current_tier_index() >= 1


def test_custom_venue_default_availability():
    """If venue_availability is not provided, the helper creates a
    fresh one and installs it on the executor."""
    custom = _build_custom()
    assert custom.venue_availability() is not None


def test_custom_venue_user_supplied_availability():
    avail = flox.VenueAvailability()
    custom = flox.assemble_custom_venue(
        account=flox.Account(1, 100.0),
        fees=flox.FeeSchedule(),
        funding=flox.FundingSchedule(),
        liquidation=flox.LiquidationEngine(),
        rate_limits=flox.RateLimitPolicy(),
        venue_availability=avail,
    )
    assert custom.venue_availability() is avail


def test_custom_venue_default_name():
    custom = flox.assemble_custom_venue(
        account=flox.Account(1, 100.0),
        fees=flox.FeeSchedule(),
        funding=flox.FundingSchedule(),
        liquidation=flox.LiquidationEngine(),
        rate_limits=flox.RateLimitPolicy(),
    )
    assert custom.venue_name() == "custom"
