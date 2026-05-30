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

`cost_of_carry` assembles `b` from the financing pieces an asset actually has, so
the same pricer covers crypto and equities. Crypto passes everything zero; an
equity nets the dividend yield and stock borrow:

```python
b = flox.cost_of_carry(rate=0.05, dividend_yield=0.02, borrow_rate=0.01)  # 0.02
call = flox.bs_price(flox.OptionType.CALL, spot=100, strike=100, t=1.0, vol=0.20,
                     rate=0.05, carry=b)
```

Single-name equities pay lumpy cash dividends, not a smooth yield.
`bs_price_discrete_dividends` uses the escrowed-dividend model: it subtracts the
present value of the dividends paid before expiry from the spot. A dividend lowers
a call and lifts a put.

```python
# A $3 dividend paid in six months, one-year option.
call = flox.bs_price_discrete_dividends(flox.OptionType.CALL, spot=100, strike=100,
                                        t=1.0, vol=0.25, rate=0.05,
                                        dividends=[(0.5, 3.0)])
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

## American options (early exercise)

Black-Scholes prices European options, which can only be exercised at expiry.
US equity options are American: the holder can exercise early, and that right is
worth something. `flox_py` prices it two ways.

`binomial_price` is a Cox-Ross-Rubinstein lattice. With `american=True` it checks
early exercise at every node; with `american=False` it is a European tree that
converges to `bs_price` as `steps` grows.

```python
# One-year American put, 8% rate, no carry -> a real early-exercise premium.
amer = flox.binomial_price(flox.OptionType.PUT, spot=100, strike=100, t=1.0, vol=0.30,
                           rate=0.08, steps=500, american=True)
euro = flox.bs_price(flox.OptionType.PUT, spot=100, strike=100, t=1.0, vol=0.30, rate=0.08)
amer > euro  # True: the right to exercise early is worth more
```

`baw_price` is the Barone-Adesi-Whaley closed-form approximation. It is far cheaper
than a fine tree, so it is the engine to reach for when computing greeks by finite
differences. An American call whose carry covers the rate (`carry >= rate`) is never
exercised early, so it returns the European price exactly.

```python
amer = flox.baw_price(flox.OptionType.PUT, spot=100, strike=100, t=1.0, vol=0.30, rate=0.08)
```

A finer tree is more accurate but slower; BAW trades a small approximation error for
speed. Pick the lattice when you need a reference value, BAW when you need many prices.

## Volatility surface (SVI)

Marking every option at one flat vol is a toy. A real backtest marks each step to
the volatility surface as it was on that date. `flox_py` fits a raw-SVI surface and
serves point-in-time, no-lookahead vols.

Calibrate one expiry's smile from observed log-moneyness and total variance
(`iv**2 * t`):

```python
params = flox.calibrate_svi(log_moneyness=[-0.4, -0.2, 0.0, 0.2, 0.4],
                            total_variance=[0.055, 0.050, 0.048, 0.051, 0.058])
params["rho"]  # skew of the fitted slice
```

Stack slices into a surface and read an implied vol at any strike and expiry. The
surface interpolates in total-variance space and can check itself for calendar
arbitrage:

```python
surf = flox.VolSurface()
surf.add_slice(t=0.25, **params)
surf.add_slice(t=1.00, a=0.06, b=0.10, rho=-0.2, m=0.0, sigma=0.2)
surf.implied_vol(log_moneyness=0.0, t=0.5)  # vol to mark a 6-month ATM option
surf.is_calendar_free()                     # True when variance rises with time
```

For a historical backtest, build the surface from timestamped quotes and pass the
as-of time. Only quotes stamped on or before it are used, so the surface is exactly
what you could have seen then — no peeking at the future:

```python
# quotes: list of (ts_ns, t, log_moneyness, iv)
surf = flox.build_surface_as_of(quotes, asof_ns=cutoff)
```

Expiries with fewer than five quotes are skipped — too thin to identify the five SVI
parameters.

## Volatility cone (realized vs implied)

A vol cone is the backdrop a trader reads implied against: for each horizon it slides
a window across the price history, annualizes the realized vol in each window, and
reports the distribution. Plot today's implied on it to see whether options are rich
or cheap versus how the underlying has actually moved.

```python
# periods_per_year is 365 for 24/7 crypto, 252 for equities.
cone = flox.vol_cone(prices, horizons=[10, 30, 60, 90], periods_per_year=365)
cone[0]["p50"]  # median realized vol at the 10-period horizon
```

To place an implied quote in the cone, pull the realized samples for one horizon and
ask what fraction sit below it — high means rich, low means cheap:

```python
samples = flox.vol_cone(prices, [30], 365)  # or compute realized_vol per window
# implied_percentile_in_cone(realized_samples, implied_vol) -> 0..1
rich = flox.implied_percentile_in_cone(realized_samples, implied_vol=0.65)
```

## Vega and forward price

```python
flox.bs_vega(spot=100, strike=100, t=1.0, vol=0.20, rate=0.05, carry=0.05)
flox.forward_price(spot=100, t=1.0, carry=0.05)
```

`bs_vega` is the sensitivity of price to a 1.0 (100 vol-point) change in volatility and
is identical for calls and puts. The full greek set (delta, gamma, theta, rho, plus the
second-order vanna, volga, charm) is covered in a separate guide.
