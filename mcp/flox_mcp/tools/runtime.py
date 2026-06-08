"""Runtime MCP tools — `run_backtest`, `compute_indicator`, `suggest_indicator`.

These are the W2-T012 follow-up tools. Two run user / agent-supplied
data through real Python code at request time, so they need different
safety treatment than the read-only IR / docs tools:

* :func:`run_backtest` runs **arbitrary user-supplied strategy code**
  in a subprocess with rlimits and a wall-clock timeout. The sandbox
  is the MVP described in T012 — subprocess + ``resource.setrlimit``
  on POSIX, plus ``subprocess.run(..., timeout=...)``. Production
  hardening (filesystem isolation, network namespaces) is out of
  scope here; the docstring + README point users at nsjail / firejail
  for that.
* :func:`compute_indicator` calls the bundled FLOX indicator classes
  directly. Input is capped at 1 MB so a runaway agent can't OOM the
  MCP host. Requires ``flox-py`` to be installed
  (``pip install "flox-mcp[flox]"``).
* :func:`suggest_indicator` is a pure-string keyword heuristic over
  the indicator catalogue — no LLM, no flox-py needed. Returns the
  three best-matching candidates with the keyword(s) that triggered
  each match, so the agent can ground the actual recommendation in
  ``list_indicators`` / ``lookup_symbol``.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable, List, Mapping, Optional, Tuple


# ── run_backtest ──────────────────────────────────────────────────────


# Sandbox limits per W2-T012. Defaults are conservative so the MCP
# host stays responsive even if every backtest pegs CPU.
DEFAULT_CPU_SECONDS = 60
DEFAULT_RSS_BYTES = 1 * 1024 * 1024 * 1024   # 1 GiB
DEFAULT_FSIZE_BYTES = 64 * 1024 * 1024       # 64 MiB output cap
DEFAULT_WALL_TIMEOUT_S = 60

MAX_STRATEGY_CODE_BYTES = 256 * 1024         # 256 KiB code cap
MAX_DATASET_BYTES = 64 * 1024 * 1024         # 64 MiB dataset cap

_REPO_ROOT_GUESS = Path(__file__).resolve().parents[3]
_WORKER_SCRIPT = Path(__file__).resolve().parent / "_runtime_worker.py"


def _validate_dataset_path(dataset_path: str) -> Tuple[Optional[Path], Optional[str]]:
    if not dataset_path:
        return None, "run_backtest: `dataset_path` is required."
    p = Path(dataset_path).expanduser()
    if not p.is_file():
        return None, f"run_backtest: dataset file not found: {p}"
    try:
        size = p.stat().st_size
    except OSError as e:
        return None, f"run_backtest: cannot stat dataset: {e}"
    if size > MAX_DATASET_BYTES:
        return None, (
            f"run_backtest: dataset is {size / 1024 / 1024:.1f} MB, "
            f"over the {MAX_DATASET_BYTES / 1024 / 1024:.0f} MB cap. "
            f"Trim the file or use a smaller window."
        )
    return p, None


def run_backtest(
    strategy_code: str,
    dataset_path: str,
    *,
    symbol: str = "BTCUSDT",
    cpu_seconds: int = DEFAULT_CPU_SECONDS,
    rss_bytes: int = DEFAULT_RSS_BYTES,
    fsize_bytes: int = DEFAULT_FSIZE_BYTES,
    wall_timeout_s: int = DEFAULT_WALL_TIMEOUT_S,
) -> str:
    """Run a strategy against a CSV dataset in a subprocess sandbox.

    The ``strategy_code`` must define a ``Strategy`` subclass at the
    module level; the worker imports it via ``exec`` and hands it to
    ``BacktestRunner.set_strategy``. The sandbox returns the FLOX
    backtest stats dict as JSON, plus captured stdout / stderr.

    NOTE: this is the MVP sandbox. It limits CPU, memory, and output
    file size on POSIX. It does NOT isolate the filesystem, network,
    or syscalls. Treat hostile input the same way you would any
    untrusted Python — a real production deployment should wrap the
    worker with nsjail / firejail / Docker.
    """
    if not isinstance(strategy_code, str) or not strategy_code.strip():
        return "run_backtest: `strategy_code` must be a non-empty string."
    if len(strategy_code.encode("utf-8")) > MAX_STRATEGY_CODE_BYTES:
        return (
            f"run_backtest: strategy_code is over the "
            f"{MAX_STRATEGY_CODE_BYTES // 1024} KiB cap."
        )
    dataset, err = _validate_dataset_path(dataset_path)
    if err:
        return err

    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        code_file = td_path / "strategy.py"
        code_file.write_text(strategy_code)
        out_file = td_path / "result.json"

        cmd = [
            sys.executable,
            str(_WORKER_SCRIPT),
            "--code", str(code_file),
            "--dataset", str(dataset),
            "--symbol", symbol,
            "--cpu-seconds", str(cpu_seconds),
            "--rss-bytes", str(rss_bytes),
            "--fsize-bytes", str(fsize_bytes),
            "--out", str(out_file),
        ]

        try:
            proc = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=wall_timeout_s,
            )
        except subprocess.TimeoutExpired as exc:
            return (
                f"# run_backtest: TIMEOUT\n\n"
                f"Worker exceeded the wall-clock limit of "
                f"{wall_timeout_s}s. Dataset / strategy may be too "
                f"large, or the strategy may have an infinite loop.\n\n"
                f"```\n{(exc.stdout or '')[-2000:]}\n```\n"
            )

        result_payload: Optional[dict] = None
        if out_file.is_file():
            try:
                result_payload = json.loads(out_file.read_text())
            except Exception:
                result_payload = None

        if proc.returncode != 0 or result_payload is None:
            stderr = (proc.stderr or "")[-4000:]
            stdout = (proc.stdout or "")[-1000:]
            return (
                f"# run_backtest: FAILED (exit {proc.returncode})\n\n"
                f"## stderr\n```\n{stderr or '(empty)'}\n```\n\n"
                f"## stdout\n```\n{stdout or '(empty)'}\n```\n"
            )

        if result_payload.get("status") != "ok":
            return (
                f"# run_backtest: worker reported an error\n\n"
                f"```\n{json.dumps(result_payload, indent=2)[:4000]}\n```\n"
            )

        stats = result_payload.get("stats") or {}
        n_keys = len(stats)
        lines = [f"# run_backtest: OK ({n_keys} stat(s))", "", "```json",
                 json.dumps(stats, indent=2, sort_keys=True), "```"]
        captured_stdout = (result_payload.get("stdout") or "").strip()
        if captured_stdout:
            lines += ["", "## strategy stdout",
                      "```", captured_stdout[:2000], "```"]
        return "\n".join(lines)


# ── compute_indicator ─────────────────────────────────────────────────


# Cap on the size of the input array. ~1 MB of float64 is ~125k
# samples — plenty for an interactive call, and small enough that the
# MCP host is not at risk if the agent passes garbage.
MAX_INDICATOR_INPUT_BYTES = 1 * 1024 * 1024


def _import_flox_or_error() -> Tuple[Any, Optional[str]]:
    try:
        import flox_py  # type: ignore
        return flox_py, None
    except ImportError:
        return None, (
            "compute_indicator requires the optional `flox-py` package. "
            "Install it with `pip install \"flox-mcp[flox]\"` "
            "(or `pip install flox-py`)."
        )


def price_amm_swap(
    venue: str,
    pool: Any,
    amount_in: Any,
    i: int = 0,
    j: int = 1,
) -> str:
    """Price an exact DEX swap against a pool, to the wei.

    venue: constant_product | raydium_cp | uniswap_v3. pool: the pool's parameters
    (amounts as decimal strings, so 256-bit values are lossless). Returns JSON with
    the exact output amount and the marginal price, computed through the exact curve.
    """
    import json as _json

    flox, err = _import_flox_or_error()
    if err is not None:
        return err
    if not isinstance(pool, dict):
        return "price_amm_swap: `pool` must be an object of pool parameters."

    def _int(v: Any) -> int:
        return int(str(v))

    v = (venue or "").lower()
    try:
        if v in ("constant_product", "cp", "uniswap_v2"):
            curve = flox.AmmCurve.constant_product(
                _int(pool["reserve0"]), _int(pool["reserve1"]),
                int(pool.get("fee_num", 997)), int(pool.get("fee_den", 1000)))
        elif v in ("raydium_cp", "raydium"):
            curve = flox.AmmCurve.raydium_cp(
                _int(pool["reserve0"]), _int(pool["reserve1"]),
                int(pool["trade_fee_rate"]), int(pool.get("creator_fee_rate", 0)),
                bool(pool.get("creator_fee_on_input", True)))
        elif v in ("uniswap_v3", "v3", "concentrated_liquidity"):
            ticks = [(_int(t[0]), _int(t[1])) for t in pool.get("ticks", [])]
            curve = flox.AmmCurve.uniswap_v3(
                _int(pool["sqrt_price_x96"]), _int(pool["liquidity"]),
                int(pool["fee_pips"]), ticks)
        else:
            return (
                f"price_amm_swap: unknown venue '{venue}'. Use one of "
                "constant_product, raydium_cp, uniswap_v3."
            )
    except KeyError as e:
        return f"price_amm_swap: missing pool parameter {e} for venue '{v}'."
    except Exception as e:  # noqa: BLE001
        return f"price_amm_swap: bad pool parameters: {e}"

    amt = _int(amount_in)
    out = curve.amount_out(int(i), int(j), amt)
    # Marginal price from a tiny probe (a millionth of the input, min 1).
    probe = max(amt // 1_000_000, 1)
    probe_out = curve.amount_out(int(i), int(j), probe)
    marginal = (probe_out / probe) if probe > 0 else 0.0
    realized = (out / amt) if amt > 0 else 0.0
    impact = (1.0 - realized / marginal) if marginal > 0 else 0.0
    return _json.dumps({
        "venue": v,
        "amount_in": str(amt),
        "amount_out": str(out),
        "i": int(i),
        "j": int(j),
        "marginal_price": marginal,
        "realized_price": realized,
        "price_impact": impact,
        "note": "exact to the wei via the flox curve",
    }, indent=2)


def _import_dex_or_error() -> Tuple[Any, Optional[str]]:
    try:
        from flox_py import dex  # type: ignore
        return dex, None
    except ImportError:
        return None, (
            "this tool requires the optional `flox-py` package (>= the build that "
            "ships flox_py.dex). Install it with `pip install \"flox-mcp[flox]\"`."
        )


def _build_dex_pool(dex: Any, spec: Any) -> Any:
    """Build a flox.dex pool from an agent-facing spec.

    spec: {venue, token0:{symbol,decimals}, token1:{symbol,decimals}, ...venue params}.
    constant_product / uniswap_v2: reserves (["1000 WETH","2000000 USDC"]), fee, fee_den.
    raydium_cp: reserves, trade_fee. uniswap_v3: sqrt_price_x96, liquidity, fee, ticks.
    """
    if not isinstance(spec, dict):
        raise ValueError("pool must be an object of pool parameters")
    t = spec.get("token0"), spec.get("token1")
    if not all(isinstance(x, dict) and "symbol" in x and "decimals" in x for x in t):
        raise ValueError("token0/token1 must be {symbol, decimals} objects")
    t0 = dex.Token(t[0]["symbol"], int(t[0]["decimals"]))
    t1 = dex.Token(t[1]["symbol"], int(t[1]["decimals"]))
    v = (spec.get("venue") or "").lower()
    if v in ("constant_product", "cp", "uniswap_v2"):
        return dex.UniswapV2(t0, t1, reserves=spec["reserves"],
                             fee=spec.get("fee", "0.30%"), fee_den=int(spec.get("fee_den", 1000)))
    if v in ("raydium_cp", "raydium"):
        return dex.RaydiumCp(t0, t1, reserves=spec["reserves"],
                             trade_fee=spec.get("trade_fee", "0.25%"))
    if v in ("uniswap_v3", "v3", "concentrated_liquidity"):
        ticks = [(int(str(a)), int(str(b))) for a, b in spec.get("ticks", [])]
        return dex.UniswapV3(t0, t1, int(str(spec["sqrt_price_x96"])),
                             int(str(spec["liquidity"])), fee=spec.get("fee", "0.05%"), ticks=ticks)
    raise ValueError(f"unknown venue '{spec.get('venue')}'. Use one of "
                     "constant_product, raydium_cp, uniswap_v3.")


def _rows_to_list(rows: Any) -> list:
    """A flox.dex table (pandas DataFrame or list of dicts) to a list of dicts.

    Wei-scale fields (reserves) are coerced to decimal strings so a JSON consumer does
    not lose precision past 2^53; small fields (ts, price, il) keep their native type.
    """
    if not isinstance(rows, list):
        rows = rows.to_dict("records")  # pandas DataFrame
    wei_keys = ("reserve0", "reserve1")
    out = []
    for r in rows:
        out.append({k: (str(val) if k in wei_keys and val is not None else val)
                    for k, val in r.items()})
    return out


def route_amm_swap(venues: Any, amount: Any, into: Any = None) -> str:
    """Best execution across a set of AMM pools, exact to the wei via flox.dex.Router.

    venues: a list of pool specs (see _build_dex_pool). amount: "NUMBER SYMBOL".
    into: the out-token symbol (optional). Returns the winning venue, the fill, and the
    per-venue comparison table.
    """
    import json as _json

    dex, err = _import_dex_or_error()
    if err is not None:
        return err
    if not isinstance(venues, list) or not venues:
        return "route_amm_swap: `venues` must be a non-empty list of pool specs."
    try:
        pools = [_build_dex_pool(dex, s) for s in venues]
        router = dex.Router(pools)
        best_pool, best_q = router.best(amount, into)
        table = router.table(amount, into)
    except Exception as e:  # noqa: BLE001
        return f"route_amm_swap: {e}"
    table = _rows_to_list(table)
    return _json.dumps({
        "amount_in": str(amount),
        "best": {
            "venue": best_pool.venue,
            "out": str(best_q.out),
            "out_wei": str(best_q.exact_wei),
            "price_impact": float(best_q.price_impact),
        },
        "venues": table,
        "note": "best execution exact to the wei via flox.dex.Router",
    }, indent=2)


def amm_price_impact(pool: Any, sizes: Any) -> str:
    """The depth / slippage table for a pool: realized price and impact per size.

    pool: a pool spec (see _build_dex_pool). sizes: a list of "NUMBER SYMBOL" amounts.
    Use this to reason about size before quoting. Exact to the wei via flox.dex.
    """
    import json as _json

    dex, err = _import_dex_or_error()
    if err is not None:
        return err
    if not isinstance(sizes, list) or not sizes:
        return "amm_price_impact: `sizes` must be a non-empty list of amounts."
    try:
        p = _build_dex_pool(dex, pool)
        rows = _rows_to_list(p.depth(sizes))
    except Exception as e:  # noqa: BLE001
        return f"amm_price_impact: {e}"
    return _json.dumps({
        "venue": p.venue,
        "spot_price": float(p.spot_price),
        "depth": rows,
        "note": "depth / slippage exact to the wei via flox.dex",
    }, indent=2)


def replay_pool_tape(pool: Any, swaps: Any = None, evm_logs: Any = None) -> str:
    """Replay a recorded pool history through the exact curve into a backtest series.

    pool: a pool spec (see _build_dex_pool). Supply either `swaps` (a list of
    [ts, "NUMBER SYMBOL", into?]) or `evm_logs` (decoded Uniswap v2 Swap log dicts).
    Returns the price / reserves / LP-value / IL / drift series so an agent can backtest
    a DEX position from a transcript.
    """
    import json as _json

    dex, err = _import_dex_or_error()
    if err is not None:
        return err
    if not swaps and not evm_logs:
        return "replay_pool_tape: supply either `swaps` or `evm_logs`."
    try:
        p = _build_dex_pool(dex, pool)
        if evm_logs:
            tape = dex.Tape.from_evm_logs(p, evm_logs)
        else:
            tape = dex.Tape(p).from_swaps([tuple(s) for s in swaps])
        bt = tape.replay()
    except Exception as e:  # noqa: BLE001
        return f"replay_pool_tape: {e}"
    drift = bt.attrs["drift_count"] if not isinstance(bt, list) else \
        sum(1 for r in bt if r.get("drift"))
    return _json.dumps({
        "venue": p.venue,
        "drift_count": int(drift),
        "series": _rows_to_list(bt),
        "note": "exact replay through the flox.dex curve",
    }, indent=2)


def compute_indicator(
    name: str,
    data: Any,
    **kwargs: Any,
) -> str:
    """Run a single FLOX indicator over ``data`` and return its output.

    Tries the class form first (``flox_py.<Name>``) and, if absent,
    falls back to the function form (``flox_py.<name.lower()>``).
    Returns Markdown with the first 32 / last 32 output values plus
    summary stats so the agent can sanity-check the result without
    flooding the conversation.
    """
    if not isinstance(name, str) or not name:
        return "compute_indicator: `name` is required and must be a string."
    if not isinstance(data, (list, tuple)):
        return "compute_indicator: `data` must be a list of numbers."
    n = len(data)
    if n == 0:
        return "compute_indicator: `data` is empty."
    # 8 bytes per float64 — bound the array before allocating.
    if n * 8 > MAX_INDICATOR_INPUT_BYTES:
        return (
            f"compute_indicator: input has {n} samples, over the "
            f"{MAX_INDICATOR_INPUT_BYTES // 1024} KiB cap. Trim it."
        )

    flox, err = _import_flox_or_error()
    if err:
        return err

    cls = getattr(flox, name, None)
    if cls is None:
        cls = getattr(flox, name.upper(), None)
    fn = getattr(flox, name.lower(), None)

    try:
        import numpy as np  # type: ignore
        arr = np.asarray(data, dtype=float)
    except Exception as exc:
        return f"compute_indicator: cannot coerce data to float64: {exc}"

    try:
        if cls is not None and callable(cls) and isinstance(cls, type):
            instance = cls(**kwargs) if kwargs else cls()
            if hasattr(instance, "compute"):
                out = instance.compute(arr)
            else:
                # Streaming-only indicator — manual loop.
                out = [instance.update(float(x)) for x in arr]
        elif fn is not None:
            out = fn(arr, **kwargs) if kwargs else fn(arr)
        else:
            return (
                f"compute_indicator: no indicator named `{name}` in flox_py. "
                f"Use `list_indicators` to see the catalogue."
            )
    except TypeError as exc:
        return (
            f"compute_indicator: `{name}` rejected the call signature "
            f"({type(exc).__name__}: {exc}). Check kwargs against "
            f"`list_indicators`."
        )
    except Exception as exc:
        return (
            f"compute_indicator: `{name}` raised "
            f"{type(exc).__name__}: {exc}"
        )

    return _format_indicator_output(name, out, kwargs)


def _format_indicator_output(name: str, out: Any, kwargs: Mapping[str, Any]) -> str:
    try:
        import numpy as np  # type: ignore
    except ImportError:
        np = None

    if np is not None and isinstance(out, np.ndarray):
        finite = out[np.isfinite(out)] if out.size else out
        head = out[:32].tolist()
        tail = out[-32:].tolist() if out.size > 32 else []
        n_finite = int(finite.size)
        stats: dict = {"length": int(out.size), "finite": n_finite}
        if n_finite:
            stats["min"] = float(finite.min())
            stats["max"] = float(finite.max())
            stats["mean"] = float(finite.mean())
            stats["last"] = float(out[-1]) if out.size else None
    else:
        seq = list(out) if not isinstance(out, list) else out
        head = seq[:32]
        tail = seq[-32:] if len(seq) > 32 else []
        valid = [v for v in seq if isinstance(v, (int, float)) and v == v]
        stats = {"length": len(seq), "finite": len(valid)}
        if valid:
            stats["min"] = float(min(valid))
            stats["max"] = float(max(valid))
            stats["mean"] = float(sum(valid) / len(valid))
            stats["last"] = seq[-1] if seq else None

    kw_repr = ", ".join(f"{k}={v}" for k, v in kwargs.items())
    title = f"# compute_indicator: `{name}`" + (f" ({kw_repr})" if kw_repr else "")

    lines = [title, "", "## summary", "```json",
             json.dumps(stats, indent=2, sort_keys=True), "```", "",
             "## first 32"]
    lines.append("```")
    lines.append(", ".join("None" if v is None else f"{v:.6g}" for v in head))
    lines.append("```")
    if tail:
        lines += ["", "## last 32", "```",
                  ", ".join("None" if v is None else f"{v:.6g}" for v in tail),
                  "```"]
    return "\n".join(lines)


# ── suggest_indicator ─────────────────────────────────────────────────


# Keyword → indicator suggestions. Order within a topic is the
# preferred recommendation order; the result emitter applies the same
# order across topics so the most-specific match shows up first.
INDICATOR_KEYWORDS: dict[str, dict[str, str]] = {
    "trend": {
        "SMA": "simple moving average; the canonical trend filter",
        "EMA": "exponential moving average; faster reaction than SMA",
        "KAMA": "Kaufman adaptive moving average; auto-tunes period to volatility",
        "DEMA": "double EMA; reduces lag at the cost of more noise",
        "TEMA": "triple EMA; further reduces lag",
        "RMA": "running moving average (Wilder smoothing); used inside RSI/ATR",
    },
    "volatility": {
        "ATR": "average true range; range-based volatility",
        "ParkinsonVol": "Parkinson volatility from highs/lows",
        "RogersSatchellVol": "Rogers-Satchell drift-independent volatility",
        "Bollinger": "Bollinger Bands; SMA ± k * stddev",
        "RollingZScore": "rolling z-score; how many sigma above/below the rolling mean",
    },
    "mean_reversion": {
        "Bollinger": "Bollinger Bands; classic SMA ± k*stddev fade signal",
        "RollingZScore": "rolling z-score; threshold-based mean reversion",
        "RSI": "RSI; overbought/oversold mean-reversion oscillator",
        "Stochastic": "stochastic oscillator; %K threshold mean reversion",
    },
    "oscillator": {
        "RSI": "relative strength index; classic momentum oscillator",
        "Stochastic": "stochastic oscillator; %K / %D",
        "MACD": "moving-average convergence divergence",
        "CCI": "commodity channel index",
    },
    "momentum": {
        "RSI": "relative strength index",
        "MACD": "moving-average convergence divergence",
        "Stochastic": "stochastic oscillator",
    },
    "volume": {
        "obv": "on-balance volume (function-style)",
        "vwap": "volume-weighted average price (function-style)",
        "cvd": "cumulative volume delta (function-style)",
    },
    "regime": {
        "adf": "augmented Dickey-Fuller (function-style); stationarity test",
        "autocorrelation": "rolling autocorrelation (function-style)",
        "Skewness": "rolling skewness",
        "Kurtosis": "rolling kurtosis",
    },
    "stationarity": {
        "adf": "augmented Dickey-Fuller stationarity test",
        "autocorrelation": "rolling autocorrelation",
    },
    "noise": {
        "ShannonEntropy": "rolling Shannon entropy of returns",
    },
    "channel": {
        "Bollinger": "Bollinger Bands",
        "atr": "ATR-based channel inputs",
    },
    "stddev": {
        "Bollinger": "Bollinger Bands; SMA ± k * stddev",
        "RollingZScore": "rolling z-score; price expressed in stddev units",
        "RollingStd": "rolling standard deviation",
    },
}

# Aliases mapping a user phrase to one or more topics.
_TOPIC_ALIASES: dict[str, Iterable[str]] = {
    "trend": ("trend",),
    "moving average": ("trend",),
    "ma": ("trend",),
    "smooth": ("trend",),
    "vol": ("volatility",),
    "volatility": ("volatility",),
    "vol-of-vol": ("volatility",),
    "oscillator": ("oscillator",),
    "momentum": ("momentum", "oscillator"),
    "overbought": ("oscillator",),
    "oversold": ("oscillator",),
    "volume": ("volume",),
    "regime": ("regime",),
    "stationary": ("stationarity",),
    "stationarity": ("stationarity",),
    "mean revert": ("mean_reversion", "oscillator", "stationarity"),
    "mean reversion": ("mean_reversion", "oscillator", "stationarity"),
    "revert to mean": ("mean_reversion",),
    "fade": ("mean_reversion",),
    "noise": ("noise",),
    "entropy": ("noise",),
    "channel": ("channel",),
    "band": ("channel", "stddev"),
    "bands": ("channel", "stddev"),
    "envelope": ("channel",),
    "standard deviation": ("stddev", "volatility"),
    "stddev": ("stddev", "volatility"),
    "std dev": ("stddev", "volatility"),
    "sigma": ("stddev", "volatility"),
    "z-score": ("stddev", "mean_reversion"),
    "zscore": ("stddev", "mean_reversion"),
    "z score": ("stddev", "mean_reversion"),
}


def suggest_indicator(description: str, *, k: int = 3) -> str:
    """Recommend FLOX indicators for an English description.

    Pure keyword heuristic — no model, no network. The caller's text
    is lowercased, scanned for known phrases, and the matching topic
    buckets pour their recommendations into a ranked list.
    """
    if not isinstance(description, str) or not description.strip():
        return "suggest_indicator: `description` must be a non-empty string."

    desc = description.lower()
    matched_topics: List[Tuple[str, str]] = []
    seen_topics: set = set()
    for phrase, topics in _TOPIC_ALIASES.items():
        if phrase in desc:
            for t in topics:
                if t not in seen_topics:
                    seen_topics.add(t)
                    matched_topics.append((phrase, t))

    if not matched_topics:
        return (
            f"# suggest_indicator: no keyword match for `{description!r}`\n\n"
            f"Try one of: trend, momentum, volatility, volume, regime, "
            f"stationarity, channel, noise. The matcher is keyword-based — "
            f"phrasing matters."
        )

    # Round-robin across matched topics: take one indicator per topic
    # per pass, then start a second pass for the next-best from each.
    # Without this, a query that matches several topics (e.g. "mean
    # reversion + standard deviations + moving average") would fill
    # the first k slots from whichever topic happens to be iterated
    # first, hiding the more specific suggestions from the other
    # matched topics.
    topics_in_order = [t for _, t in matched_topics]
    topic_iters: dict[str, list[Tuple[str, str]]] = {
        t: list(INDICATOR_KEYWORDS.get(t, {}).items()) for t in topics_in_order
    }
    suggestions: List[Tuple[str, str, str]] = []
    seen_names: set = set()
    while True:
        progressed = False
        for topic in topics_in_order:
            entries = topic_iters[topic]
            while entries:
                ind_name, rationale = entries.pop(0)
                if ind_name in seen_names:
                    continue
                seen_names.add(ind_name)
                suggestions.append((ind_name, topic, rationale))
                progressed = True
                break
        if not progressed:
            break

    top = suggestions[: max(1, int(k))]
    lines = [
        f"# suggest_indicator: top {len(top)} for `{description}`",
        "",
        f"_matched topics: {', '.join(t for _, t in matched_topics)}_",
        "",
        "| Indicator | Topic | Rationale |",
        "|---|---|---|",
    ]
    for name, topic, rationale in top:
        lines.append(f"| `{name}` | {topic} | {rationale} |")
    lines += [
        "",
        "Confirm shape and exact kwargs with `list_indicators` "
        "(class signatures) or `lookup_symbol(<name>)` "
        "(cross-binding lookup).",
    ]
    return "\n".join(lines)
