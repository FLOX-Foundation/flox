"""
python/tests/test_indicator_period_validation.py

period=0 on the rolling-sum indicator family (SMA/RMA/Bollinger/VWAP/CCI) used
to underflow `period - 1` to a huge index inside compute() -- a heap-buffer
overflow confirmed under AddressSanitizer, reachable straight from Python with
an ordinary call like `flox.sma(arr, 0)`. The C++ engine now treats period=0
like "not enough data" (an all-NaN result, never a crash), but a zero period
is almost always a caller mistake (a config value or an optimizer search bound
that slipped to 0), so the Python layer raises E_IND_001 loudly instead of
returning a silently useless array.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_indicator_period_validation.py
"""

import os
import sys

build_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "python")
sys.path.insert(0, os.path.abspath(build_dir))

import numpy as np  # noqa: E402
import flox_py as flox  # noqa: E402


def main():
    arr = np.array([1.0, 2.0, 3.0, 4.0, 5.0])

    top_level_calls = [
        ("sma", lambda: flox.sma(arr, 0)),
        ("rma", lambda: flox.rma(arr, 0)),
        ("bollinger", lambda: flox.bollinger(arr, 0, 2.0)),
        ("vwap", lambda: flox.vwap(arr, arr, 0)),
        ("cci", lambda: flox.cci(arr, arr, arr, 0)),
    ]
    for name, call in top_level_calls:
        try:
            call()
            raise AssertionError(f"{name}(..., 0) did not raise")
        except flox.FloxError as e:
            assert e.code == "E_IND_001", f"{name}: expected E_IND_001, got {e.code}"

    class_constructions = [
        ("SMA", lambda: flox.SMA(0)),
        ("RMA", lambda: flox.RMA(0)),
        ("Bollinger", lambda: flox.Bollinger(0)),
        ("CCI", lambda: flox.CCI(0)),
    ]
    for name, call in class_constructions:
        try:
            call()
            raise AssertionError(f"{name}(0) did not raise")
        except flox.FloxError as e:
            assert e.code == "E_IND_001", f"{name}: expected E_IND_001, got {e.code}"

    # A positive period is unaffected: the validation only rejects 0.
    assert not np.isnan(flox.sma(arr, 3)[-1])
    assert flox.SMA(3) is not None

    print("test_indicator_period_validation: OK (period=0 raises E_IND_001, "
          "period>0 is unaffected)")


def test_main() -> None:
    """Entry point pytest can collect (see test_amm_curve.py for why)."""
    main()


if __name__ == "__main__":
    main()
