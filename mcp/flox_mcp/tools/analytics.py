"""MCP analytics tools — Phase 2 read-only introspection of a running
flox engine.

These tools speak HTTP to the local ``ControlServer`` the user app
embeds, the same one Phase 2 mutating tools use. All endpoints in
this module are read-only; they accept ``read``, ``paper``, and
``live`` scopes alike.

Use cases:

* "Why did strategy X go short here?" → ``explain_decision``
* "What are the indicator values right now?" → ``get_indicator_values``
* "What strategies are running?" → ``list_strategies``
* "Replay the last hour with EMA period 50 instead of 21" → ``whatif``
"""
from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from typing import Any, Mapping, Optional


_DEFAULT_URL = "http://127.0.0.1:8765"


def _env(name: str, default: Optional[str] = None) -> Optional[str]:
    val = os.environ.get(name)
    if val is None or val == "":
        return default
    return val


def _post(path: str, body: Mapping[str, Any]) -> str:
    url = _env("FLOX_CONTROL_URL", _DEFAULT_URL)
    token = _env("FLOX_CONTROL_TOKEN")
    if not token:
        return json.dumps({
            "error": "FLOX_CONTROL_TOKEN is not set; analytics tools "
                     "cannot reach the control plane.",
        }, indent=2)
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {token}",
    }
    data = json.dumps(dict(body)).encode("utf-8")
    req = urllib.request.Request(
        url=url + path, data=data, method="POST", headers=headers,
    )
    try:
        with urllib.request.urlopen(req, timeout=10.0) as resp:
            return resp.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        try:
            payload = json.loads(exc.read().decode("utf-8"))
        except Exception:
            payload = {"error": f"http {exc.code}"}
        return json.dumps(payload, indent=2)
    except urllib.error.URLError as exc:
        return json.dumps({
            "error": f"control plane unreachable at {url}: {exc.reason!r}.",
        }, indent=2)


def list_strategies() -> str:
    return _post("/list_strategies", {})


def get_strategy_state(*, name: str) -> str:
    return _post("/get_strategy_state", {"name": name})


def get_indicator_values(*, strategy: str, name: Optional[str] = None) -> str:
    body: dict[str, Any] = {"strategy": strategy}
    if name is not None:
        body["name"] = name
    return _post("/get_indicator_values", body)


def get_event_log(
    *,
    strategy: Optional[str] = None,
    type: Optional[str] = None,  # noqa: A002
    from_ts_ns: Optional[int] = None,
    to_ts_ns: Optional[int] = None,
    limit: int = 100,
) -> str:
    body: dict[str, Any] = {"limit": int(limit)}
    if strategy is not None:
        body["strategy"] = strategy
    if type is not None:
        body["type"] = type
    if from_ts_ns is not None:
        body["from_ts_ns"] = int(from_ts_ns)
    if to_ts_ns is not None:
        body["to_ts_ns"] = int(to_ts_ns)
    return _post("/get_event_log", body)


def explain_decision(*, event_id: str, max_depth: int = 32) -> str:
    return _post("/explain_decision", {
        "event_id": event_id,
        "max_depth": int(max_depth),
    })


def replay_window(
    *,
    from_ts_ns: Optional[int] = None,
    to_ts_ns: Optional[int] = None,
    strategy: Optional[str] = None,
    param_overrides: Optional[Mapping[str, Any]] = None,
) -> str:
    body: dict[str, Any] = {}
    if from_ts_ns is not None:
        body["from_ts_ns"] = int(from_ts_ns)
    if to_ts_ns is not None:
        body["to_ts_ns"] = int(to_ts_ns)
    if strategy is not None:
        body["strategy"] = strategy
    if param_overrides is not None:
        body["param_overrides"] = dict(param_overrides)
    return _post("/replay_window", body)


def whatif(
    *,
    strategy: str,
    param_overrides: Mapping[str, Any],
    from_ts_ns: Optional[int] = None,
    to_ts_ns: Optional[int] = None,
) -> str:
    body: dict[str, Any] = {
        "strategy": strategy,
        "param_overrides": dict(param_overrides),
    }
    if from_ts_ns is not None:
        body["from_ts_ns"] = int(from_ts_ns)
    if to_ts_ns is not None:
        body["to_ts_ns"] = int(to_ts_ns)
    return _post("/whatif", body)


__all__ = [
    "list_strategies",
    "get_strategy_state",
    "get_indicator_values",
    "get_event_log",
    "explain_decision",
    "replay_window",
    "whatif",
]
