"""
python/tests/test_greeks.py — option greeks bindings (greeks,
second_order_greeks).

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_greeks.py
or against an installed module:
    python3 python/tests/test_greeks.py
"""

import sys
import os

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


S, K, T, R, B, V = 100.0, 100.0, 1.0, 0.05, 0.05, 0.20


def price(t, s, k, tt, vol, rate, carry):
    return flox.bs_price(t, s, k, tt, vol, rate=rate, carry=carry)


g = flox.greeks(flox.OptionType.CALL, S, K, T, V, rate=R, carry=B)
gp = flox.greeks(flox.OptionType.PUT, S, K, T, V, rate=R, carry=B)

# Delta: finite difference vs analytic.
h = 1e-4
fd_delta = (price(flox.OptionType.CALL, S + h, K, T, V, R, B) -
            price(flox.OptionType.CALL, S - h, K, T, V, R, B)) / (2 * h)
check(abs(g["delta"] - fd_delta) < 1e-5, f"call delta vs FD ({g['delta']} vs {fd_delta})")
check(g["delta"] > 0 and gp["delta"] < 0, "call delta>0, put delta<0")

# Gamma: second difference; identical call/put.
fd_gamma = (price(flox.OptionType.CALL, S + 1, K, T, V, R, B) -
            2 * price(flox.OptionType.CALL, S, K, T, V, R, B) +
            price(flox.OptionType.CALL, S - 1, K, T, V, R, B)) / 1.0
check(abs(g["gamma"] - fd_gamma) < 1e-4, "gamma vs FD")
check(abs(g["gamma"] - gp["gamma"]) < 1e-12, "gamma call==put")

# Vega: matches the standalone bs_vega and FD.
check(abs(g["vega"] - flox.bs_vega(S, K, T, V, rate=R, carry=B)) < 1e-9, "vega == bs_vega")

# Theta: long call decays (negative, per year).
check(g["theta"] < 0, "long call theta negative")
fd_theta = -(price(flox.OptionType.CALL, S, K, T + 1e-5, V, R, B) -
             price(flox.OptionType.CALL, S, K, T - 1e-5, V, R, B)) / (2e-5)
check(abs(g["theta"] - fd_theta) < 1e-2, "theta vs FD")

# Rho: b-fixed dV/dr, FD bumps rate holding carry.
fd_rho = (price(flox.OptionType.CALL, S, K, T, V, R + 1e-6, B) -
          price(flox.OptionType.CALL, S, K, T, V, R - 1e-6, B)) / (2e-6)
check(abs(g["rho"] - fd_rho) < 1e-2, "rho vs FD (b-fixed)")

# Second-order greeks: vanna = d(delta)/d(vol), volga = d(vega)/d(vol).
s2 = flox.second_order_greeks(flox.OptionType.CALL, S, K, T, V, rate=R, carry=B)
fd_vanna = (flox.greeks(flox.OptionType.CALL, S, K, T, V + 1e-5, rate=R, carry=B)["delta"] -
            flox.greeks(flox.OptionType.CALL, S, K, T, V - 1e-5, rate=R, carry=B)["delta"]) / (2e-5)
check(abs(s2["vanna"] - fd_vanna) < 1e-3, "vanna vs FD")
fd_volga = (flox.greeks(flox.OptionType.CALL, S, K, T, V + 1e-5, rate=R, carry=B)["vega"] -
            flox.greeks(flox.OptionType.CALL, S, K, T, V - 1e-5, rate=R, carry=B)["vega"]) / (2e-5)
check(abs(s2["volga"] - fd_volga) < 1e-2, "volga vs FD")
check("charm" in s2, "charm present")

# Crypto default (rate=carry=0) returns finite greeks.
gc = flox.greeks(flox.OptionType.CALL, 70000, 70000, 30 / 365, 0.6)
check(gc["delta"] > 0 and gc["vega"] > 0, "crypto greeks finite")

print(f"\n{_passed} passed, {_failed} failed")
if _failed:
    sys.exit(1)
