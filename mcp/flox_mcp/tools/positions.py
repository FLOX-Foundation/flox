"""Live state inspection tools — read-only positions / orders / PnL /
kill-switch over a runtime snapshot file.

This is W2-T015 Phase 1: read-only inspection. The MCP server reads
a JSON snapshot the user's flox app writes periodically. Mutating
operations (place_order, cancel, set_kill_switch) and a real-time IPC
transport are deferred to Phase 2.

The snapshot path comes from the ``FLOX_RUNTIME_STATE`` env var, or
falls back to the caller-supplied ``state_path`` argument. The schema
is documented in ``docs/reference/runtime-state-schema.md``.
"""
from __future__ import annotations

import json
import os
import time
from pathlib import Path
from typing import Any, Dict, List, Optional


_DEFAULT_STATE_PATH = "/tmp/flox-runtime-state.json"
_SUPPORTED_SCHEMA_VERSIONS = {1}


def _resolve_path(state_path: Optional[str]) -> Path:
    if state_path:
        return Path(state_path).expanduser()
    env = os.environ.get("FLOX_RUNTIME_STATE")
    if env:
        return Path(env).expanduser()
    return Path(_DEFAULT_STATE_PATH)


def _load_state(state_path: Optional[str]) -> Dict[str, Any]:
    p = _resolve_path(state_path)
    if not p.exists():
        raise FileNotFoundError(
            f"runtime state snapshot not found at {p}. "
            f"Set FLOX_RUNTIME_STATE or have your app write a snapshot. "
            f"See docs/reference/runtime-state-schema.md."
        )
    with p.open("r") as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise ValueError(f"runtime state at {p} is not a JSON object")
    version = data.get("schema_version")
    if version not in _SUPPORTED_SCHEMA_VERSIONS:
        raise ValueError(
            f"runtime state schema_version={version!r} at {p} is not "
            f"supported by this flox-mcp build (supported: "
            f"{sorted(_SUPPORTED_SCHEMA_VERSIONS)})"
        )
    return data


def _snapshot_age_ms(state: Dict[str, Any]) -> Optional[int]:
    captured_at_ns = state.get("captured_at_ns")
    if not isinstance(captured_at_ns, int) or captured_at_ns <= 0:
        return None
    now_ns = time.time_ns()
    return max(0, (now_ns - captured_at_ns) // 1_000_000)


def _format_payload(state: Dict[str, Any], data: Any) -> str:
    """Wrap returned data with snapshot metadata so the agent has
    visibility into staleness without parsing free-form text."""
    payload = {
        "snapshot_age_ms": _snapshot_age_ms(state),
        "data": data,
    }
    return json.dumps(payload, indent=2, sort_keys=True)


def _format_error(message: str) -> str:
    return json.dumps({"error": message}, indent=2)


# ── Tools ────────────────────────────────────────────────────────────


def get_positions(
    account: Optional[str] = None,
    strategy: Optional[str] = None,
    state_path: Optional[str] = None,
) -> str:
    """Return positions matching account / strategy filters.

    Filters are AND-ed. Both default to None (no filter). Returns a
    JSON object ``{"snapshot_age_ms": int|None, "data": [{...}, ...]}``.
    """
    try:
        state = _load_state(state_path)
    except (FileNotFoundError, ValueError) as exc:
        return _format_error(str(exc))
    rows = state.get("positions") or []
    if account is not None:
        rows = [r for r in rows if r.get("account") == account]
    if strategy is not None:
        rows = [r for r in rows if r.get("strategy") == strategy]
    return _format_payload(state, rows)


def get_open_orders(
    filter: Optional[str] = None,
    state_path: Optional[str] = None,
) -> str:
    """Return open orders. ``filter`` is a case-insensitive substring
    match against ``symbol_name`` or ``strategy``."""
    try:
        state = _load_state(state_path)
    except (FileNotFoundError, ValueError) as exc:
        return _format_error(str(exc))
    rows: List[Dict[str, Any]] = state.get("open_orders") or []
    if filter:
        needle = filter.lower()
        rows = [
            r for r in rows
            if needle in str(r.get("symbol_name", "")).lower()
            or needle in str(r.get("strategy", "")).lower()
        ]
    return _format_payload(state, rows)


def get_pnl(
    strategy: Optional[str] = None,
    state_path: Optional[str] = None,
) -> str:
    """Return PnL totals plus the per-strategy breakdown. ``strategy``
    filters the breakdown to one row; the ``total`` block always
    reflects the full snapshot.

    ``period`` is reserved for Phase 2 — the current snapshot is
    point-in-time, so windowed PnL would need a series of snapshots
    or a separate read path."""
    try:
        state = _load_state(state_path)
    except (FileNotFoundError, ValueError) as exc:
        return _format_error(str(exc))
    pnl = state.get("pnl") or {}
    by_strategy = pnl.get("by_strategy") or []
    if strategy is not None:
        by_strategy = [r for r in by_strategy if r.get("strategy") == strategy]
    return _format_payload(state, {
        "by_strategy": by_strategy,
        "total": pnl.get("total") or {},
    })


def get_kill_switch(state_path: Optional[str] = None) -> str:
    """Return the kill-switch state: ``{"active": bool, "reason":
    str|None, "since_ns": int|None}``."""
    try:
        state = _load_state(state_path)
    except (FileNotFoundError, ValueError) as exc:
        return _format_error(str(exc))
    ks = state.get("kill_switch") or {"active": False, "reason": None, "since_ns": None}
    return _format_payload(state, ks)


__all__ = [
    "get_positions",
    "get_open_orders",
    "get_pnl",
    "get_kill_switch",
]
