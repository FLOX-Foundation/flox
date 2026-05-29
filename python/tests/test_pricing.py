"""
python/tests/test_pricing.py — option pricing bindings (bs_price, bs_vega,
implied_vol, forward_price, OptionType).

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_pricing.py
or against an installed module:
    python3 python/tests/test_pricing.py
"""

import sys
import os
import math

build_dir = os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'python')
if os.path.isdir(build_dir):
    sys.path.insert(0, os.path.abspath(build_dir))

import flox_py as flox

_passed = 0
_failed = 0


def check(cond, msg):
    global _passed, _failed
    if cond:
        _passed += 1
    else:
        _failed += 1
        print(f"  FAIL: {msg}")


# Reference: S=K=100, T=1, r=b=0.05, vol=0.20 → call 10.4506, put 5.5735.
call = flox.bs_price(flox.OptionType.CALL, 100, 100, 1.0, 0.20, rate=0.05, carry=0.05)
put = flox.bs_price(flox.OptionType.PUT, 100, 100, 1.0, 0.20, rate=0.05, carry=0.05)
check(abs(call - 10.4506) < 1e-3, f"reference call {call}")
check(abs(put - 5.5735) < 1e-3, f"reference put {put}")

# Put-call parity: C - P = S e^((b-r)T) - K e^(-rT)
parity = 100 * math.exp((0.05 - 0.05) * 1.0) - 100 * math.exp(-0.05 * 1.0)
check(abs((call - put) - parity) < 1e-9, "put-call parity")

# Crypto default rate=carry=0, ATM → call == put (symmetric).
cc = flox.bs_price(flox.OptionType.CALL, 70000, 70000, 30 / 365, 0.6)
cp = flox.bs_price(flox.OptionType.PUT, 70000, 70000, 30 / 365, 0.6)
check(cc > 0, "crypto atm call positive")
check(abs(cc - cp) < 1e-3, "crypto atm symmetric (rate=carry=0)")

# Expiry → intrinsic.
check(abs(flox.bs_price(flox.OptionType.CALL, 120, 100, 0.0, 0.2) - 20.0) < 1e-9,
      "expiry intrinsic call")

# Vega positive + matches finite difference.
h = 1e-5
fd = (flox.bs_price(flox.OptionType.CALL, 100, 100, 1.0, 0.20 + h, rate=0.05, carry=0.05) -
      flox.bs_price(flox.OptionType.CALL, 100, 100, 1.0, 0.20 - h, rate=0.05, carry=0.05)) / (2 * h)
vega = flox.bs_vega(100, 100, 1.0, 0.20, rate=0.05, carry=0.05)
check(vega > 0, "vega positive")
check(abs(vega - fd) < 1e-2, f"vega matches FD ({vega} vs {fd})")

# Implied vol round-trip.
for true_vol in (0.1, 0.3, 0.8):
    price = flox.bs_price(flox.OptionType.CALL, 100, 100, 1.0, true_vol, rate=0.05, carry=0.05)
    iv = flox.implied_vol(flox.OptionType.CALL, price, 100, 100, 1.0, rate=0.05, carry=0.05)
    check(iv["converged"], f"iv converged vol={true_vol}")
    check(abs(iv["vol"] - true_vol) < 1e-4, f"iv round-trip vol={true_vol} got {iv['vol']}")

# Implied vol rejects arbitrage price (above asset bound).
too_high = flox.bs_price(flox.OptionType.CALL, 100, 100, 1.0, 10.0, rate=0.05, carry=0.05) + 1.0
iv_bad = flox.implied_vol(flox.OptionType.CALL, too_high, 100, 100, 1.0, rate=0.05, carry=0.05)
check(not iv_bad["converged"], "iv rejects arbitrage price")
check(math.isnan(iv_bad["vol"]), "iv NaN on arbitrage price")

# Forward price.
check(abs(flox.forward_price(100, 1.0, 0.05) - 100 * math.exp(0.05)) < 1e-9, "forward price")

# OptionType enum exposed.
check(hasattr(flox, "OptionType"), "OptionType enum present")
check(flox.OptionType.CALL != flox.OptionType.PUT, "OptionType values distinct")

print(f"\n{_passed} passed, {_failed} failed")
if _failed:
    sys.exit(1)
