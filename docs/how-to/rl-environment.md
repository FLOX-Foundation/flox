# Train a reinforcement-learning agent on a flox tape

`flox_py.rl_env.FloxTradingEnv` is a Gymnasium-compatible environment that drives an agent through the trades in a captured `.floxlog` tape. It speaks the standard `Env` protocol (`reset`, `step`, `render`, `close`, `action_space`, `observation_space`, `metadata`) without importing `gymnasium` itself, so plugging it into `stable_baselines3`, `RLlib`, or `CleanRL` does not pull gymnasium into flox's dependency surface. The user installs gymnasium and the learner of choice; the env works out of the box.

Phase 1 stays narrow: trade-by-trade replay, scalar `qty`, three discrete actions (hold, long, short). Continuous action spaces, multi-instrument portfolios, and limit-order semantics are Phase 2 follow-ups.

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

The default ignores transaction costs entirely. For honest training, layer fees and slippage into the reward function or compose this env with `flox_py.SimulatedExecutor` (which already handles fees and queue-aware fills) for the fill side.

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

- A backtest in the `flox_py.bundle` sense. The env replays trades and applies idealized fills (current trade price, instant). Realistic fills (slippage, queue position, latency) need `flox_py.SimulatedExecutor` and the latency-models module on top.
- A multi-symbol portfolio simulator. One symbol per env. Multi-instrument is Phase 2.
- A live agent runtime. The env consumes a captured tape; for live RL you need an outer loop that feeds new trades and re-runs the policy. The same `step` / `reset` API applies, but the data plumbing is up to you.

## See also

* [Record and replay tapes](tape-record.md). The format the env consumes.
* [Backtest with realistic fills](backtest-realistic-fills.md). Slippage and queue knobs on the simulator side.
* [Backtest with latency](backtest-with-latency.md). Latency primitives that pair with the env when you want fill realism.
