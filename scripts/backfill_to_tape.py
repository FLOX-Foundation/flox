#!/usr/bin/env python3
"""Historical-data backfill into a `.floxlog` tape.

The canonical `flox tape record` CLI captures live data. For
historical backfill (anything before the recorder was running)
ccxt's `fetch_ohlcv` / `fetch_trades` paginates depending on the
exchange. This script wraps that loop and feeds the rows through
`flox_py.Runner` + `MarketDataRecorderHook` so the result is a
plain `.floxlog` directory — same format as a live recording.

Usage:
  python3 scripts/backfill_to_tape.py \\
      --exchange bitget --symbol BTC/USDT --type klines \\
      --from 2026-04-01 --to 2026-04-08 \\
      --out ./tapes/bitget-btc-apr-week-1

Status code:
  0 — success
  1 — argument error
  2 — exchange / network error
  3 — quota cap exceeded (set --max-records to allow more)

This script is the path the `record_data` MCP tool shells out to.
Keeping it here (not bundled inside the MCP server) means CCXT is
only a dependency when the user actually calls it.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from datetime import datetime, timezone


def _parse_dt(s: str) -> int:
    """Accept either an ISO-ish date / datetime or a unix-ms integer."""
    s = s.strip()
    if s.isdigit():
        return int(s)
    fmts = ("%Y-%m-%dT%H:%M:%S", "%Y-%m-%d %H:%M:%S", "%Y-%m-%d")
    for fmt in fmts:
        try:
            dt = datetime.strptime(s, fmt).replace(tzinfo=timezone.utc)
            return int(dt.timestamp() * 1000)
        except ValueError:
            continue
    raise ValueError(f"cannot parse timestamp {s!r}")


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--exchange", required=True,
                   help="ccxt exchange id (bitget, binance, bybit, ...)")
    p.add_argument("--symbol", required=True, help="symbol, e.g. BTC/USDT")
    p.add_argument("--type", default="klines",
                   choices=("klines", "trades"),
                   help="what to fetch — `klines` (1m bars) or `trades`")
    p.add_argument("--from", dest="from_", required=True,
                   help="start date / time (ISO or unix-ms)")
    p.add_argument("--to", required=True,
                   help="end date / time (ISO or unix-ms)")
    p.add_argument("--out", required=True,
                   help="output `.floxlog` directory")
    p.add_argument("--max-records", type=int, default=1_000_000,
                   help="refuse to start if estimated records exceed cap")
    p.add_argument("--timeframe", default="1m",
                   help="kline timeframe (only used for --type=klines)")
    args = p.parse_args()

    try:
        import ccxt  # type: ignore
    except ImportError:
        print(json.dumps({
            "error": "ccxt is not installed. Install with `pip install ccxt`."
        }))
        return 2

    try:
        import flox_py as flox  # type: ignore
        from flox_py.tape import make_recorder_hook  # type: ignore
    except ImportError as exc:
        print(json.dumps({
            "error": f"flox_py import failed: {exc}. "
                     f"Install with `pip install flox-py`."
        }))
        return 2

    try:
        from_ms = _parse_dt(args.from_)
        to_ms = _parse_dt(args.to)
    except ValueError as exc:
        print(json.dumps({"error": str(exc)}))
        return 1

    if to_ms <= from_ms:
        print(json.dumps({"error": "--to must be greater than --from"}))
        return 1

    # Cheap pre-flight estimate so the user doesn't burn quota by accident.
    duration_minutes = (to_ms - from_ms) / 60_000
    if args.type == "klines":
        estimated = int(duration_minutes / _timeframe_minutes(args.timeframe))
    else:
        # Trades vary wildly; assume 1/sec average. The exchange's actual
        # rate is whatever it is; this is just a sanity cap.
        estimated = int((to_ms - from_ms) / 1000)
    if estimated > args.max_records:
        print(json.dumps({
            "error": (f"estimated {estimated} records exceeds --max-records "
                      f"{args.max_records}. Raise the cap explicitly to proceed.")
        }))
        return 3

    ex_class = getattr(ccxt, args.exchange, None)
    if ex_class is None:
        print(json.dumps({"error": f"unknown ccxt exchange: {args.exchange}"}))
        return 1
    ex = ex_class({"enableRateLimit": True})

    registry = flox.SymbolRegistry()
    sym = registry.add_symbol(args.exchange,
                               args.symbol.replace("/", ""), tick_size=0.01)
    rec = make_recorder_hook(args.out, max_segment_mb=64)
    runner = flox.Runner(registry, on_signal=lambda s: None)
    runner.set_market_data_recorder(rec)

    written = 0
    http_calls = 0
    cursor = from_ms

    try:
        if args.type == "klines":
            while cursor < to_ms:
                bars = ex.fetch_ohlcv(args.symbol, args.timeframe,
                                       since=cursor, limit=1000)
                http_calls += 1
                if not bars:
                    break
                for ts_ms, _o, _h, _l, c, v in bars:
                    if ts_ms >= to_ms:
                        break
                    runner.on_trade(sym, price=float(c), qty=float(v),
                                    is_buy=True, ts_ns=ts_ms * 1_000_000)
                    written += 1
                last_ts = bars[-1][0]
                if last_ts <= cursor:
                    break
                cursor = last_ts + 1
                time.sleep(ex.rateLimit / 1000)
        else:
            while cursor < to_ms:
                trades = ex.fetch_trades(args.symbol, since=cursor, limit=1000)
                http_calls += 1
                if not trades:
                    break
                for t in trades:
                    if t.get("timestamp", 0) >= to_ms:
                        break
                    runner.on_trade(sym, price=float(t["price"]),
                                    qty=float(t["amount"]),
                                    is_buy=(t.get("side") == "buy"),
                                    ts_ns=int(t["timestamp"]) * 1_000_000)
                    written += 1
                last_ts = trades[-1].get("timestamp", cursor)
                if last_ts <= cursor:
                    break
                cursor = last_ts + 1
                time.sleep(ex.rateLimit / 1000)
    except Exception as exc:
        rec.close()
        print(json.dumps({
            "error": f"{type(exc).__name__}: {exc}",
            "written_records": written,
            "http_calls_made": http_calls,
        }))
        return 2

    rec.close()
    stats = rec.stats if hasattr(rec, "stats") else {}
    print(json.dumps({
        "written_records": written,
        "http_calls_made": http_calls,
        "out_path": args.out,
        "exchange": args.exchange,
        "symbol": args.symbol,
        "type": args.type,
        "from_ms": from_ms,
        "to_ms": to_ms,
        "stats": dict(stats) if stats else {},
    }))
    return 0


def _timeframe_minutes(tf: str) -> float:
    units = {"m": 1, "h": 60, "d": 1440}
    if not tf:
        return 1
    suffix = tf[-1]
    n = int(tf[:-1]) if suffix in units else int(tf)
    return n * units.get(suffix, 1)


if __name__ == "__main__":
    sys.exit(main())
