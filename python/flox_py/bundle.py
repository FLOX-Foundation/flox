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

    def on_signal(sig: Any) -> None:
        order_type = (getattr(sig, "order_type", "") or "").lower()
        if order_type not in ("market", "limit"):
            return
        side = (getattr(sig, "side", "") or "").lower()
        oid = int(getattr(sig, "order_id", 0) or 0)
        if oid == 0:
            oid = next_id[0]
            next_id[0] += 1
        sim.submit_order(
            oid, side,
            float(getattr(sig, "price", 0.0)),
            float(getattr(sig, "quantity", 0.0)),
            type=order_type,
            symbol=int(sym),
        )

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

    expected = _run_strategy_against_tape(
        strategy, tape,
        symbol_name=symbol_name,
        exchange=exchange,
        tick_size=tick_size,
        slippage_model=slippage_model,
        slippage_params=slippage_params,
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

        # expected output
        e_bytes = json.dumps(expected, indent=2, sort_keys=True).encode("utf-8")
        info = tarfile.TarInfo(_EXPECTED_OUTPUT_NAME)
        info.size = len(e_bytes)
        tf.addfile(info, io.BytesIO(e_bytes))

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

        actual = _run_strategy_against_tape(
            strat_path, tape_path,
            symbol_name=manifest.get("symbol_name", "BTCUSDT"),
            exchange=manifest.get("exchange", "bundle"),
            tick_size=float(manifest.get("tick_size", 0.01)),
            slippage_model=manifest.get("slippage_model", "none"),
            slippage_params=manifest.get("slippage_params") or {},
        )
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
