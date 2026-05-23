# Cross-margin accounts

Real prop accounts almost universally run cross-margin: equity is
shared across all positions on the account, so a profitable BTC
short backs a losing ETH long. Backtests that treat each position
in isolation overstate liquidation risk for cross-margined
portfolios (because cross has more shared cushion) and understate
the systemic risk when one position drags the whole account.

W15's `Account` type owns the shared state — equity, the position
book across symbols, per-symbol mark prices, and a 30-day rolling
notional counter — and plugs into `LiquidationEngine` and
`FeeSchedule` so they evaluate at the account level instead of
per-position.

## Build an account

=== "Python"

    ```python
    import flox

    acct = flox.Account(account_id=42, equity=10_000.0)
    acct.open_position(symbol=1, quantity=5.0, entry_price=50_000.0)
    acct.open_position(symbol=2, quantity=-30.0, entry_price=3_000.0)
    ```

=== "TypeScript"

    ```typescript
    import { Account } from "flox";

    const acct = new Account(42, 10_000);
    acct.openPosition(1, 5.0, 50_000);
    acct.openPosition(2, -30.0, 3_000);
    ```

The default margin mode is `cross`. Switch to `isolated` per-account
with `set_margin_mode("isolated")` / `setMarginMode("isolated")`.

## Cross-margin liquidation

Attach the account to a `LiquidationEngine`. The engine walks
attached accounts on every `on_marks` tick: for accounts in cross
mode, it evaluates the account-level maintenance-margin check
(`equity + total_uPnL` vs `total_notional * mm_fraction`) and, when
the account is underwater, closes the worst-PnL position first.

Use `on_marks(...)` (T053) with the full set of current marks per
tick — it updates every attached account's marks atomically before
walking. The legacy single-symbol `on_mark(...)` is still
available but is a footgun for multi-symbol accounts: forgetting
to set the other symbols' marks leaves the cross-margin check
evaluating against stale data.

=== "Python"

    ```python
    liq = flox.LiquidationEngine.binance_um_futures()
    liq.attach_account(acct)

    # Atomic multi-symbol update; engine walks every attached
    # account once it has fresh marks for every position.
    out = liq.on_marks([(1, 47_000.0), (2, 2_800.0)], ts_ns=now)
    print(out["liquidations_count"])
    ```

=== "TypeScript"

    ```typescript
    const liq = new flox.LiquidationEngine();
    liq.loadProfile("binance_um_futures");
    liq.attachAccount(acct);

    const n = liq.onMarks([[1, 47_000], [2, 2_800]], now);
    ```

### Stale-mark guard

When a backtest must refuse to walk on stale data, set timestamps
explicitly via `set_mark(sym, price, ts_ns)` (or pass `ts_ns` to
`on_marks`) and check the account before driving the engine:

=== "Python"

    ```python
    if acct.has_stale_marks(now_ns=now, budget_ns=60_000_000_000):
        raise RuntimeError("refresh marks before walking")
    liq.on_marks(current_marks, ts_ns=now)
    ```

=== "TypeScript"

    ```typescript
    if (acct.hasStaleMarks(now, 60_000_000_000)) {
        throw new Error("refresh marks before walking");
    }
    liq.onMarks(currentMarks, now);
    ```

When a profitable short backs a losing long, the account stays
solvent and no liquidation fires. When both legs bleed, the engine
closes the worst leg, re-checks, and continues until the account
is solvent or no positions remain. Any residual equity deficit
hits the insurance fund (and ADL, if configured).

## Shared 30-day fee tier

Real venues compute the VIP tier from aggregate 30-day notional
across all symbols, not per-symbol. Binding the account to one or
more `FeeSchedule`s makes them read the aggregate counter:

=== "Python"

    ```python
    btc_sched = flox.FeeSchedule.binance_um_futures()
    eth_sched = flox.FeeSchedule.binance_um_futures()
    btc_sched.bind_account(acct)
    eth_sched.bind_account(acct)

    btc_sched.record_fill(ts_ns=0, notional=150_000)
    eth_sched.record_fill(ts_ns=0, notional=150_000)
    # Aggregate 300k crosses Binance VIP 1 (>= 250k). Both
    # schedules now resolve at the higher tier.
    assert btc_sched.current_tier_index() >= 1
    ```

=== "TypeScript"

    ```typescript
    const btcSched = new flox.FeeSchedule();
    btcSched.loadProfile("binance_um_futures");
    const ethSched = new flox.FeeSchedule();
    ethSched.loadProfile("binance_um_futures");
    btcSched.bindAccount(acct);
    ethSched.bindAccount(acct);

    btcSched.recordFill(0, 150_000);
    ethSched.recordFill(0, 150_000);
    ```

The account's rolling counter ages out fills older than 30 days
automatically (matching the venue's window).

## Isolated mode

Isolated accounts skip the cross-margin walk in the engine — each
position liquidates against its own posted margin via the existing
per-position path. Switch via the margin mode:

=== "Python"

    ```python
    acct.set_margin_mode("isolated")
    ```

=== "TypeScript"

    ```typescript
    acct.setMarginMode("isolated");
    ```

## Notes

- `Account` is non-owning from the engine's perspective. The
  caller manages lifetime; the language binding's keep-alive
  semantics prevent premature GC.
- Multiple accounts may attach to the same engine — the walk
  iterates them all.
- Multiple `FeeSchedule`s sharing the same account see a
  consistent aggregate counter; `current_tier_index()` resolves
  on-demand when bound so a counter increment from one schedule is
  immediately visible to the others.
- Cross-pool collateral (e.g. Binance USDT vs BUSD pools),
  multi-currency accounts, and venue-specific account-tier fee
  discounts are out of scope.
