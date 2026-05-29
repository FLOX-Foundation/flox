# Price options and back out implied volatility

`flox_py` ships a generalized Black-Scholes-Merton pricer and an implied-volatility
solver as plain module functions. They cover European options on any underlying by
varying one parameter, the cost-of-carry `b`:

| Case | `carry` (b) |
|------|-------------|
| Crypto (Deribit), priced off the forward | `0.0` (also `rate=0.0`) |
| Option on a future (Black-76) | `0.0` |
| Non-dividend stock | `r` |
| Continuous dividend yield `q` | `r - q` |
| FX (Garman-Kohlhagen) | `r - rf` |

Crypto options are the common case, so `rate` and `carry` both default to `0.0`.

## Price a call and a put

```python
import flox_py as flox

# BTC 30-day ATM call, 60% vol, priced off the forward (rate=carry=0).
call = flox.bs_price(flox.OptionType.CALL, spot=70000, strike=70000, t=30/365, vol=0.60)
put = flox.bs_price(flox.OptionType.PUT, spot=70000, strike=70000, t=30/365, vol=0.60)
```

`t` is time to expiry in years. At expiry (`t=0`) or zero vol the function returns the
discounted intrinsic value instead of failing, so a settled contract is safe to price.

## Equity option with a discount rate

Pass `rate` and `carry` for a dated equity option. Here a one-year call on a
non-dividend stock at a 5% rate (so `carry = rate`):

```python
call = flox.bs_price(flox.OptionType.CALL, spot=100, strike=100, t=1.0, vol=0.20,
                     rate=0.05, carry=0.05)
# 10.4506
```

## Back out implied volatility

`implied_vol` inverts a quoted price to the vol that reproduces it. It uses
Newton-Raphson with a bracketed bisection fallback, so it still lands when vega
collapses on deep in- or out-of-the-money strikes.

```python
result = flox.implied_vol(flox.OptionType.CALL, price=call, spot=100, strike=100,
                          t=1.0, rate=0.05, carry=0.05)
result["vol"]        # 0.20
result["converged"]  # True
result["iterations"] # iterations used
```

When the quoted price breaks no-arbitrage bounds (below intrinsic or above the asset
value), `converged` is `False` and `vol` is `NaN`. Check `converged` before using the
result.

## Vega and forward price

```python
flox.bs_vega(spot=100, strike=100, t=1.0, vol=0.20, rate=0.05, carry=0.05)
flox.forward_price(spot=100, t=1.0, carry=0.05)
```

`bs_vega` is the sensitivity of price to a 1.0 (100 vol-point) change in volatility and
is identical for calls and puts. The full greek set (delta, gamma, theta, rho, plus the
second-order vanna, volga, charm) is covered in a separate guide.
