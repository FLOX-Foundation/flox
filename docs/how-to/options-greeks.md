# Compute option greeks

`greeks` returns the first-order sensitivities of an option price, and
`second_order_greeks` returns the cross-sensitivities vol traders watch. Both
take the same arguments as `bs_price` and use the same cost-of-carry convention
(see [Price options and implied volatility](options-pricing.md)), so crypto is
the default with `rate=carry=0`.

## First-order greeks

```python
import flox_py as flox

g = flox.greeks(flox.OptionType.CALL, spot=100, strike=100, t=1.0, vol=0.20,
                rate=0.05, carry=0.05)
g["delta"]  # price change per 1.0 move in spot
g["gamma"]  # delta change per 1.0 move in spot
g["vega"]   # price change per 1.0 (100 vol-point) change in vol
g["theta"]  # price change per YEAR (divide by 365 for per-day)
g["rho"]    # price change per 1.0 change in the discount rate
```

Theta is annualized. Divide by 365 for the per-calendar-day decay. Rho is the
partial with respect to the discount rate holding carry fixed, which is the
unambiguous derivative in the generalized model; for plain-stock semantics where
carry equals the rate, add the carry sensitivity if you want both to move together.

## Delta-hedging a position

Delta is the hedge ratio. A long call carries positive delta, so a delta-neutral
hedge shorts `delta` units of the underlying per contract:

```python
hedge_units = -g["delta"]  # short this many underlying units per long call
```

Gamma tells you how fast that hedge drifts as spot moves: high gamma near the
strike means the hedge needs frequent rebalancing.

## Second-order greeks

```python
s = flox.second_order_greeks(flox.OptionType.CALL, spot=100, strike=100, t=1.0,
                             vol=0.20, rate=0.05, carry=0.05)
s["vanna"]  # d(delta)/d(vol), also d(vega)/d(spot)
s["volga"]  # d(vega)/d(vol)
s["charm"]  # d(delta)/d(time), per year
```

Vanna and volga matter when a position carries vega risk that itself moves with
spot or vol; charm is the delta drift from time passing, which a delta-neutral
book has to rehedge against even when spot is still.
