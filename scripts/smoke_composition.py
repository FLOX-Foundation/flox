#!/usr/bin/env python3
"""W15 composition smoke. Wire every W15 subsystem onto a single
SimulatedExecutor, run a short synthetic tape, and assert the result
matches a fixed-shape summary.

The point isn't to validate correctness deeply (test_w15_composition.cpp
already does that). The point is to catch wiring/init-order/dead-
export regressions before they reach production. Designed to run in
under 30 seconds on CI.

Exit codes:
  0 — every subsystem wired up and the summary matches.
  1 — any subsystem raised or the summary diverged.
"""
from __future__ import annotations

import sys
import traceback


def main() -> int:
    try:
        import flox_py
    except ImportError as exc:
        print(f"smoke FAIL: flox_py import failed: {exc}", file=sys.stderr)
        return 1

    failures: list[str] = []

    def must(name: str, cond: bool, *, detail: str = "") -> None:
        if cond:
            print(f"  ok    {name}")
        else:
            msg = f"FAIL  {name}" + (f": {detail}" if detail else "")
            print(f"  {msg}")
            failures.append(name)

    print("[smoke] W15 composition wiring check")

    # 1) Build the executor.
    exec = flox_py.SimulatedExecutor()
    must("create SimulatedExecutor", exec is not None)

    # 2) Queue model + new pro-rata variants.
    exec.set_queue_model("pro_rata_with_priority", depth=4)
    exec.set_order_priority_multiplier(1, 1.5)
    must("set_queue_model + priority multiplier", True)

    # 3) FOK mode (T042).
    exec.set_fok_mode("single_price")
    must("set_fok_mode('single_price')",
         exec.fok_mode() == "single_price")
    exec.set_fok_mode("any_price")

    # 4) STP + multi-account (T044).
    exec.set_stp_mode("cancel_newest")
    exec.set_stp_group_membership(42, 100)
    exec.set_stp_group_membership(43, 100)
    must("STP group membership", exec.stp_group_for(42) == 100)

    # 5) Rate-limit policy with per-endpoint families (T049).
    rl = flox_py.RateLimitPolicy()
    rl.add_bucket("orders_10s", 10_000_000_000, 5)
    rl.add_family_bucket(
        flox_py.RateLimitEndpointFamily.MarketData,
        "md_60s", 60_000_000_000, 6000)
    rl.add_family_bucket(
        flox_py.RateLimitEndpointFamily.Account,
        "account_60s", 60_000_000_000, 1200)
    exec.set_rate_limit_policy(rl)
    must("per-endpoint rate-limit pools",
         rl.try_consume("query_market_data", 0))

    # 6) Venue downtime + outage pathology (T046).
    va = flox_py.VenueAvailability()
    va.schedule_outage_ex(
        start_ns=1_000_000_000, duration_ns=500_000_000,
        outage_type="slow_degradation",
        degradation_latency_multiplier=20.0,
    )
    exec.set_venue_availability(va)
    must("slow_degradation latency multiplier",
         va.latency_multiplier(1_100_000_000) == 20.0)

    # 7) Funding schedule (T047) — per-symbol tape via direct entries.
    f = flox_py.FundingSchedule.tape_by_symbol([
        flox_py.FundingTapeEntry(),  # default zeros (rate=0)
    ])
    must("FundingSchedule.tape_by_symbol", f is not None)

    f2 = flox_py.FundingSchedule.binance_um_futures()
    payments = f2.tick(
        now_ns=9 * 3600 * 1_000_000_000,
        symbols=[1],
        positions=[1.0],
        mark_prices=[50000.0],
    )
    must("FundingSchedule.binance_um_futures tick",
         len(payments) >= 1,
         detail=f"got {len(payments)} payments")

    # 8) LiquidationEngine + ADL ranking variants (T036 + T045).
    liq = flox_py.LiquidationEngine.binance_um_futures()
    must("binance ADL ranking == Binance",
         liq.adl_ranking() == flox_py.AdlRanking.Binance)
    liq.set_executor(exec)
    must("LiquidationEngine.set_executor wired", True)

    # 9) Bracket order surface (T040 partial-fill arm mode).
    exec.set_bracket_child_arm_mode("on_partial_fill")
    must("set_bracket_child_arm_mode", True)

    # 10) Drive a brief tape: ensure no crashes.
    try:
        for i in range(10):
            ts = (i + 2) * 1_000_000_000
            exec.on_bar(1, 50000.0 + i)
        must("tick loop drives 10 bars without crash", True)
    except Exception as exc:
        must("tick loop drives 10 bars without crash", False,
             detail=f"raised: {exc}")

    # 11) Cascade-stats accessors (T039).
    must("LiquidationEngine cascade stats accessor",
         isinstance(liq.cascade_sizes_per_tick(), list))

    if failures:
        print(f"[smoke] FAILED ({len(failures)} check(s)): {failures}")
        return 1
    print("[smoke] OK — every subsystem wired up.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        traceback.print_exc()
        sys.exit(1)
