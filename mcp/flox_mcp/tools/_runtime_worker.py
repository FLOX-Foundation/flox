"""Subprocess worker invoked by ``run_backtest`` in
``mcp/flox_mcp/tools/runtime.py``.

Lives in its own file (and is invoked by absolute path) so the parent
MCP process can cap CPU / RSS / output-size before user-supplied
strategy code starts running. POSIX rlimits are applied here, before
the user code is exec'd; on platforms without ``resource`` we run
without them and rely on the wall-clock timeout from the parent.

Output contract: writes a single JSON object to ``--out``:

    {
      "status": "ok" | "error",
      "stats": {...} | null,
      "error": str | null,
      "stdout": str
    }

The parent reads this file regardless of exit code so it can surface
a structured error even when a non-zero exit happened (segfault,
OOM, etc.).
"""
from __future__ import annotations

import argparse
import io
import json
import os
import sys
import traceback
from contextlib import redirect_stdout
from pathlib import Path


def _apply_rlimits(cpu_seconds: int, rss_bytes: int, fsize_bytes: int) -> None:
    try:
        import resource  # POSIX-only
    except ImportError:
        return  # Windows: parent's wall-clock timeout is the only guard.

    try:
        resource.setrlimit(resource.RLIMIT_CPU, (cpu_seconds, cpu_seconds))
    except (ValueError, OSError):
        pass
    # RLIMIT_AS bounds total virtual memory; macOS does not always
    # honour it (kernel returns OK but doesn't enforce). RLIMIT_RSS
    # is Linux-only. Try AS first, fall back silently.
    for limit_name in ("RLIMIT_AS", "RLIMIT_DATA"):
        if hasattr(resource, limit_name):
            try:
                resource.setrlimit(getattr(resource, limit_name),
                                    (rss_bytes, rss_bytes))
                break
            except (ValueError, OSError):
                continue
    try:
        resource.setrlimit(resource.RLIMIT_FSIZE, (fsize_bytes, fsize_bytes))
    except (ValueError, OSError):
        pass


def _write_result(path: Path, payload: dict) -> None:
    try:
        path.write_text(json.dumps(payload))
    except OSError:
        # If even writing the JSON exceeds the FSIZE rlimit, fall back
        # to stdout so the parent's structured error covers it.
        sys.stderr.write("worker: failed to write result file\n")


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--code", required=True)
    p.add_argument("--dataset", required=True)
    p.add_argument("--symbol", default="BTCUSDT")
    p.add_argument("--cpu-seconds", type=int, default=60)
    p.add_argument("--rss-bytes", type=int, default=1 * 1024 * 1024 * 1024)
    p.add_argument("--fsize-bytes", type=int, default=64 * 1024 * 1024)
    p.add_argument("--out", required=True)
    args = p.parse_args()

    out_path = Path(args.out)
    captured = io.StringIO()

    _apply_rlimits(args.cpu_seconds, args.rss_bytes, args.fsize_bytes)

    try:
        import flox_py as flox  # type: ignore
    except Exception as exc:
        _write_result(out_path, {
            "status": "error",
            "stats": None,
            "error": (f"flox_py import failed: {type(exc).__name__}: {exc}. "
                      f"run_backtest needs flox-py installed in the worker "
                      f"environment; install with `pip install flox-py`."),
            "stdout": "",
        })
        return 1

    code_text = Path(args.code).read_text()
    namespace: dict = {"flox_py": flox, "flox": flox, "__name__": "_strategy_module"}

    try:
        with redirect_stdout(captured):
            exec(compile(code_text, "<strategy>", "exec"), namespace)
    except Exception as exc:
        _write_result(out_path, {
            "status": "error",
            "stats": None,
            "error": (f"strategy import raised "
                      f"{type(exc).__name__}: {exc}\n"
                      f"{traceback.format_exc()}"),
            "stdout": captured.getvalue(),
        })
        return 1

    # Pick the user's Strategy subclass: prefer one explicitly assigned
    # to a top-level name STRATEGY (or strategy), otherwise scan for a
    # subclass of flox.Strategy.
    strat_cls = (namespace.get("STRATEGY")
                 or namespace.get("strategy")
                 or namespace.get("Strategy"))
    if strat_cls is None:
        Strategy = getattr(flox, "Strategy", None)
        if Strategy is not None:
            for value in namespace.values():
                if isinstance(value, type) and issubclass(value, Strategy) \
                        and value is not Strategy:
                    strat_cls = value
                    break

    if strat_cls is None:
        _write_result(out_path, {
            "status": "error",
            "stats": None,
            "error": ("strategy code did not define a Strategy subclass. "
                      "Either subclass `flox.Strategy` or assign "
                      "`STRATEGY = MyStrategy` at module level."),
            "stdout": captured.getvalue(),
        })
        return 1

    try:
        with redirect_stdout(captured):
            registry = flox.SymbolRegistry()
            sym = registry.add_symbol("exchange", args.symbol, tick_size=0.01)
            bt = flox.BacktestRunner(registry, fee_rate=0.0004,
                                     initial_capital=10_000)
            bt.set_strategy(strat_cls([sym]))
            stats = bt.run_csv(args.dataset, symbol=args.symbol)
    except Exception as exc:
        _write_result(out_path, {
            "status": "error",
            "stats": None,
            "error": (f"backtest raised {type(exc).__name__}: {exc}\n"
                      f"{traceback.format_exc()}"),
            "stdout": captured.getvalue(),
        })
        return 1

    # Normalise stats — only retain JSON-serializable values.
    safe_stats: dict = {}
    for k, v in (stats or {}).items():
        if isinstance(v, (int, float, str, bool)) or v is None:
            safe_stats[k] = v
        else:
            safe_stats[k] = str(v)

    _write_result(out_path, {
        "status": "ok",
        "stats": safe_stats,
        "error": None,
        "stdout": captured.getvalue(),
    })
    return 0


if __name__ == "__main__":
    sys.exit(main())
