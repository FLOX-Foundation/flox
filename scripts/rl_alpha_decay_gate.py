#!/usr/bin/env python3
"""RL env → paper alpha-decay measurement gate.

The Phase 2 North Star for the RL env is "one policy, three deployment
modes." This script enforces a CI-checkable bound on the drift between
modes: a canonical deterministic policy is run through
``FloxTradingEnv`` and through ``PaperBroker`` against an identical
synthetic tape; the script asserts that the per-step reward
distributions stay close (paper equity-at-end within a configured
percentage of env equity-at-end).

All inputs are synthetic and deterministic from a single seed. No real
market data and no recorded exchange sessions ship in the repo. The
``--tape`` flag exists for local sanity checks against private data;
when set the script refuses to write CI artifacts so private data does
not leak into public logs.

Exit codes:
- 0 — decay is below the configured cap
- 1 — decay exceeds the cap (drift between modes)
- 2 — gate setup error (missing env, fixture generation failure, etc.)
"""
from __future__ import annotations

import argparse
import math
import random
import sys
from pathlib import Path
from typing import List, Sequence, Tuple

REPO_ROOT = Path(__file__).resolve().parents[1]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break


def _generate_synthetic_tape(
    seed: int, n_events: int = 4000, anchor_price: float = 50_000.0
) -> List[Tuple[int, float, float, int]]:
    """Deterministic Ornstein-Uhlenbeck price walk with occasional
    jumps. Inter-event timing follows an exponential distribution at
    around 10ms cadence. The tape has the right shape — float prices,
    positive quantities, alternating sides — without resembling any
    real venue's distribution. The gate only needs the same code path
    to exercise on every run."""
    rng = random.Random(seed)
    trades: List[Tuple[int, float, float, int]] = []
    price = anchor_price
    theta = 0.02  # mean reversion strength
    sigma = 1.5  # tick-scale volatility
    ts_ns = 0
    for i in range(n_events):
        # OU step + occasional jump
        drift = theta * (anchor_price - price)
        shock = rng.gauss(0.0, sigma)
        price += drift + shock
        if rng.random() < 0.001:
            price += rng.gauss(0.0, sigma * 10.0)
        price = max(price, 1.0)

        qty = rng.lognormvariate(-3.0, 1.0)
        side = 0 if rng.random() < 0.5 else 1
        ts_ns += int(rng.expovariate(1.0 / 10_000_000.0))
        trades.append((ts_ns, float(price), float(qty), side))
    return trades


def _stub_policy(observation: Sequence[float]) -> List[float]:
    """Deterministic policy that buys on a downtrend and sells on an
    uptrend of the last few observed prices. Always emits a market
    order so fees and queue position do not enter the decay signal —
    the gate measures whether the broker pipeline preserves the same
    physics as the env on the simplest possible action stream."""
    # Use the first ~8 normalised price entries as the signal.
    n_signal = min(8, len(observation) - 2)
    if n_signal <= 1:
        return [0.0, 0.0, 0.0]
    window = list(observation[:n_signal])
    delta = window[-1] - window[0]
    if delta > 0.0005:
        return [-1.0, 0.0, 0.0]  # short
    if delta < -0.0005:
        return [1.0, 0.0, 0.0]  # long
    return [0.0, 0.0, 0.0]


def _equity_trajectory_env(
    trades: Sequence[Tuple[int, float, float, int]],
    *,
    qty: float,
    window_size: int,
) -> List[float]:
    import flox_py
    from flox_py.rl_env import FloxTradingEnv

    stack = flox_py.VenueStack.binance_um_futures(
        account_id=1, equity=10_000.0
    )
    env = FloxTradingEnv.from_venue_stack(
        stack, tape=trades, qty=qty, max_position=qty,
        window_size=window_size, symbol_id=1, n_open_slots=0,
        tick_size=0.5,
    )
    obs, _ = env.reset(seed=0)
    equities: List[float] = []
    done = False
    while not done:
        action = _stub_policy(obs)
        obs, _r, term, trunc, info = env.step(action)
        eq = info.get("equity_at_mark", info.get("equity"))
        if eq is not None:
            equities.append(float(eq))
        done = term or trunc
    return equities


def _equity_trajectory_paper(
    trades: Sequence[Tuple[int, float, float, int]],
    *,
    qty: float,
    window_size: int,
) -> List[float]:
    """Run the same stub policy through PaperBroker by hand. We do
    not invoke the real Runner because the stub policy is not a
    flox.Strategy subclass; instead we simulate what the adapter
    does: on each tick, update the obs builder, run the policy,
    emit an order through the paper executor. The resulting equity
    trajectory is what the broker would have produced."""
    import flox_py
    from flox_py.rl_env import ObservationBuilder

    stack = flox_py.VenueStack.binance_um_futures(
        account_id=2, equity=10_000.0
    )
    exec_ = stack.executor()
    acct = stack.account()
    fees = stack.fees()

    builder = ObservationBuilder(
        window_size=window_size, n_open_slots=0,
        tick_size=0.5, max_price_offset_ticks=50, max_position=qty,
    )
    builder.reset(first_price=float(trades[0][1]))

    position = 0.0
    entry_price = 0.0
    next_order_id = 0
    last_fill_idx = 0
    equities: List[float] = []
    venue_entry = 0.0

    def realize_and_set(new_pos: float, fill_price: float) -> None:
        nonlocal position, venue_entry
        old_pos = position
        if abs(old_pos) < 1e-12:
            if abs(new_pos) > 1e-12:
                acct.open_position(1, new_pos, fill_price)
                venue_entry = fill_price
        elif old_pos * new_pos < 0:
            direction = 1.0 if old_pos > 0 else -1.0
            realized = (fill_price - venue_entry) * direction * abs(old_pos)
            acct.add_equity(realized)
            acct.close_position(1)
            acct.open_position(1, new_pos, fill_price)
            venue_entry = fill_price
        elif abs(new_pos) < 1e-12:
            direction = 1.0 if old_pos > 0 else -1.0
            realized = (fill_price - venue_entry) * direction * abs(old_pos)
            acct.add_equity(realized)
            acct.close_position(1)
            venue_entry = 0.0
        else:
            acct.close_position(1)
            acct.open_position(1, new_pos, fill_price)
            venue_entry = fill_price
        position = new_pos

    for ts_ns, price, trade_qty, trade_side in trades:
        builder.on_trade(float(price))
        builder.set_position(position, venue_entry)
        obs = builder.build()
        action = _stub_policy(obs)
        signed_frac = max(-1.0, min(1.0, float(action[0])))
        target = signed_frac * qty
        delta = target - position
        if abs(delta) > 1e-12:
            side = "buy" if delta > 0 else "sell"
            next_order_id += 1
            exec_.submit_order(
                id=next_order_id,
                side=side,
                price=float(price),
                quantity=abs(delta),
                type="market",
                symbol=1,
                tif="gtc",
                account_id=acct.account_id(),
            )
        exec_.on_trade_qty(1, float(price), float(trade_qty), trade_side == 0)

        fills = exec_.fills_list()
        for f in fills[last_fill_idx:]:
            fill_qty = float(f["quantity"])
            fill_price = float(f["price"])
            signed = fill_qty if f["side"] == "buy" else -fill_qty
            realize_and_set(position + signed, fill_price)
            notional = fill_price * fill_qty
            fees.record_fill(int(ts_ns), notional)
            fee = float(fees.fee_for(int(ts_ns), notional, False))
            acct.add_equity(-fee)
        last_fill_idx = len(fills)

        acct.set_mark(1, float(price), int(ts_ns))
        equities.append(float(acct.equity() + acct.total_unrealised_pnl()))

    return equities


def _summary(eq: Sequence[float]) -> dict:
    if not eq:
        return {"start": 0.0, "end": 0.0, "return_pct": 0.0, "sharpe": 0.0}
    start = eq[0]
    end = eq[-1]
    return_pct = (end - start) / start * 100.0 if start != 0 else 0.0
    diffs = [eq[i + 1] - eq[i] for i in range(len(eq) - 1)]
    if diffs:
        mean = sum(diffs) / len(diffs)
        var = sum((d - mean) ** 2 for d in diffs) / max(len(diffs) - 1, 1)
        std = math.sqrt(var)
        sharpe = mean / std if std > 0 else 0.0
    else:
        sharpe = 0.0
    return {
        "start": start, "end": end, "return_pct": return_pct, "sharpe": sharpe
    }


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--max-decay", type=float, default=0.30,
        help="Maximum |Δequity| / |env_change| allowed between env and paper.",
    )
    parser.add_argument(
        "--seed", type=int, default=42,
        help="Seed for the synthetic tape generator.",
    )
    parser.add_argument(
        "--n-events", type=int, default=4000,
        help="Number of synthetic trades in the tape.",
    )
    parser.add_argument(
        "--qty", type=float, default=0.001,
        help="Order quantity. Small enough to keep equity drift moderate.",
    )
    parser.add_argument(
        "--window-size", type=int, default=8,
        help="Observation window size. Matches default env window.",
    )
    parser.add_argument(
        "--tape", type=str, default="",
        help="Optional local-only path to a real .floxlog tape. When "
             "set the gate refuses to write CI artifacts.",
    )
    args = parser.parse_args(argv)

    if args.tape:
        # Real data path is opt-in and stays out of CI.
        try:
            from flox_py.rl_env import _load_tape_trades  # type: ignore
        except ImportError:
            print("error: flox_py not importable", file=sys.stderr)
            return 2
        trades = _load_tape_trades(args.tape)
        print(f"# loaded {len(trades)} real trades from {args.tape}")
        print("# CI artifacts refused — running in local-only mode")
    else:
        trades = _generate_synthetic_tape(args.seed, args.n_events)
        print(f"# generated {len(trades)} synthetic trades (seed={args.seed})")

    print("running env path...")
    env_eq = _equity_trajectory_env(
        trades, qty=args.qty, window_size=args.window_size
    )
    env_s = _summary(env_eq)

    print("running paper path...")
    paper_eq = _equity_trajectory_paper(
        trades, qty=args.qty, window_size=args.window_size
    )
    paper_s = _summary(paper_eq)

    env_change = env_s["end"] - env_s["start"]
    paper_change = paper_s["end"] - paper_s["start"]
    denom = abs(env_change) if abs(env_change) > 1.0 else 1.0
    decay = abs(env_change - paper_change) / denom

    print()
    print(f"env   start={env_s['start']:.4f} end={env_s['end']:.4f} "
          f"return={env_s['return_pct']:+.4f}% sharpe={env_s['sharpe']:+.4f}")
    print(f"paper start={paper_s['start']:.4f} end={paper_s['end']:.4f} "
          f"return={paper_s['return_pct']:+.4f}% sharpe={paper_s['sharpe']:+.4f}")
    print(f"Δequity_env={env_change:+.4f} Δequity_paper={paper_change:+.4f}")
    print(f"decay={decay * 100:.2f}% (cap={args.max_decay * 100:.2f}%)")

    if decay > args.max_decay:
        print(f"FAIL: decay {decay * 100:.2f}% exceeds cap {args.max_decay * 100:.2f}%")
        return 1
    print(f"PASS: decay within cap")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
