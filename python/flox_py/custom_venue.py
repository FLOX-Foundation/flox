"""Custom venue assembly helper.

`VenueStack` ships four canonical factories (binance_um_futures,
bybit_linear, okx_swap, deribit). For venues outside that set, or
configurations that override one subsystem (e.g. a custom fee
ladder), use `assemble_custom_venue` to wire pre-built subsystems
together in one call.

The C++ `VenueStack::assemble(AssembleArgs&&)` is the underlying
escape hatch but takes `std::unique_ptr<>` parameters that don't
translate cleanly through pybind11. This pure-Python helper
mirrors its behaviour by orchestrating the existing setter API
across user-owned subsystem objects.
"""
from __future__ import annotations

from typing import TYPE_CHECKING, Optional

if TYPE_CHECKING:
    # These imports run only under static type checking. At runtime the
    # source tree (python/flox_py/) has no compiled `_flox_py` extension
    # alongside `__init__.py` — only `build/python/flox_py/` has the
    # `.so`. Eager imports at module-load time would crash CI jobs that
    # walk the source layout (e.g. `python/tests/test_flox_new_cli.py`
    # invoked from repo root). Defer to call-time imports inside
    # `assemble_custom_venue`.
    from ._flox_py import (
        Account,
        FeeSchedule,
        FundingSchedule,
        LiquidationEngine,
        RateLimitPolicy,
        SimulatedExecutor,
        VenueAvailability,
    )


class CustomVenue:
    """Lightweight Python container mirroring `VenueStack`'s accessor
    surface for user-assembled venues.

    Holds a reference to every owned subsystem so the C++ pointers
    handed across the wiring stay live for the venue's lifetime.
    """

    __slots__ = (
        "_executor",
        "_account",
        "_liquidation",
        "_fees",
        "_funding",
        "_venue_availability",
        "_rate_limits",
        "_venue_name",
    )

    def __init__(
        self,
        *,
        executor: SimulatedExecutor,
        account: Account,
        liquidation: LiquidationEngine,
        fees: FeeSchedule,
        funding: FundingSchedule,
        venue_availability: VenueAvailability,
        rate_limits: RateLimitPolicy,
        venue_name: str,
    ) -> None:
        self._executor = executor
        self._account = account
        self._liquidation = liquidation
        self._fees = fees
        self._funding = funding
        self._venue_availability = venue_availability
        self._rate_limits = rate_limits
        self._venue_name = venue_name

    def executor(self) -> SimulatedExecutor:
        return self._executor

    def account(self) -> Account:
        return self._account

    def liquidation(self) -> LiquidationEngine:
        return self._liquidation

    def fees(self) -> FeeSchedule:
        return self._fees

    def funding(self) -> FundingSchedule:
        return self._funding

    def venue_availability(self) -> VenueAvailability:
        return self._venue_availability

    def venue_name(self) -> str:
        return self._venue_name


def assemble_custom_venue(
    *,
    account: Account,
    fees: FeeSchedule,
    funding: FundingSchedule,
    liquidation: LiquidationEngine,
    rate_limits: RateLimitPolicy,
    venue_availability: Optional[VenueAvailability] = None,
    venue_name: str = "custom",
) -> CustomVenue:
    """Wire pre-built subsystems into a venue-stack-shaped bundle.

    Caller builds each subsystem with whatever configuration they
    need (custom fee ladder, custom funding interval, custom MM
    tier, ...) and passes them in. The helper:

    - Creates a fresh `SimulatedClock` + `SimulatedExecutor`.
    - Binds the executor to the supplied rate-limit policy +
      venue-availability instance (or a fresh `VenueAvailability`
      if none supplied).
    - Attaches the account to the liquidation engine and routes
      liquidation orders through the executor.
    - Binds the fee schedule to the account so 30d notional
      aggregates across symbols.

    The returned `CustomVenue` holds references to every subsystem
    so the wiring pointers stay live.
    """
    # Runtime imports (see TYPE_CHECKING guard at top for why these
    # are deferred from module load).
    from ._flox_py import SimulatedExecutor, VenueAvailability

    executor = SimulatedExecutor()
    venue_avail = venue_availability if venue_availability is not None else VenueAvailability()

    # Wire the executor.
    executor.set_venue_availability(venue_avail)
    executor.set_rate_limit_policy(rate_limits)

    # Wire account → fees + liquidation.
    fees.bind_account(account)
    liquidation.attach_account(account)
    liquidation.set_executor(executor)

    return CustomVenue(
        executor=executor,
        account=account,
        liquidation=liquidation,
        fees=fees,
        funding=funding,
        venue_availability=venue_avail,
        rate_limits=rate_limits,
        venue_name=venue_name,
    )
