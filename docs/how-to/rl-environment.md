# Train a reinforcement-learning agent on a flox tape

`flox_py.rl_env.FloxTradingEnv` is a Gymnasium-compatible environment that drives an agent through the trades in a captured `.floxlog` tape. It speaks the standard `Env` protocol (`reset`, `step`, `render`, `close`, `action_space`, `observation_space`, `metadata`) without importing `gymnasium` itself, so plugging it into `stable_baselines3`, `RLlib`, or `CleanRL` does not pull gymnasium into flox's dependency surface. The user installs gymnasium and the learner of choice; the env works out of the box.

Two construction paths are available. `FloxTradingEnv.from_tape(...)` is the lightweight Phase 1 path — trade-by-trade replay with ideal fills, no fees, no funding, no liquidation. `FloxTradingEnv.from_venue_stack(stack, tape=...)` plugs the env on top of a `VenueStack` so every action routes through the same simulated executor used in backtest and paper trading; fees and funding feed back into the reward via the cross-margin Account, and liquidation terminates the episode. Phase 2 follow-ups will add continuous action spaces, limit-order semantics, and multi-symbol portfolios.

## Quick start

```python
from flox_py.rl_env import FloxTradingEnv

env = FloxTradingEnv.from_tape(
    "./tapes/bybit-btc-2026-05-07",
    qty=0.01,
    window_size=16,
)

obs, info = env.reset(seed=42)
total_reward = 0.0
done = False
while not done:
    action = env.action_space.sample()  # plug in your policy here
    obs, reward, terminated, truncated, info = env.step(action)
    total_reward += reward
    done = terminated or truncated

print(total_reward, info["realized_pnl"], info["position"])
```

`from_tape` loads the entire tape into memory at construction time. For long captures, slice the tape upstream; the env constructor also accepts a plain list of `(ts_ns, price, qty, side)` tuples if you want to drive it from a non-tape source.

## Action and observation spaces

The default action space is `Discrete(3)`:

| Action | Meaning |
|---|---|
| 0 | Hold (no order). |
| 1 | Go long `qty` (or stay long if already there). |
| 2 | Go short `qty` (or stay short if already there). |

Switching position closes the existing one at the current price (realizes PnL) and opens the new one.

The default observation is a `Box` of shape `(window_size + 2,)`:

- The first `window_size` entries are the most recent prices, normalized by the first observed price (so the values stay around 1.0).
- One entry for current position quantity (signed).
- One entry for unrealized PnL since the last position change.

Override `window_size` at construction. If you want a different observation, subclass `FloxTradingEnv` and override `_observation`.

## Reward

The default reward is the change in total PnL (realized plus unrealized) since the previous step. Pass `reward_fn=lambda env, ctx: ...` to compute your own; the callback receives the env and a context dict (`ts_ns`, `price`, `position`, `realized_pnl`, `unrealized_pnl`, `step`) and returns a float.

```python
def risk_adjusted_reward(env, ctx):
    pnl_delta = env._last_total_pnl  # if you read internals
    drawdown_penalty = max(0.0, -ctx["unrealized_pnl"]) * 0.1
    return float(pnl_delta - drawdown_penalty)

env = FloxTradingEnv.from_tape(path, reward_fn=risk_adjusted_reward)
```

The default in the `from_tape` path ignores transaction costs entirely. For honest training, switch to `from_venue_stack` — the venue-stack path computes reward as the change in account equity at mark, with taker (or maker, if `is_maker=True`) fees deducted on each fill via the stack's fee schedule. Funding accrued by the schedule and realized PnL on close are folded into equity automatically.

## Venue-stack backed env

```python
import flox_py
from flox_py.rl_env import FloxTradingEnv

stack = flox_py.VenueStack.binance_um_futures(account_id=1, equity=10_000.0)

env = FloxTradingEnv.from_venue_stack(
    stack,
    tape="./tapes/btc-2026-05-07",
    qty=0.01,
    window_size=16,
    symbol_id=1,
)
```

What changes versus `from_tape`:

- `step()` submits market orders through `stack.executor()` (the same `VenueExecutor` returned to any other Python caller of the stack), feeds the current trade tick to the matching engine, and drains the resulting fills.
- Fees come from `stack.fees().fee_for(...)` and are deducted from account equity on every fill. The fee schedule's 30d rolling notional advances on each fill, so the tier moves with realized volume — same behavior as a backtest.
- The cross-margin Account's `set_mark` is called every step, and `stack.liquidation().on_mark(...)` runs the liquidation walk. Episodes terminate on the first liquidation event.
- Reward is the change in `account.equity() + account.total_unrealised_pnl()` since the previous step. Fees, funding accruals, realized PnL on close, and unrealized PnL on mark all fold in naturally.
- `info` gains `equity`, `unrealized_pnl`, `equity_at_mark`, `fee_tier`, and `liquidation_outcome` fields so the agent's training loop can log the venue-side state.

Same strategy class, same data, the only thing that differs from `from_tape` is the realism around the fills. Pick this path for any training that will hand the trained policy to `PaperBroker` or `CcxtBroker` — the physics will match.

## Continuous actions

`from_venue_stack` defaults to a continuous action space — a `Box((3,))` with one axis for signed quantity, one for price offset in ticks, one for time-in-force. Discrete(3) stays available as `action_mode="discrete"` for Phase 1 compatibility.

| Axis | Range | Meaning |
|---|---|---|
| 0 | `[-1.0, +1.0]` | Signed qty as a fraction of `max_position`. `+1.0` = full long, `-1.0` = full short, `0.0` = flat |
| 1 | `[-N, +N]` ticks (`N = max_price_offset_ticks`, default 50) | Limit price offset from mid. `0` means market |
| 2 | `[0.0, 2.0]` | TIF, rounded to int: `0=GTC`, `1=IOC`, `2=Post-only` |

```python
env = FloxTradingEnv.from_venue_stack(
    stack, tape=tape,
    qty=0.01, max_position=0.05,
    tick_size=0.01,
    max_price_offset_ticks=50,
    # action_mode="continuous" is the default
)
# action: [signed_qty_fraction, price_offset_ticks, tif_flag]
obs, reward, term, trunc, info = env.step([0.5, 0.0, 0.0])    # market buy 50% of max_position
obs, reward, term, trunc, info = env.step([-1.0, 2.0, 2.0])   # post-only limit sell at mid + 2 ticks
```

Decode rules:

- `price_offset_ticks == 0` (after rounding) means **market order** — TIF axis is ignored, the order routes through the executor at the most recent trade price.
- `price_offset_ticks != 0` means **limit order**. The price is `mid + offset * tick_size * side_sign`, where mid is the latest trade price (Phase 1 approximation; T034 will swap in the best bid / best ask) and `side_sign` is `+1` for buys, `-1` for sells.
- The TIF axis rounds to the nearest int. Out-of-range values are clipped to the box bounds.
- Out-of-bounds actions are **clipped** (not raised). A `RuntimeWarning` is emitted and `info["action_clipped"] = True` so a learner that occasionally samples outside the box does not crash the env.

For Phase 1 prototypes, pass `action_mode="discrete"` to keep the `Discrete(3)` interface — same semantics as before T033.

## stable_baselines3 with continuous actions

```python
from stable_baselines3 import PPO

env = FloxTradingEnv.from_venue_stack(stack, tape=tape, qty=0.01)
model = PPO("MlpPolicy", env, verbose=1)
model.learn(total_timesteps=100_000)
```

`MlpPolicy` handles the Box action space natively. For discrete-only runners (e.g. DQN), construct with `action_mode="discrete"`.

## Plugging into stable_baselines3

```python
import gymnasium as gym
from stable_baselines3 import PPO
from flox_py.rl_env import FloxTradingEnv

env = FloxTradingEnv.from_tape("./tapes/btc-2026-05-07", qty=0.01)
# stable_baselines3 wraps any Gymnasium env; FloxTradingEnv passes
# the duck-typed Env protocol it expects.
model = PPO("MlpPolicy", env, verbose=1)
model.learn(total_timesteps=100_000)
```

`stable_baselines3` does not introspect the env class identity; it calls the documented Env methods. The duck-typed `_DiscreteSpace` and `_BoxSpace` mirror the gymnasium API closely enough for both observation and action sampling to work. If your library is stricter, wrap the env in `gym.make` with a custom registration or substitute the spaces with real `gymnasium.spaces.Discrete` / `gymnasium.spaces.Box` objects post-construction.

## What this is not

- A multi-symbol portfolio simulator. One symbol per env in both construction paths. Multi-instrument is Phase 2.
- A live agent runtime. The env consumes a captured tape; for live RL you need an outer loop that feeds new trades and re-runs the policy. The same `step` / `reset` API applies, but the data plumbing is up to you.
- A continuous-action interface. Phase 1 keeps `Discrete(3)` everywhere; Phase 2 adds a `Box` action space for signed quantity, price offset in ticks, and TIF.

## See also

* [Record and replay tapes](tape-record.md). The format the env consumes.
* [Backtest with realistic fills](backtest-realistic-fills.md). Slippage and queue knobs on the simulator side.
* [Backtest with latency](backtest-with-latency.md). Latency primitives that pair with the env when you want fill realism.
