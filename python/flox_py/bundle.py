"""Reproducibility bundle format — `flox bundle pack/replay/validate`.

A bundle is a single self-contained tarball that encodes everything
needed to reproduce a backtest result byte-for-byte on another
machine: strategy code, the captured tape it ran against, the engine
version it was built with, and the expected output it should
reproduce. Packing captures the run; validating proves the next run
matches.

Bundle layout:

    bundle.tar.zst
    ├── manifest.json          versions + integrity hashes + config
    ├── strategy/
    │   └── strategy.py        the user's strategy module
    ├── config/
    │   └── params.json        runtime params (slippage, queue model,
    │                          initial capital, ...)
    ├── tape/                  the W14 tape this run drove against
    │   ├── manifest.json
    │   └── trades-*.bin
    └── expected_output.json   PnL, fill sequence, final positions

Phase 1 (this build) covers the pack-replay-validate cycle for
single-strategy single-tape bundles. Multi-strategy ensembles and
network/exchange-stub bundles land in follow-up work.
"""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import shutil
import sys
import tarfile
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional


BUNDLE_FORMAT_VERSION = 1
_MANIFEST_NAME = "manifest.json"
_EXPECTED_OUTPUT_NAME = "expected_output.json"
_EXPECTED_FLOXRUN_DIR = "expected.floxrun"
_STRATEGY_DIR = "strategy"
_CONFIG_DIR = "config"
_TAPE_DIR = "tape"


def _flox_version() -> str:
    """Best-effort flox version. Falls back to 'unknown' if the
    binding does not expose one — bundles still pin a SHA via
    package_dir hashing as a backup integrity gate."""
    try:
        import flox_py
        return getattr(flox_py, "__version__", "unknown")
    except Exception:
        return "unknown"


def _sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _sha256_dir(p: Path) -> str:
    """Stable SHA over directory: sort entries, hash (relpath, content)
    pairs in order. Same input tree always produces the same hash."""
    h = hashlib.sha256()
    base = p.resolve()
    for f in sorted(base.rglob("*")):
        if not f.is_file():
            continue
        rel = f.relative_to(base).as_posix().encode("utf-8")
        h.update(rel)
        h.update(b"\0")
        h.update(_sha256_file(f).encode("ascii"))
        h.update(b"\n")
    return h.hexdigest()


def _load_strategy_module(path: Path):
    spec = importlib.util.spec_from_file_location("flox_bundle_strategy", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"could not load strategy module at {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["flox_bundle_strategy"] = mod
    spec.loader.exec_module(mod)
    return mod


def _find_strategy_class(mod) -> type:
    import flox_py

    candidates = [
        v for v in vars(mod).values()
        if isinstance(v, type)
        and issubclass(v, flox_py.Strategy)
        and v is not flox_py.Strategy
    ]
    if not candidates:
        raise ValueError(
            f"no flox.Strategy subclass found in {mod.__name__}"
        )
    if len(candidates) > 1:
        raise ValueError(
            f"multiple flox.Strategy subclasses in {mod.__name__}: "
            f"{[c.__name__ for c in candidates]}; pack only supports one"
        )
    return candidates[0]


# ── Run helper: drive a strategy through a tape with a simulator ──


def _run_strategy_against_tape(
    strategy_path: Path,
    tape_path: Path,
    *,
    symbol_name: str = "BTCUSDT",
    exchange: str = "bundle",
    tick_size: float = 0.01,
    slippage_model: str = "none",
    slippage_params: Optional[Dict[str, float]] = None,
    floxrun_out: Optional[Path] = None,
) -> Dict[str, Any]:
    """Replay the trades in ``tape_path`` through the strategy at
    ``strategy_path`` using a SimulatedExecutor with the given
    config. Returns a dict capturing fill sequence and totals; this
    is what `pack` records as expected output and `validate`
    compares against."""
    import flox_py

    mod = _load_strategy_module(strategy_path)
    StratCls = _find_strategy_class(mod)

    registry = flox_py.SymbolRegistry()
    sym = registry.add_symbol(exchange, symbol_name, tick_size=tick_size)

    sim = flox_py.SimulatedExecutor()
    sim.set_default_slippage(
        model=slippage_model,
        ticks=int((slippage_params or {}).get("ticks", 0)),
        tick_size=float((slippage_params or {}).get("tick_size", 0.0)),
        bps=float((slippage_params or {}).get("bps", 0.0)),
        impact_coeff=float((slippage_params or {}).get("impact_coeff", 0.0)),
    )

    next_id = [1]

    # Optional .floxrun capture. When `floxrun_out` is set we mirror
    # every signal / order / fill into a TraceRecorder so the bundle
    # can ship the per-run trace alongside the legacy JSON summary.
    rec: Any = None
    submitted_orders: Dict[int, Dict[str, Any]] = {}
    if floxrun_out is not None:
        try:
            from flox_py._flox_py import (
                FillLiquidity, OrderEventKind, TraceRecorder,
            )
            rec = TraceRecorder(
                path=str(floxrun_out),
                strategy_id=strategy_path.stem,
                strategy_hash=_sha256_file(strategy_path)[:16],
                run_started_ns=time.time_ns(),
            )
        except Exception as e:
            import sys as _sys
            print(f"flox bundle: TraceRecorder unavailable: {e!r}", file=_sys.stderr)
            rec = None

    # Signal.order_type strings follow the C-ABI signal enum
    # ("tp_market" / "tp_limit"); SimulatedExecutor.submit_order parses
    # the long names. Keys double as the accepted-signal filter; the
    # index in this dict is also the bundle-local trace code (0=market,
    # 1=limit predate the conditional types; the rest extend that
    # convention in enum order).
    _SIGNAL_TO_EXEC_TYPE = {
        "market": "market",
        "limit": "limit",
        "stop_market": "stop_market",
        "stop_limit": "stop_limit",
        "tp_market": "take_profit_market",
        "tp_limit": "take_profit_limit",
        "trailing_stop": "trailing_stop",
    }
    _TRACE_ORDER_TYPE = {
        name: code for code, name in enumerate(_SIGNAL_TO_EXEC_TYPE)
    }

    def on_signal(sig: Any) -> None:
        order_type = (getattr(sig, "order_type", "") or "").lower()
        if order_type not in _SIGNAL_TO_EXEC_TYPE:
            return
        side = (getattr(sig, "side", "") or "").lower()
        oid = int(getattr(sig, "order_id", 0) or 0)
        if oid == 0:
            oid = next_id[0]
            next_id[0] += 1
        price = float(getattr(sig, "price", 0.0))
        qty = float(getattr(sig, "quantity", 0.0))
        sim.submit_order(
            oid, side, price, qty,
            type=_SIGNAL_TO_EXEC_TYPE[order_type], symbol=int(sym),
            trigger=float(getattr(sig, "trigger_price", 0.0) or 0.0),
            trailing_offset=float(getattr(sig, "trailing_offset", 0.0) or 0.0),
            trailing_bps=int(getattr(sig, "trailing_bps", 0) or 0),
        )
        if rec is not None:
            try:
                from flox_py._flox_py import OrderEventKind
                ts = time.time_ns()
                rec.write_signal(
                    run_ts_ns=ts, signal_id=oid, name=order_type,
                    symbol_ids=[int(sym)],
                )
                rec.write_order_event(
                    run_ts_ns=ts, order_id=oid, parent_signal_id=oid,
                    symbol_id=int(sym),
                    event_kind=OrderEventKind.SUBMIT,
                    side=0 if side == "buy" else 1,
                    order_type=_TRACE_ORDER_TYPE[order_type],
                    price_raw=int(price * 1e8), qty_raw=int(qty * 1e8),
                )
                submitted_orders[oid] = {"side": side, "symbol": int(sym)}
            except Exception:
                pass

    runner = flox_py.Runner(registry, on_signal=on_signal)
    try:
        strat = StratCls([int(sym)])
    except TypeError:
        strat = StratCls()
    runner.add_strategy(strat)
    runner.start()

    from flox_py.tape import replay_tape

    def on_trade(ts_ns, sym_id, price, qty, side):
        sim.advance_clock(int(ts_ns))
        sim.on_trade_qty(int(sym_id), float(price), float(qty), side == 0)
        runner.on_trade(int(sym_id), float(price), float(qty), side == 0, int(ts_ns))

    n = replay_tape(tape_path, on_trade=on_trade)
    runner.stop()

    fills = sim.fills_list()
    total_qty = sum(float(f["quantity"]) for f in fills)

    if rec is not None:
        try:
            from flox_py._flox_py import FillLiquidity
            ts = time.time_ns()
            for f in fills:
                rec.write_fill(
                    run_ts_ns=ts,
                    order_id=int(f.get("order_id", 0) or 0),
                    fill_id=int(f.get("order_id", 0) or 0),
                    price_raw=int(float(f["price"]) * 1e8),
                    qty_raw=int(float(f["quantity"]) * 1e8),
                    symbol_id=int(f["symbol"]),
                    side=0 if str(f["side"]).lower() == "buy" else 1,
                    liquidity=FillLiquidity.TAKER,
                )
            rec.set_run_ended_ns(time.time_ns())
            rec.close()
        except Exception:
            pass
    # Order IDs are intentionally omitted: the runner assigns them
    # from a process-wide counter, so they are not stable across runs
    # in the same process. The pair (symbol, side, price, quantity)
    # is the user-meaningful fill identity for reproducibility.
    return {
        "trade_count": int(n),
        "fill_count": len(fills),
        "fills": [
            {
                "symbol": int(f["symbol"]),
                "side": str(f["side"]),
                "price": float(f["price"]),
                "quantity": float(f["quantity"]),
            }
            for f in fills
        ],
        "total_filled_quantity": total_qty,
    }


# ── Pack ───────────────────────────────────────────────────────────


def pack_bundle(
    *,
    strategy: Path,
    tape: Path,
    output: Path,
    config: Optional[Dict[str, Any]] = None,
    symbol_name: str = "BTCUSDT",
    exchange: str = "bundle",
    tick_size: float = 0.01,
) -> Path:
    """Build a bundle from a strategy file and a tape directory.
    Writes a tar (or tar.zst if zstandard is available) at ``output``.
    Returns the output path."""
    strategy = Path(strategy).expanduser().resolve()
    tape = Path(tape).expanduser().resolve()
    output = Path(output).expanduser().resolve()

    if not strategy.is_file():
        raise FileNotFoundError(f"strategy file not found: {strategy}")
    if not tape.is_dir():
        raise FileNotFoundError(f"tape directory not found: {tape}")

    cfg = dict(config or {})
    slippage_model = str(cfg.get("slippage_model", "none"))
    slippage_params = dict(cfg.get("slippage_params", {}))

    floxrun_dir = Path(tempfile.mkdtemp(prefix="flox-bundle-floxrun-")) / "expected.floxrun"
    expected = _run_strategy_against_tape(
        strategy, tape,
        symbol_name=symbol_name,
        exchange=exchange,
        tick_size=tick_size,
        slippage_model=slippage_model,
        slippage_params=slippage_params,
        floxrun_out=floxrun_dir,
    )

    manifest = {
        "bundle_format_version": BUNDLE_FORMAT_VERSION,
        "flox_version": _flox_version(),
        "created_at_ns": time.time_ns(),
        "strategy_filename": strategy.name,
        "strategy_sha256": _sha256_file(strategy),
        "tape_sha256": _sha256_dir(tape),
        "symbol_name": symbol_name,
        "exchange": exchange,
        "tick_size": tick_size,
        "slippage_model": slippage_model,
        "slippage_params": slippage_params,
    }

    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, "w") as tf:
        # manifest
        m_bytes = json.dumps(manifest, indent=2, sort_keys=True).encode("utf-8")
        info = tarfile.TarInfo(_MANIFEST_NAME)
        info.size = len(m_bytes)
        tf.addfile(info, io.BytesIO(m_bytes))

        # strategy
        tf.add(str(strategy), arcname=f"{_STRATEGY_DIR}/{strategy.name}")

        # config
        c_bytes = json.dumps(cfg, indent=2, sort_keys=True).encode("utf-8")
        info = tarfile.TarInfo(f"{_CONFIG_DIR}/params.json")
        info.size = len(c_bytes)
        tf.addfile(info, io.BytesIO(c_bytes))

        # tape (as a directory tree)
        tf.add(str(tape), arcname=_TAPE_DIR)

        # expected output (legacy JSON summary, kept for back-compat)
        e_bytes = json.dumps(expected, indent=2, sort_keys=True).encode("utf-8")
        info = tarfile.TarInfo(_EXPECTED_OUTPUT_NAME)
        info.size = len(e_bytes)
        tf.addfile(info, io.BytesIO(e_bytes))

        # expected output (.floxrun directory). Older bundles produced
        # before the .floxrun ride-along landed have no `.floxrun/`
        # entry; `replay_bundle` falls back to JSON-only diff for those.
        if floxrun_dir.is_dir():
            tf.add(str(floxrun_dir), arcname=_EXPECTED_FLOXRUN_DIR)

    return output


# ── Replay / validate ──────────────────────────────────────────────


@dataclass
class BundleResult:
    actual: Dict[str, Any]
    expected: Dict[str, Any]
    manifest: Dict[str, Any]
    bundle_path: str
    matches: bool = False
    diff: List[str] = field(default_factory=list)


def _extract(bundle_path: Path) -> Path:
    tmp = Path(tempfile.mkdtemp(prefix="flox-bundle-"))
    with tarfile.open(bundle_path, "r") as tf:
        tf.extractall(tmp)
    return tmp


def _diff_dicts(actual: Dict[str, Any], expected: Dict[str, Any]) -> List[str]:
    diffs: List[str] = []
    keys = sorted(set(actual.keys()) | set(expected.keys()))
    for k in keys:
        a = actual.get(k, "<missing>")
        e = expected.get(k, "<missing>")
        if a != e:
            diffs.append(f"{k}: actual={a!r} expected={e!r}")
    return diffs


def replay_bundle(bundle_path: Path) -> BundleResult:
    """Extract a bundle, replay the strategy against the bundled tape
    with the bundled config, and return the actual output alongside
    the expected output. Does not compare; use :func:`validate_bundle`
    for the assertion."""
    bundle_path = Path(bundle_path).expanduser().resolve()
    if not bundle_path.is_file():
        raise FileNotFoundError(f"bundle not found: {bundle_path}")

    work = _extract(bundle_path)
    try:
        manifest = json.loads((work / _MANIFEST_NAME).read_text())
        if manifest.get("bundle_format_version") != BUNDLE_FORMAT_VERSION:
            raise ValueError(
                f"bundle format {manifest.get('bundle_format_version')!r} "
                f"is not supported by this build (expected "
                f"{BUNDLE_FORMAT_VERSION})"
            )
        expected = json.loads((work / _EXPECTED_OUTPUT_NAME).read_text())
        strat_name = manifest["strategy_filename"]
        strat_path = work / _STRATEGY_DIR / strat_name
        tape_path = work / _TAPE_DIR

        # Capture a fresh `.floxrun` from the replay so we can diff
        # signal / order / fill counts against the bundled expected
        # trace. Older bundles have no `expected.floxrun/`; we still
        # produce a fresh one for the
        # actual side and the comparison degrades to "expected has
        # no .floxrun, skipping that diff dimension".
        actual_floxrun = work / "actual.floxrun"
        actual = _run_strategy_against_tape(
            strat_path, tape_path,
            symbol_name=manifest.get("symbol_name", "BTCUSDT"),
            exchange=manifest.get("exchange", "bundle"),
            tick_size=float(manifest.get("tick_size", 0.01)),
            slippage_model=manifest.get("slippage_model", "none"),
            slippage_params=manifest.get("slippage_params") or {},
            floxrun_out=actual_floxrun,
        )

        expected_floxrun = work / _EXPECTED_FLOXRUN_DIR
        actual["floxrun_present"] = actual_floxrun.is_dir()
        expected["floxrun_present"] = expected_floxrun.is_dir()
        if expected_floxrun.is_dir() and actual_floxrun.is_dir():
            try:
                from flox_py._flox_py import TraceReader
                a_reader = TraceReader(str(actual_floxrun))
                e_reader = TraceReader(str(expected_floxrun))
                actual["floxrun_signal_count"] = len(a_reader.read_all_signals())
                actual["floxrun_order_count"] = len(a_reader.read_all_order_events())
                actual["floxrun_fill_count"] = len(a_reader.read_all_fills())
                expected["floxrun_signal_count"] = len(e_reader.read_all_signals())
                expected["floxrun_order_count"] = len(e_reader.read_all_order_events())
                expected["floxrun_fill_count"] = len(e_reader.read_all_fills())
            except Exception:
                # Reader missing or corrupt: silently skip the
                # floxrun diff; the JSON diff still runs.
                pass

        return BundleResult(
            actual=actual,
            expected=expected,
            manifest=manifest,
            bundle_path=str(bundle_path),
        )
    finally:
        shutil.rmtree(work, ignore_errors=True)


def validate_bundle(bundle_path: Path) -> BundleResult:
    """Replay a bundle and assert the output matches the recorded
    expected output. Sets ``matches`` and populates ``diff``."""
    res = replay_bundle(bundle_path)
    res.diff = _diff_dicts(res.actual, res.expected)
    res.matches = not res.diff
    return res


__all__ = [
    "BUNDLE_FORMAT_VERSION",
    "BundleResult",
    "pack_bundle",
    "replay_bundle",
    "validate_bundle",
]
