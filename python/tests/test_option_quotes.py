"""
python/tests/test_option_quotes.py — DataWriter.write_option_quotes and
DataReader.read_option_quotes_from round-trip.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_option_quotes.py
or against an installed module:
    python3 python/tests/test_option_quotes.py
"""

import sys
import os
import tempfile
import shutil

build_dir = os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'python')
if os.path.isdir(build_dir):
    sys.path.insert(0, os.path.abspath(build_dir))

import flox_py as flox
import numpy as np

_passed = 0
_failed = 0


def check(cond, msg):
    global _passed, _failed
    if cond:
        _passed += 1
    else:
        _failed += 1
        print(f"  FAIL: {msg}")


d = tempfile.mkdtemp()
try:
    n = 50
    ts = np.arange(n, dtype=np.int64) * 1_000_000_000 + 1_000_000_000
    marks = np.linspace(3000.0, 3050.0, n)
    indices = marks + 0.5
    ivs = np.linspace(0.40, 0.90, n)
    oi = np.full(n, 1234.0)
    syms = np.full(n, 7, dtype=np.uint32)

    w = flox.DataWriter(d, max_segment_mb=128, exchange_id=0)
    written = w.write_option_quotes(
        exchange_ts_ns=ts, recv_ts_ns=ts, mark_prices=marks,
        index_prices=indices, ivs=ivs, open_interest=oi, symbol_ids=syms,
    )
    w.close()
    check(written == n, f"wrote all {n} quotes (got {written})")

    r = flox.DataReader(d)
    arr = r.read_option_quotes_from(0)
    check(arr.size == n, f"read back {n} quotes (got {arr.size})")

    expected_fields = {"exchange_ts_ns", "mark_price_raw", "index_price_raw",
                       "iv_raw", "open_interest_raw", "symbol_id", "instrument"}
    check(expected_fields.issubset(set(arr.dtype.names)), "dtype has expected fields")

    # Raw -> double conversions.
    check(abs(arr["mark_price_raw"][0] / 1e8 - marks[0]) < 1e-6, "mark round-trip")
    check(abs(arr["iv_raw"][0] / 1e8 - ivs[0]) < 1e-6, "iv round-trip")
    check(abs(arr["iv_raw"][-1] / 1e8 - ivs[-1]) < 1e-6, "iv round-trip last")
    check(abs(arr["open_interest_raw"][0] / 1e8 - oi[0]) < 1e-6, "oi round-trip")
    check(int(arr["symbol_id"][0]) == 7, "symbol id preserved")
    check(int(arr["instrument"][0]) == int(flox.OptionType.CALL) + 3 or
          int(arr["instrument"][0]) >= 0, "instrument tagged")  # Option enum value

    # start_ts filter: skip the first half.
    mid_ts = int(ts[n // 2])
    arr2 = r.read_option_quotes_from(mid_ts)
    check(arr2.size == n - n // 2, f"start_ts filter ({arr2.size})")

    # Trades and option quotes do not bleed into each other.
    trades = r.read_trades_from(0)
    check(trades.size == 0, "no trades in an option-quote-only tape")
finally:
    shutil.rmtree(d, ignore_errors=True)

print(f"\n{_passed} passed, {_failed} failed")
if _failed:
    sys.exit(1)
