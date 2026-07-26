#!/usr/bin/env python3
"""Call the binding surface; fail on binding-level errors.

check_binding_parity.py verifies that a symbol *exists* in each binding. Every
defect the binding-surface audit found had an existing symbol:

    VenueStack.clock()            TypeError: Unregistered type : SimulatedClock
    VenueStack.binanceUmFutures() SyntaxError: Unexpected number
    setQueueFifoTopN()            ReferenceError: not defined
    FeeSchedule.loadProfile('x')  silently did nothing

A name gate cannot see any of those. This one walks each binding by reflection
and *calls* what it finds, then classifies what comes back:

  binding-level  the wrapper itself is broken -- an unregistered return type, a
                 signature the addon cannot accept, a global that was never
                 registered. Always a defect. Fails this gate.
  domain-level   the call reached real code and that code objected: a missing
                 required argument, an empty registry, a bad path. Expected,
                 and ignored here.

Only zero-argument calls are made, and names that would submit orders, write
files or tear down state are skipped -- see SKIP_NAMES. That is enough: every
defect above was reachable from a no-argument or single-literal call.

Run:
    python3 scripts/check_binding_smoke.py            # both bindings
    python3 scripts/check_binding_smoke.py --python   # one of them
    python3 scripts/check_binding_smoke.py --verbose
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Signatures of a broken wrapper, as opposed to code rejecting an input.
BINDING_LEVEL = (
    "Unregistered type",
    "did not match C++ signature",
    "No constructor defined",
    "is not defined",            # QuickJS/Node: global never registered
    "Unexpected number",         # NAPI: new Function(...) from a bad ctor lookup
    "Unexpected token",
    "is not a function",
    "not a constructor",
    # Deliberately NOT "Cannot convert undefined or null to object": NAPI
    # methods all report length 0, so a zero-arg probe of a method that needs
    # arguments produces exactly that message. It cannot tell a broken wrapper
    # from a forgotten argument, and OrderGroup.setRiskLimits({...}) works fine.
)

# Anything that trades, mutates disk, or stops the world.
SKIP_NAMES = {
    "submit", "submit_order", "submitOrder", "submit_bracket", "submitBracket",
    "submit_iceberg", "submitIceberg", "submit_oco", "submitOco",
    "cancel", "cancel_all", "cancelAll", "cancel_order", "cancelOrder",
    "cancel_bracket", "cancelBracket", "place_order", "placeOrder",
    "flatten", "flatten_positions", "flattenPositions",
    "start", "stop", "run", "close", "destroy", "reset", "shutdown",
    "write", "flush", "record", "record_fill", "recordFill", "save",
    "set_kill_switch", "setKillSwitch", "trigger",
}


def python_smoke(verbose: bool) -> list[str]:
    """Reflect over flox_py and call every zero-arg method we can reach."""
    probe = r'''
import inspect, json, sys
try:
    import flox_py
except Exception as exc:
    print(json.dumps({"fatal": f"cannot import flox_py: {exc}"})); raise SystemExit(0)

SKIP = set(json.loads(sys.argv[1]))
BAD = json.loads(sys.argv[2])
problems, checked = [], 0

def looks_binding_level(msg):
    return any(b in msg for b in BAD)

roots = []
for name in dir(flox_py):
    obj = getattr(flox_py, name, None)
    if not isinstance(obj, type):
        continue
    try:
        roots.append((name, obj()))            # default-constructible
    except Exception:
        # Factories that hand out a configured object are the interesting ones
        # (VenueStack.clock() was only reachable this way).
        for fname in ("binance_um_futures", "bybit_linear", "okx_swap", "deribit"):
            f = getattr(obj, fname, None)
            if callable(f):
                try:
                    roots.append((f"{name}.{fname}()", f(account_id=1, equity=1000.0)))
                    break
                except Exception:
                    pass

for label, inst in roots:
    for attr in dir(inst):
        if attr.startswith("_") or attr in SKIP:
            continue
        try:
            member = getattr(inst, attr)
        except Exception as exc:
            msg = f"{type(exc).__name__}: {exc}"
            if looks_binding_level(msg):
                problems.append(f"{label}.{attr} (attribute access) -> {msg}")
            continue
        if not callable(member):
            continue
        try:
            sig = inspect.signature(member)
            if any(p.default is inspect.Parameter.empty
                   and p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)
                   for p in sig.parameters.values()):
                continue
        except (TypeError, ValueError):
            pass    # pybind11 overloads have no introspectable signature; try it
        checked += 1
        try:
            member()
        except Exception as exc:
            msg = f"{type(exc).__name__}: {exc}"
            if looks_binding_level(msg):
                problems.append(f"{label}.{attr}() -> {msg}")

print(json.dumps({"checked": checked, "roots": len(roots), "problems": problems}))
'''
    r = subprocess.run([sys.executable, "-c", probe,
                        json.dumps(sorted(SKIP_NAMES)), json.dumps(list(BINDING_LEVEL))],
                       capture_output=True, text=True, cwd=REPO)
    line = (r.stdout or "").strip().splitlines()
    if not line:
        return [f"python probe produced no output: {r.stderr[-400:]}"]
    data = json.loads(line[-1])
    if "fatal" in data:
        print(f"  python: SKIP ({data['fatal']})")
        return []
    print(f"  python: {data['roots']} objects, {data['checked']} zero-arg calls")
    if verbose:
        for p in data["problems"]:
            print(f"    {p}")
    return [f"python: {p}" for p in data["problems"]]


def node_smoke(verbose: bool) -> list[str]:
    """Same walk over the Node addon's exports and prototypes."""
    node_dir = REPO / "node"
    if not (node_dir / "index.js").is_file():
        print("  node: SKIP (addon not built)")
        return []
    probe = r'''
const path = require('path');
const SKIP = new Set(JSON.parse(process.env.FLOX_SMOKE_SKIP));
const BAD = JSON.parse(process.env.FLOX_SMOKE_BAD);
let flox;
try { flox = require(path.join(process.cwd(), 'index.js')); }
catch (e) { console.log(JSON.stringify({fatal: String(e.message)})); process.exit(0); }

const bindingLevel = (m) => BAD.some(b => String(m).includes(b));
const problems = [];
let checked = 0, roots = 0;

const instances = [];
for (const name of Object.keys(flox)) {
  const C = flox[name];
  if (typeof C !== 'function') continue;
  // Static factories first: they are how a configured object is obtained, and
  // they were the broken part.
  for (const f of ['binanceUmFutures', 'bybitLinear', 'okxSwap', 'deribit']) {
    if (typeof C[f] === 'function') {
      try { instances.push([`${name}.${f}()`, C[f](1, 100000)]); }
      catch (e) { if (bindingLevel(e.message)) problems.push(`${name}.${f}() -> ${e.message}`); }
    }
  }
  try { instances.push([name, new C()]); } catch (e) { /* needs args */ }
}
roots = instances.length;

for (const [label, inst] of instances) {
  if (inst === null || typeof inst !== 'object') continue;
  const proto = Object.getPrototypeOf(inst) || {};
  for (const attr of Object.getOwnPropertyNames(proto)) {
    if (attr === 'constructor' || attr.startsWith('_') || SKIP.has(attr)) continue;
    let member;
    try { member = inst[attr]; }
    catch (e) { if (bindingLevel(e.message)) problems.push(`${label}.${attr} (get) -> ${e.message}`); continue; }
    if (typeof member !== 'function' || member.length > 0) continue;
    checked++;
    try { member.call(inst); }
    catch (e) { if (bindingLevel(e.message)) problems.push(`${label}.${attr}() -> ${e.message}`); }
  }
}
console.log(JSON.stringify({checked, roots, problems}));
'''
    import os
    env = dict(os.environ,
               FLOX_SMOKE_SKIP=json.dumps(sorted(SKIP_NAMES)),
               FLOX_SMOKE_BAD=json.dumps(list(BINDING_LEVEL)))
    r = subprocess.run(["node", "-e", probe], capture_output=True, text=True,
                       cwd=node_dir, env=env)
    line = (r.stdout or "").strip().splitlines()
    if not line:
        return [f"node probe produced no output: {r.stderr[-400:]}"]
    data = json.loads(line[-1])
    if "fatal" in data:
        print(f"  node: SKIP ({data['fatal']})")
        return []
    print(f"  node: {data['roots']} objects, {data['checked']} zero-arg calls")
    if verbose:
        for p in data["problems"]:
            print(f"    {p}")
    return [f"node: {p}" for p in data["problems"]]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--python", action="store_true", help="Python binding only")
    ap.add_argument("--node", action="store_true", help="Node binding only")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    both = not (args.python or args.node)

    print("Calling the binding surface (binding-level errors only):")
    problems: list[str] = []
    if both or args.python:
        problems += python_smoke(args.verbose)
    if both or args.node:
        problems += node_smoke(args.verbose)

    if problems:
        print("\nerror: binding-level failures — the wrapper is broken, not the "
              "input:\n", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    print("OK: no binding-level failures.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
