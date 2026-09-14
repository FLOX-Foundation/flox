"""MCP control plane client — Phase 2 mutating tools.

These tools speak to a ``flox_py.control_server.ControlServer`` over
HTTP. The user's app embeds the server; the AI client launches
``flox-mcp`` as a child process; the child reads
``FLOX_CONTROL_URL`` plus ``FLOX_CONTROL_TOKEN`` from the
environment and proxies typed JSON requests.

Every tool returns the server's response verbatim, including the
``audit_id`` so the operator can correlate an MCP call with the
audit log line it produced. On a missing or unreachable control
server the tools surface a structured error rather than crashing
the MCP loop.

Every function here returns ``(text, is_error)``. ``is_error`` is
``False`` only when the control server accepted and processed the
call (a 2xx response, which itself may report ``accepted=false`` for
a `dry_run` or a rejected order inside ``text`` -- that is a
successful *query*, not a tool failure). Every other outcome --
no token configured, the control plane unreachable, or the server
returning an HTTP error status -- means the mutating action the
caller asked for did not happen, so ``is_error`` is ``True``. The
caller (``server.py``) surfaces this as the MCP ``isError`` flag on
the tool result; before this, every path here returned plain text
and a client branching on ``isError`` would see ``False`` even when
``place_order`` never reached the exchange.
"""
from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from typing import Any, Mapping, Optional, Tuple


_DEFAULT_URL = "http://127.0.0.1:8765"


def _env(name: str, default: Optional[str] = None) -> Optional[str]:
    val = os.environ.get(name)
    if val is None or val == "":
        return default
    return val


def _post(path: str, body: Mapping[str, Any]) -> Tuple[str, bool]:
    url = _env("FLOX_CONTROL_URL", _DEFAULT_URL)
    token = _env("FLOX_CONTROL_TOKEN")
    if not token:
        return json.dumps({
            "error": ("No flox engine detected. Mutating control tools "
                      "(place_order, cancel_order, cancel_all, "
                      "flatten_positions, set_kill_switch) need a running "
                      "engine reachable via FLOX_CONTROL_URL + "
                      "FLOX_CONTROL_TOKEN. If you are exploring without "
                      "an engine, use docs_search / lookup_symbol / "
                      "scaffold_strategy / run_backtest instead."),
        }, indent=2), True

    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {token}",
    }
    data = json.dumps(dict(body)).encode("utf-8")
    req = urllib.request.Request(
        url=url + path, data=data, method="POST", headers=headers,
    )
    try:
        with urllib.request.urlopen(req, timeout=5.0) as resp:
            return resp.read().decode("utf-8"), False
    except urllib.error.HTTPError as exc:
        try:
            payload = json.loads(exc.read().decode("utf-8"))
        except Exception:
            payload = {"error": f"http {exc.code}"}
        return json.dumps(payload, indent=2), True
    except urllib.error.URLError as exc:
        return json.dumps({
            "error": f"control plane unreachable at {url}: {exc.reason!r}. "
                     "Is the user app running with ControlServer.start()?",
        }, indent=2), True


def place_order(
    *,
    account: str,
    symbol: int,
    side: str,
    qty: float,
    type: str = "market",
    price: float = 0.0,
    reason: str = "",
    dry_run: bool = True,
    approve_token: Optional[str] = None,
) -> Tuple[str, bool]:
    body: dict[str, Any] = {
        "account": account,
        "symbol": int(symbol),
        "side": side,
        "qty": float(qty),
        "type": type,
        "price": float(price),
        "reason": reason,
        "dry_run": bool(dry_run),
    }
    if approve_token:
        body["approve_token"] = approve_token
    return _post("/place_order", body)


def cancel_order(*, account: str, order_id: int, dry_run: bool = True) -> Tuple[str, bool]:
    return _post("/cancel_order", {
        "account": account,
        "order_id": int(order_id),
        "dry_run": bool(dry_run),
    })


def cancel_all(*, account: str, symbol: int = 0, dry_run: bool = True) -> Tuple[str, bool]:
    return _post("/cancel_all", {
        "account": account,
        "symbol": int(symbol),
        "dry_run": bool(dry_run),
    })


def flatten_positions(
    *, account: str, symbol: Optional[int] = None, dry_run: bool = True
) -> Tuple[str, bool]:
    body: dict[str, Any] = {"account": account, "dry_run": bool(dry_run)}
    if symbol is not None:
        body["symbol"] = int(symbol)
    return _post("/flatten_positions", body)


def set_kill_switch(*, active: bool, reason: str = "", dry_run: bool = True) -> Tuple[str, bool]:
    return _post("/set_kill_switch", {
        "active": bool(active),
        "reason": reason,
        "dry_run": bool(dry_run),
    })


__all__ = [
    "place_order",
    "cancel_order",
    "cancel_all",
    "flatten_positions",
    "set_kill_switch",
]
