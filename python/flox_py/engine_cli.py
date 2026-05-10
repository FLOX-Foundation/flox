"""``flox engine sim`` — bootstrap a paper-trading engine in one command.

Replaces the 50-line ``engine.py`` users would otherwise hand-roll
to wire a strategy, ``SimulatedExecutor``, ``ControlServer``, and the
runtime-state writer that tier-4 MCP read tools poll.

Out of scope for v1:

- Live connector orchestration (use ``flox tape record`` to capture a
  tape first, then replay it through the sim).
- Process supervision; use a normal supervisor (systemd, tmux) if
  the engine should outlive a terminal.
- Real-money trading. ``sim`` is exclusively the simulator surface;
  a separate ``flox engine live`` would carry the bigger safety
  blast radius.
"""
from __future__ import annotations

import argparse
import json
import os
import secrets
import signal
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional


_DEFAULT_STATE_FILE = "$HOME/.flox/runtime.json"


# ── Strategy loading ─────────────────────────────────────────────────


def _load_strategy_class(path: Path):
    """Import a single ``flox_py.Strategy`` subclass from a .py file.

    Same convention ``flox bundle pack`` already uses — keeps the
    user-authoring surface consistent across the CLI.
    """
    import importlib.util
    import flox_py

    if not path.is_file():
        raise SystemExit(f"flox engine sim: strategy file not found: {path}")
    spec = importlib.util.spec_from_file_location("flox_engine_strategy", path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"flox engine sim: could not load {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["flox_engine_strategy"] = mod
    spec.loader.exec_module(mod)
    candidates = [
        v for v in vars(mod).values()
        if isinstance(v, type)
        and issubclass(v, flox_py.Strategy)
        and v is not flox_py.Strategy
    ]
    if not candidates:
        raise SystemExit(
            f"flox engine sim: {path} has no flox.Strategy subclass. "
            f"Define one as `class MyStrat(flox.Strategy): ...`"
        )
    if len(candidates) > 1:
        raise SystemExit(
            f"flox engine sim: {path} has multiple flox.Strategy subclasses "
            f"({[c.__name__ for c in candidates]}). engine sim runs one."
        )
    return candidates[0]


# ── Minimal kill-switch stub ────────────────────────────────────────


class _KillSwitch:
    """Tiny duck-typed kill-switch. ControlServer only needs ``set`` /
    ``state``; the production runtime uses a richer hook.

    The sim engine is paper-trading — flipping this here just gates
    the ControlServer's mutating endpoints, which is the operator
    behaviour we want to mirror locally.
    """

    def __init__(self) -> None:
        self._active = False
        self._reason: Optional[str] = None
        self._since_ns: Optional[int] = None

    def set(self, active: bool, reason: Optional[str] = None) -> None:
        self._active = bool(active)
        self._reason = reason if active else None
        self._since_ns = time.time_ns() if active else None

    def state(self) -> Dict[str, Any]:
        return {
            "active": self._active,
            "reason": self._reason,
            "since_ns": self._since_ns,
        }


# ── Runtime state writer ────────────────────────────────────────────


class _StateWriter:
    """Periodically dumps a runtime snapshot for tier-4 MCP read tools.

    The schema matches ``docs/reference/runtime-state-schema.md`` v1.
    For v1 of the sim engine, positions / pnl / open_orders are
    populated as empty arrays — the snapshot's value here is that it
    *exists with valid schema*, so the read tools (post-T033) return
    an actionable response instead of erroring. Deriving real
    positions/PnL from the SimulatedExecutor fill stream is a
    follow-up — tracked separately.
    """

    def __init__(self, path: Path, kill_switch: _KillSwitch,
                 strategy_name: str, symbol_id: int,
                 symbol_name: str, period_s: float = 1.0) -> None:
        self._path = path
        self._ks = kill_switch
        self._strategy_name = strategy_name
        self._symbol_id = symbol_id
        self._symbol_name = symbol_name
        self._period_s = period_s
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        # Serialize writes — the loop thread and explicit out-of-band
        # callers both run _write_once, and both producing tmp files
        # at once collides on the rename.
        self._lock = threading.Lock()

    def _snapshot(self) -> Dict[str, Any]:
        return {
            "schema_version": 1,
            "captured_at_ns": time.time_ns(),
            "kill_switch": self._ks.state(),
            "strategies": [{
                "name": self._strategy_name,
                "status": "running",
                "symbols": [self._symbol_id],
            }],
            "positions": [],
            "open_orders": [],
            "pnl": {
                "by_strategy": [{
                    "strategy": self._strategy_name,
                    "realized": 0.0,
                    "unrealized": 0.0,
                    "fees": 0.0,
                    "trades": 0,
                }],
                "total": {"realized": 0.0, "unrealized": 0.0, "fees": 0.0},
            },
        }

    def _write_once(self) -> None:
        with self._lock:
            self._path.parent.mkdir(parents=True, exist_ok=True)
            # Atomic-ish write so a tier-4 reader never sees a partial
            # JSON document mid-flush.
            tmp = self._path.with_suffix(self._path.suffix + ".tmp")
            tmp.write_text(json.dumps(self._snapshot(), indent=2, sort_keys=True))
            os.replace(tmp, self._path)

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                self._write_once()
            except Exception as exc:  # pragma: no cover
                print(f"flox engine sim: state writer error: {exc}",
                      file=sys.stderr)
            self._stop.wait(self._period_s)

    def start(self) -> None:
        # Write immediately so the file exists by the time the user
        # sees the banner — they may run an MCP query straight away.
        self._write_once()
        self._thread = threading.Thread(
            target=self._run, daemon=True, name="flox-state-writer",
        )
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)


# ── Banner ───────────────────────────────────────────────────────────


def _print_banner(strategy_name: str, strategy_path: Path,
                  symbol_name: str, tape_path: Path,
                  state_path: Path, control_url: str, token: str) -> None:
    print("flox engine sim")
    print(f"  strategy:   {strategy_path} ({strategy_name})")
    print(f"  symbol:     {symbol_name}")
    print(f"  source:     tape {tape_path}")
    print(f"  state file: {state_path}")
    print(f"  control:    {control_url}")
    print(f"  token:      {token}")
    print()
    print("To wire into MCP, run in another shell:")
    print()
    print(f"    flox-mcp init \\")
    print(f"        --engine-url {control_url} \\")
    print(f"        --token {token}")
    print()
    print("Press Ctrl-C to stop.")
    print()


# ── Tape feed ────────────────────────────────────────────────────────


def _feed_tape(runner, tape_path: Path) -> int:
    """Drive a Runner from a recorded ``.floxlog`` tape. Returns the
    number of trades dispatched."""
    from . import tape as tape_mod

    counter = [0]

    def _on_trade(ts_ns: int, sym_id: int, price: float,
                  qty: float, side: int) -> None:
        # side: 0 = buy, 1 = sell. Runner.on_trade wants is_buy=bool.
        runner.on_trade(sym_id, price, qty, side == 0, ts_ns)
        counter[0] += 1

    tape_mod.replay_tape(tape_path, on_trade=_on_trade)
    return counter[0]


# ── Command ──────────────────────────────────────────────────────────


def cmd_engine_sim(args: argparse.Namespace) -> int:
    import flox_py
    from . import control_server as cs_mod

    strategy_path = Path(args.strategy).expanduser().resolve()
    tape_path = Path(args.tape).expanduser().resolve()
    if not tape_path.exists():
        print(f"flox engine sim: tape not found: {tape_path}", file=sys.stderr)
        return 1

    state_path = Path(
        os.path.expandvars(args.state_file or _DEFAULT_STATE_FILE)
    ).expanduser()

    StratCls = _load_strategy_class(strategy_path)

    registry = flox_py.SymbolRegistry()
    sym_id = registry.add_symbol(
        args.exchange, args.symbol_name, tick_size=args.tick_size,
    )

    runner = flox_py.Runner(registry, on_signal=lambda s: None,
                            threaded=bool(args.threaded))
    sim = flox_py.SimulatedExecutor()
    runner.set_executor(sim)

    ks = _KillSwitch()
    runner.set_kill_switch(ks)

    strategy = StratCls([sym_id])
    runner.add_strategy(strategy)
    runner.start()

    # Token: explicit override > generated.  Generated tokens are
    # url-safe so they paste cleanly into shell commands without
    # quoting.
    token = args.token or secrets.token_urlsafe(16)
    server = cs_mod.ControlServer(
        tokens={token: "paper"},
        executor=sim,
        kill_switch=ks,
        runner=runner,
        strategies=lambda: [{
            "name": StratCls.__name__,
            "status": "running",
            "symbols": [sym_id],
        }],
        host=args.host,
        port=args.port,
    )
    server.start()

    writer = _StateWriter(
        path=state_path, kill_switch=ks,
        strategy_name=StratCls.__name__,
        symbol_id=sym_id, symbol_name=args.symbol_name,
    )
    writer.start()

    _print_banner(
        strategy_name=StratCls.__name__,
        strategy_path=strategy_path,
        symbol_name=args.symbol_name,
        tape_path=tape_path,
        state_path=state_path,
        control_url=server.url,
        token=token,
    )

    stop_evt = threading.Event()

    def _on_signal(_signum, _frame):
        stop_evt.set()

    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)

    try:
        n = _feed_tape(runner, tape_path)
        print(f"tape replay complete: {n} trade(s) dispatched.")
        print("engine still running — Ctrl-C to stop.")
        # Hold until SIGINT so the user can introspect via MCP after
        # the tape ends.
        stop_evt.wait()
    finally:
        print("\nstopping engine...")
        try:
            runner.stop()
        except Exception:
            pass
        server.stop()
        writer.stop()

    return 0


# ── Argparse wiring (called from flox_py.cli) ───────────────────────


def add_engine_subparser(sub: Any) -> None:
    """Attach the ``engine`` subcommand tree onto an existing argparse
    sub-parsers handle. Callable from ``flox_py.cli`` so the verb
    appears under ``flox engine ...`` next to ``flox tape ...``.
    """
    p = sub.add_parser(
        "engine",
        help="Run a flox engine — sim (paper-trading) is the only "
             "mode wired today.",
    )
    eng_sub = p.add_subparsers(dest="engine_command", required=True)

    p_sim = eng_sub.add_parser(
        "sim",
        help="Bootstrap a paper-trading engine: Runner + "
             "SimulatedExecutor + ControlServer + state writer.",
    )
    p_sim.add_argument(
        "--strategy", required=True,
        help="Path to a .py file with one flox.Strategy subclass.",
    )
    p_sim.add_argument(
        "--tape", required=True,
        help="Path to a .floxlog tape directory (the data source).",
    )
    p_sim.add_argument(
        "--symbol-name", default="BTCUSDT",
        help="Symbol name to register (default: BTCUSDT).",
    )
    p_sim.add_argument(
        "--exchange", default="sim",
        help="Exchange tag for the symbol (default: sim).",
    )
    p_sim.add_argument(
        "--tick-size", type=float, default=0.01,
        help="Tick size for the symbol (default: 0.01).",
    )
    p_sim.add_argument(
        "--port", type=int, default=8765,
        help="ControlServer port (default 8765).",
    )
    p_sim.add_argument(
        "--host", default="127.0.0.1",
        help="ControlServer bind host (default 127.0.0.1).",
    )
    p_sim.add_argument(
        "--token", default=None,
        help="Explicit ControlServer token. Default: generated.",
    )
    p_sim.add_argument(
        "--state-file", default=None,
        help=f"Runtime state file (default: {_DEFAULT_STATE_FILE}).",
    )
    p_sim.add_argument(
        "--threaded", action="store_true",
        help="Use the Disruptor (threaded) Runner. Default: sync.",
    )
    p_sim.set_defaults(handler=cmd_engine_sim)
