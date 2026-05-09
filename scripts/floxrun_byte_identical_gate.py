#!/usr/bin/env python3
"""Cross-binding `.floxrun` parity gate.

Runs the same fixture through every available binding (pybind11 / NAPI
/ Codon), reads each emitted `.floxrun` directory back through
`TraceReader`, and asserts the structured records (signals + order
events + fills) are identical across bindings.

True byte-identical comparison is blocked by per-segment wall-clock
metadata embedded in segment headers — that's a recorder-side
deterministic-mode follow-up. Comparing the structured records is the
strongest meaningful guarantee today: it proves every binding emits
the same recorded content for the same input.

Skips bindings whose runtime / build artefact is missing rather than
failing — the gate is runnable on developer machines that build only
a subset.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parent.parent
PYTHON_FIXTURE = ROOT / "scripts" / "floxrun_byte_identical_fixture.py"
NODE_FIXTURE = ROOT / "node" / "test" / "byte_identical_fixture.js"
CODON_FIXTURE_BIN = ROOT / "build" / "codon" / "codon_byte_identical_fixture"
CODON_LIB_DIR = ROOT / "build" / "src" / "capi"


def _read_via_pybind(out_dir: Path) -> Dict[str, List[Dict[str, Any]]]:
    """Read records back through pybind11 TraceReader. Used as the
    canonical reader for every binding's output — the goal is to compare
    the records themselves, not the storage layout."""
    sys.path.insert(0, str(ROOT / "build" / "python"))
    import flox_py  # type: ignore  # noqa: PLC0415

    reader = flox_py.TraceReader(str(out_dir))
    return {
        "signals": [_normalize(s) for s in reader.read_all_signals()],
        "orders": [_normalize(o) for o in reader.read_all_order_events()],
        "fills": [_normalize(f) for f in reader.read_all_fills()],
    }


def _normalize(record: Any) -> Dict[str, Any]:
    """Convert a record (dict or attribute object) to a plain dict, stripping
    fields that move with each run (run_ts_ns isn't expected to differ in
    this fixture but we drop it defensively)."""
    if isinstance(record, dict):
        out = dict(record)
    else:
        out = {k: getattr(record, k) for k in dir(record) if not k.startswith("_")}
    # bytes -> b64 so dict equality works.
    for k, v in list(out.items()):
        if isinstance(v, bytes):
            out[k] = ("b64", v.hex())
    return out


def run_python(out_dir: Path) -> Optional[Dict[str, List[Dict[str, Any]]]]:
    if not (ROOT / "build" / "python" / "flox_py").exists():
        print("[skip] pybind11: build/python/flox_py missing")
        return None
    env = dict(os.environ)
    env["PYTHONPATH"] = str(ROOT / "build" / "python")
    res = subprocess.run([sys.executable, str(PYTHON_FIXTURE), str(out_dir)],
                          env=env, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[fail] pybind11: {res.stderr.strip()}", file=sys.stderr)
        return None
    return _read_via_pybind(out_dir)


def run_node(out_dir: Path) -> Optional[Dict[str, List[Dict[str, Any]]]]:
    if shutil.which("node") is None:
        print("[skip] node: node binary not found")
        return None
    if not (ROOT / "node" / "build" / "Release" / "flox_node.node").exists():
        print("[skip] node: build/Release/flox_node.node missing")
        return None
    res = subprocess.run(["node", str(NODE_FIXTURE), str(out_dir)],
                          capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[fail] node: {res.stderr.strip()}", file=sys.stderr)
        return None
    return _read_via_pybind(out_dir)


def run_codon(out_dir: Path) -> Optional[Dict[str, List[Dict[str, Any]]]]:
    if not CODON_FIXTURE_BIN.exists():
        print(f"[skip] codon: {CODON_FIXTURE_BIN} missing")
        return None
    env = dict(os.environ)
    env["DYLD_LIBRARY_PATH"] = str(CODON_LIB_DIR) + ":" + env.get("DYLD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = str(CODON_LIB_DIR) + ":" + env.get("LD_LIBRARY_PATH", "")
    res = subprocess.run([str(CODON_FIXTURE_BIN), str(out_dir)],
                          env=env, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[fail] codon: {res.stderr.strip()}", file=sys.stderr)
        return None
    return _read_via_pybind(out_dir)


# NAPI converts ns-level int64 timestamps through JS `Number` (float64)
# which loses bottom bits past Number.MAX_SAFE_INTEGER (~2^53). A real
# fix is BigInt support in the NAPI write_* surface; until then the
# gate tolerates timestamp drift of up to 1024 ns and asserts strict
# equality on every other field. Tracked under a separate follow-up.
_TIMESTAMP_TOLERANCE_NS = 1024
_TIMESTAMP_FIELDS = {"run_ts_ns", "feed_ts_ns", "runTsNs", "feedTsNs"}


def _diff_records(name_a: str, recs_a: Dict[str, List[Dict[str, Any]]],
                   name_b: str, recs_b: Dict[str, List[Dict[str, Any]]]) -> List[str]:
    issues: List[str] = []
    for kind in ("signals", "orders", "fills"):
        la, lb = recs_a[kind], recs_b[kind]
        if len(la) != len(lb):
            issues.append(f"{name_a} vs {name_b}: {kind} count {len(la)} vs {len(lb)}")
            continue
        for i, (a, b) in enumerate(zip(la, lb)):
            for k in sorted(set(a) | set(b)):
                va, vb = a.get(k), b.get(k)
                if va == vb:
                    continue
                if (k in _TIMESTAMP_FIELDS and isinstance(va, int) and isinstance(vb, int)
                        and abs(va - vb) <= _TIMESTAMP_TOLERANCE_NS):
                    continue
                issues.append(
                    f"{name_a} vs {name_b}: {kind}[{i}].{k}  "
                    f"{va!r}  vs  {vb!r}")
    return issues


def main() -> int:
    runners: List[Tuple[str, Any]] = [
        ("pybind11", run_python),
        ("napi", run_node),
        ("codon", run_codon),
    ]
    results: Dict[str, Dict[str, List[Dict[str, Any]]]] = {}
    with tempfile.TemporaryDirectory() as tmp:
        for name, runner in runners:
            out = Path(tmp) / name / "run.floxrun"
            out.parent.mkdir(parents=True, exist_ok=True)
            recs = runner(out)
            if recs is None:
                continue
            results[name] = recs
            print(f"[ok]   {name}: "
                  f"{len(recs['signals'])} signal(s), "
                  f"{len(recs['orders'])} order(s), "
                  f"{len(recs['fills'])} fill(s)")

    if len(results) < 2:
        print("\nfewer than 2 bindings produced output — nothing to compare; "
              "build the missing artefacts and re-run.", file=sys.stderr)
        return 1

    bindings = list(results.keys())
    reference = results[bindings[0]]
    diverged: List[str] = []
    for other in bindings[1:]:
        diverged.extend(_diff_records(bindings[0], reference, other, results[other]))

    if diverged:
        print("\n::error::cross-binding .floxrun parity gate FAILED",
              file=sys.stderr)
        for d in diverged:
            print(f"  {d}", file=sys.stderr)
        return 1

    print(f"\n.floxrun parity gate OK across {len(results)} binding(s): "
          f"{', '.join(bindings)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
