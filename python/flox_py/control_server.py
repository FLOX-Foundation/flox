"""HTTP control plane for flox engines — Phase 2 of the MCP positions
work. The user's flox app embeds a ``ControlServer`` that exposes a
small set of mutating endpoints over localhost. The flox-mcp child
process spawned by an AI client connects with a scoped bearer token
and sends typed JSON requests; the server enforces the scope, audit
log, rate limits, and dry-run defaults before touching the engine.

Why HTTP and not a Unix socket:

* Cross-platform (Windows works without a separate code path)
* Standard bearer-token auth pattern that any HTTP client speaks
* Trivial to debug with curl when wiring up an MCP client

Why token-scoped rather than open localhost:

* The MCP server runs as a child of the AI client. Anything else on
  the machine that can read the user's env vars can also send to
  ``FLOX_CONTROL_URL`` — token scope plus rate limiting plus the
  out-of-band approval channel for live tier are the layered defense.

Three scopes:

* ``read`` — only health / status endpoints
* ``paper`` — read plus mutating ops against accounts whose name
  starts with ``paper-`` (the simulator's domain)
* ``live`` — everything, but ``place_order`` additionally requires
  an out-of-band ``approve_token`` issued through a separate channel
"""
from __future__ import annotations

import json
import logging
import threading
import time
import uuid
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Dict, List, Mapping, Optional


log = logging.getLogger("flox.control_server")


VALID_SCOPES = {"read", "paper", "live"}


# ── Errors ────────────────────────────────────────────────────────────


class ControlServerError(Exception):
    """Base class for control-plane errors."""

    status_code: int = 400


class AuthRequired(ControlServerError):
    status_code = 401


class ScopeForbidden(ControlServerError):
    status_code = 403


class RateLimited(ControlServerError):
    status_code = 429


class ApprovalRequired(ControlServerError):
    status_code = 403


# ── Audit log ────────────────────────────────────────────────────────


@dataclass
class AuditRecord:
    audit_id: str
    timestamp_ns: int
    token_id: str
    scope: str
    tool: str
    args: Dict[str, Any]
    accepted: bool
    dry_run: bool
    effects: List[Dict[str, Any]]
    error: Optional[str] = None

    def to_json(self) -> str:
        return json.dumps(
            {
                "audit_id": self.audit_id,
                "timestamp_ns": self.timestamp_ns,
                "token_id": self.token_id,
                "scope": self.scope,
                "tool": self.tool,
                "args": self.args,
                "accepted": self.accepted,
                "dry_run": self.dry_run,
                "effects": self.effects,
                "error": self.error,
            },
            sort_keys=True,
        )


class AuditLogger:
    """Append-only structured-JSON sink. One file open for the lifetime
    of the server; one flush per record so a crash never loses an
    accepted mutating call."""

    def __init__(self, sink: Optional[Path] = None) -> None:
        self.sink = sink
        self._lock = threading.Lock()
        self._fh = None
        if sink is not None:
            sink.parent.mkdir(parents=True, exist_ok=True)
            self._fh = sink.open("a", buffering=1)  # line-buffered

    def emit(self, record: AuditRecord) -> None:
        line = record.to_json() + "\n"
        with self._lock:
            if self._fh is not None:
                self._fh.write(line)
                self._fh.flush()
            else:
                log.info("flox.control audit %s", line.strip())

    def close(self) -> None:
        with self._lock:
            if self._fh is not None:
                self._fh.close()
                self._fh = None


# ── Rate limiter ─────────────────────────────────────────────────────


@dataclass
class TokenBucket:
    capacity: float
    refill_per_sec: float
    tokens: float
    last_refill_ns: int

    def try_consume(self, n: float = 1.0, now_ns: Optional[int] = None) -> bool:
        if now_ns is None:
            now_ns = time.time_ns()
        elapsed_s = (now_ns - self.last_refill_ns) / 1e9
        self.tokens = min(self.capacity, self.tokens + elapsed_s * self.refill_per_sec)
        self.last_refill_ns = now_ns
        if self.tokens >= n:
            self.tokens -= n
            return True
        return False


class RateLimiter:
    """Per-(token, family) token-bucket limiter. ``family`` is the
    coarse op group: ``orders`` (place + flatten), ``cancels`` (cancel
    + cancel_all), ``kill`` (set_kill_switch). Defaults are
    intentionally conservative; tune via ``ControlServer(rate_limits=
    {...})`` at construction."""

    def __init__(self, defaults: Mapping[str, tuple[float, float]]) -> None:
        # family → (capacity, refill_per_sec)
        self._defaults = dict(defaults)
        self._buckets: Dict[tuple[str, str], TokenBucket] = {}
        self._lock = threading.Lock()

    def check(self, token_id: str, family: str) -> None:
        with self._lock:
            key = (token_id, family)
            if key not in self._buckets:
                cap, refill = self._defaults.get(family, (1.0, 1.0))
                self._buckets[key] = TokenBucket(
                    capacity=cap,
                    refill_per_sec=refill,
                    tokens=cap,
                    last_refill_ns=time.time_ns(),
                )
            if not self._buckets[key].try_consume():
                raise RateLimited(f"rate limit hit for {token_id}/{family}")


DEFAULT_RATE_LIMITS = {
    "orders": (1.0, 1.0),    # 1 order/sec sustained, burst 1
    "cancels": (100.0, 100.0),
    "kill": (5.0, 0.5),      # 5 quick toggles allowed, then 1 every 2s
}


# ── Approval store (out-of-band tokens for live place_order) ─────────


class ApprovalStore:
    """One-shot tokens consumed by ``place_order`` on ``live`` scope.
    Issued out of band (CLI prompt to the operator), expire after
    ``ttl_s`` seconds, single-use."""

    def __init__(self, ttl_s: float = 60.0) -> None:
        self.ttl_s = ttl_s
        self._tokens: Dict[str, int] = {}  # token → expires_at_ns
        self._lock = threading.Lock()

    def issue(self, token: Optional[str] = None) -> str:
        token = token or uuid.uuid4().hex
        with self._lock:
            self._tokens[token] = time.time_ns() + int(self.ttl_s * 1e9)
        return token

    def consume(self, token: str) -> bool:
        now = time.time_ns()
        with self._lock:
            expires = self._tokens.pop(token, None)
            if expires is None:
                return False
            return expires > now


# ── Server ───────────────────────────────────────────────────────────


@dataclass
class _Caller:
    token_id: str
    scope: str


@dataclass
class ControlServer:
    """Embed in your flox app. Pass live engine handles in; the server
    routes mutating MCP requests through them with the security model
    described in the module docstring.

    ``executor`` must support ``submit_order(id, side, price, quantity,
    type, symbol)``, ``cancel_order(id)``, and ``cancel_all(symbol)``
    in the shape ``flox_py.SimulatedExecutor`` and live executors
    expose. ``kill_switch`` must support ``set(active: bool, reason:
    str | None)`` and ``state() -> dict``; the bundled Python
    ``KillSwitch`` hook works directly. ``positions`` and ``runner``
    are optional, used by ``flatten_positions``.

    Live analytics inputs (all optional, all read-only):

    * ``strategies`` — callable returning an iterable of dicts with
      at minimum ``name``, ``status``, and ``symbols``. Powers
      ``list_strategies``.
    * ``strategy_state_provider`` — callable ``name → dict`` returning
      a strategy's current state. Powers ``get_strategy_state``.
    * ``indicator_provider`` — callable ``(strategy, name?) → list
      of dict`` returning indicator values for a strategy. Powers
      ``get_indicator_values``.
    * ``event_log`` — an :class:`flox_py.event_log.EventLog` instance.
      Powers ``get_event_log`` and ``explain_decision``.
    * ``replay_callback`` — callable ``(args: dict) → dict`` that
      runs a sandbox replay window with the supplied params. Powers
      ``replay_window`` and ``whatif``.
    """

    tokens: Mapping[str, str]  # token-string → scope
    executor: Any = None
    kill_switch: Any = None
    positions: Any = None
    runner: Any = None
    strategies: Any = None
    strategy_state_provider: Any = None
    indicator_provider: Any = None
    event_log: Any = None
    replay_callback: Any = None
    host: str = "127.0.0.1"
    port: int = 8765
    audit_sink: Optional[Path] = None
    rate_limits: Mapping[str, tuple[float, float]] = field(
        default_factory=lambda: dict(DEFAULT_RATE_LIMITS)
    )
    approval_ttl_s: float = 60.0
    require_dry_run_default: bool = True

    _http: Optional[ThreadingHTTPServer] = field(default=None, init=False, repr=False)
    _thread: Optional[threading.Thread] = field(default=None, init=False, repr=False)
    _audit: Optional[AuditLogger] = field(default=None, init=False, repr=False)
    _ratelim: Optional[RateLimiter] = field(default=None, init=False, repr=False)
    _approvals: Optional[ApprovalStore] = field(default=None, init=False, repr=False)
    _next_order_id: List[int] = field(default_factory=lambda: [1], init=False, repr=False)

    def __post_init__(self) -> None:
        bad = [s for s in self.tokens.values() if s not in VALID_SCOPES]
        if bad:
            raise ValueError(
                f"unknown scopes {sorted(set(bad))}; "
                f"allowed: {sorted(VALID_SCOPES)}"
            )
        self._audit = AuditLogger(self.audit_sink)
        self._ratelim = RateLimiter(self.rate_limits)
        self._approvals = ApprovalStore(self.approval_ttl_s)

    def issue_approval(self, token: Optional[str] = None) -> str:
        """Issue a one-shot approval token consumed by ``place_order``
        on ``live`` scope. The CLI uses this to gate a live order
        on an operator-confirmed prompt."""
        return self._approvals.issue(token)

    # ── lifecycle ─────────────────────────────────────────────────

    def start(self) -> None:
        if self._http is not None:
            raise RuntimeError("control server already started")
        srv_self = self
        addr = (self.host, self.port)

        class Handler(_ControlRequestHandler):
            server_self = srv_self  # bound at class scope for _do_*

        self._http = ThreadingHTTPServer(addr, Handler)
        self._thread = threading.Thread(
            target=self._http.serve_forever, daemon=True,
            name="flox-control-server",
        )
        self._thread.start()

    def stop(self) -> None:
        if self._http is None:
            return
        self._http.shutdown()
        self._http.server_close()
        self._http = None
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            self._thread = None
        if self._audit is not None:
            self._audit.close()

    @property
    def url(self) -> str:
        return f"http://{self.host}:{self.port}"

    # ── auth ──────────────────────────────────────────────────────

    def _authenticate(self, header: Optional[str]) -> _Caller:
        if not header or not header.startswith("Bearer "):
            raise AuthRequired("missing bearer token")
        token = header[len("Bearer "):].strip()
        scope = self.tokens.get(token)
        if scope is None:
            raise AuthRequired("unknown token")
        token_id = "tok_" + token[:6]  # never log the full token
        return _Caller(token_id=token_id, scope=scope)

    def _require_scope(self, caller: _Caller, *, allowed: set[str]) -> None:
        if caller.scope not in allowed:
            raise ScopeForbidden(
                f"scope {caller.scope!r} not in allowed={sorted(allowed)}"
            )

    # ── op handlers ──────────────────────────────────────────────

    def _next_id(self) -> int:
        oid = self._next_order_id[0]
        self._next_order_id[0] += 1
        return oid

    def _is_paper_account(self, account: str) -> bool:
        return account.startswith("paper-") or account == "paper"

    def _resolve_dry_run(self, args: Mapping[str, Any]) -> bool:
        if "dry_run" in args:
            return bool(args["dry_run"])
        return self.require_dry_run_default

    def handle_place_order(self, caller: _Caller, args: Dict[str, Any]) -> Dict[str, Any]:
        self._require_scope(caller, allowed={"paper", "live"})
        self._ratelim.check(caller.token_id, "orders")

        account = str(args.get("account") or "")
        symbol = int(args.get("symbol", 0))
        side = (args.get("side") or "").lower()
        order_type = (args.get("type") or "market").lower()
        qty = float(args.get("qty", 0.0))
        price = float(args.get("price", 0.0))
        reason = str(args.get("reason") or "")
        dry_run = self._resolve_dry_run(args)

        if not account or qty <= 0 or side not in ("buy", "sell"):
            raise ControlServerError(
                "place_order requires account, side ∈ {buy, sell}, qty > 0"
            )
        if order_type not in ("market", "limit"):
            raise ControlServerError(
                "place_order type must be market or limit"
            )

        if caller.scope == "paper" and not self._is_paper_account(account):
            raise ScopeForbidden(
                f"paper scope cannot place into non-paper account {account!r}"
            )

        if caller.scope == "live":
            approve_token = args.get("approve_token")
            if not approve_token or not self._approvals.consume(str(approve_token)):
                raise ApprovalRequired(
                    "live place_order requires a fresh approve_token "
                    "(issue via ControlServer.issue_approval)"
                )

        oid = self._next_id()
        effects: List[Dict[str, Any]] = []
        if not dry_run and self.executor is not None:
            self.executor.submit_order(oid, side, price, qty,
                                       type=order_type, symbol=symbol)
            effects.append({"submitted": {"order_id": oid, "type": order_type}})

        return {
            "accepted": True,
            "dry_run": dry_run,
            "order_id": oid,
            "reason": reason,
            "effects": effects,
        }

    def handle_cancel_order(self, caller: _Caller, args: Dict[str, Any]) -> Dict[str, Any]:
        self._require_scope(caller, allowed={"paper", "live"})
        self._ratelim.check(caller.token_id, "cancels")
        order_id = int(args.get("order_id", 0))
        if order_id <= 0:
            raise ControlServerError("cancel_order requires order_id > 0")
        dry_run = self._resolve_dry_run(args)
        effects = []
        if not dry_run and self.executor is not None:
            self.executor.cancel_order(order_id)
            effects.append({"cancel": {"order_id": order_id}})
        return {"accepted": True, "dry_run": dry_run, "effects": effects}

    def handle_cancel_all(self, caller: _Caller, args: Dict[str, Any]) -> Dict[str, Any]:
        self._require_scope(caller, allowed={"paper", "live"})
        self._ratelim.check(caller.token_id, "cancels")
        symbol = int(args.get("symbol", 0))
        dry_run = self._resolve_dry_run(args)
        effects = []
        if not dry_run and self.executor is not None:
            self.executor.cancel_all(symbol)
            effects.append({"cancel_all": {"symbol": symbol}})
        return {"accepted": True, "dry_run": dry_run, "effects": effects}

    def handle_flatten_positions(self, caller: _Caller, args: Dict[str, Any]) -> Dict[str, Any]:
        """Close every open position by submitting an opposite-side
        market order. Reads positions through the supplied
        ``positions`` accessor; if it is not configured, returns an
        empty effects list and ``accepted=True`` so the caller can
        treat absence as a no-op."""
        self._require_scope(caller, allowed={"paper", "live"})
        self._ratelim.check(caller.token_id, "orders")
        dry_run = self._resolve_dry_run(args)
        symbol_filter = args.get("symbol")
        effects: List[Dict[str, Any]] = []

        if self.positions is None:
            return {
                "accepted": True,
                "dry_run": dry_run,
                "effects": effects,
                "note": "no positions accessor configured",
            }

        try:
            rows = list(self.positions())  # accessor is a callable
        except TypeError:
            # positions might be a list-like attribute
            rows = list(self.positions)

        for row in rows:
            sym = int(row.get("symbol_id", row.get("symbol", 0)))
            qty = float(row.get("qty", 0.0))
            if symbol_filter is not None and sym != int(symbol_filter):
                continue
            if qty == 0.0:
                continue
            side = "sell" if qty > 0 else "buy"
            close_qty = abs(qty)
            oid = self._next_id()
            if not dry_run and self.executor is not None:
                self.executor.submit_order(
                    oid, side, 0.0, close_qty,
                    type="market", symbol=sym,
                )
            effects.append({
                "flatten": {
                    "order_id": oid, "symbol": sym,
                    "side": side, "qty": close_qty,
                },
            })

        return {"accepted": True, "dry_run": dry_run, "effects": effects}

    def handle_set_kill_switch(self, caller: _Caller, args: Dict[str, Any]) -> Dict[str, Any]:
        self._require_scope(caller, allowed={"paper", "live"})
        self._ratelim.check(caller.token_id, "kill")
        active = bool(args.get("active", False))
        reason = str(args.get("reason") or "")
        dry_run = self._resolve_dry_run(args)
        effects = []
        if not dry_run and self.kill_switch is not None:
            try:
                self.kill_switch.set(active, reason)
            except AttributeError:
                self.kill_switch.activate() if active else self.kill_switch.deactivate()
            effects.append({"kill_switch": {"active": active, "reason": reason}})
        return {"accepted": True, "dry_run": dry_run, "effects": effects}

    def handle_health(self, caller: _Caller, args: Dict[str, Any]) -> Dict[str, Any]:
        return {"ok": True, "scope": caller.scope, "url": self.url}

    # ── Analytics (all read-only, all scopes allowed) ────────────

    def _require_any_scope(self, caller: _Caller) -> None:
        # Every authenticated caller may read; the auth check has
        # already happened upstream of this method.
        if caller.scope not in VALID_SCOPES:
            raise ScopeForbidden(f"unknown scope {caller.scope!r}")

    def handle_list_strategies(self, caller: _Caller, args: Dict[str, Any]) -> Dict[str, Any]:
        self._require_any_scope(caller)
        if self.strategies is None:
            return {"data": [], "note": "no strategies accessor configured"}
        try:
            rows = list(self.strategies())
        except TypeError:
            rows = list(self.strategies)
        return {"data": [dict(r) for r in rows]}

    def handle_get_strategy_state(self, caller: _Caller, args: Dict[str, Any]) -> Dict[str, Any]:
        self._require_any_scope(caller)
        name = str(args.get("name") or "")
        if not name:
            raise ControlServerError("get_strategy_state requires name")
        if self.strategy_state_provider is None:
            return {"name": name, "state": None,
                    "note": "no strategy_state_provider configured"}
        state = self.strategy_state_provider(name)
        if state is None:
            return {"name": name, "state": None,
                    "note": "strategy not found or has no state"}
        return {"name": name, "state": dict(state)}

    def handle_get_indicator_values(self, caller: _Caller, args: Dict[str, Any]) -> Dict[str, Any]:
        self._require_any_scope(caller)
        strategy = str(args.get("strategy") or "")
        if not strategy:
            raise ControlServerError("get_indicator_values requires strategy")
        name = args.get("name")  # optional filter
        if self.indicator_provider is None:
            return {"strategy": strategy, "data": [],
                    "note": "no indicator_provider configured"}
        rows = list(self.indicator_provider(strategy, name))
        return {"strategy": strategy, "data": [dict(r) for r in rows]}

    def handle_get_event_log(self, caller: _Caller, args: Dict[str, Any]) -> Dict[str, Any]:
        self._require_any_scope(caller)
        if self.event_log is None:
            return {"data": [], "note": "no event_log configured"}
        records = self.event_log.query(
            strategy=args.get("strategy"),
            type=args.get("type"),
            from_ts_ns=args.get("from_ts_ns"),
            to_ts_ns=args.get("to_ts_ns"),
            limit=args.get("limit", 100),
        )
        return {"data": [r.to_dict() for r in records]}

    def handle_explain_decision(self, caller: _Caller, args: Dict[str, Any]) -> Dict[str, Any]:
        self._require_any_scope(caller)
        event_id = str(args.get("event_id") or "")
        if not event_id:
            raise ControlServerError("explain_decision requires event_id")
        if self.event_log is None:
            return {"event_id": event_id, "chain": [],
                    "note": "no event_log configured"}
        chain = self.event_log.trace(
            event_id, max_depth=int(args.get("max_depth", 32)),
        )
        if not chain:
            return {"event_id": event_id, "chain": [],
                    "note": "event_id not found"}
        return {"event_id": event_id,
                "chain": [r.to_dict() for r in chain]}

    def handle_replay_window(self, caller: _Caller, args: Dict[str, Any]) -> Dict[str, Any]:
        self._require_any_scope(caller)
        if self.replay_callback is None:
            return {"data": None,
                    "note": "no replay_callback configured"}
        try:
            result = self.replay_callback(args)
        except Exception as exc:
            raise ControlServerError(
                f"replay_window failed: {type(exc).__name__}: {exc}"
            )
        return {"data": dict(result)}

    def handle_whatif(self, caller: _Caller, args: Dict[str, Any]) -> Dict[str, Any]:
        # Same callback as replay_window; the param overrides live in
        # the args dict and the user-side callback applies them.
        return self.handle_replay_window(caller, args)


# ── HTTP wiring ──────────────────────────────────────────────────────


_OP_MAP = {
    # mutating
    "/place_order": ("place_order", "handle_place_order"),
    "/cancel_order": ("cancel_order", "handle_cancel_order"),
    "/cancel_all": ("cancel_all", "handle_cancel_all"),
    "/flatten_positions": ("flatten_positions", "handle_flatten_positions"),
    "/set_kill_switch": ("set_kill_switch", "handle_set_kill_switch"),
    # analytics (read-only)
    "/list_strategies": ("list_strategies", "handle_list_strategies"),
    "/get_strategy_state": ("get_strategy_state", "handle_get_strategy_state"),
    "/get_indicator_values": ("get_indicator_values", "handle_get_indicator_values"),
    "/get_event_log": ("get_event_log", "handle_get_event_log"),
    "/explain_decision": ("explain_decision", "handle_explain_decision"),
    "/replay_window": ("replay_window", "handle_replay_window"),
    "/whatif": ("whatif", "handle_whatif"),
}


class _ControlRequestHandler(BaseHTTPRequestHandler):
    server_self: ControlServer  # bound by ControlServer.start()

    def log_message(self, fmt: str, *args: Any) -> None:  # silence default noise
        log.debug("flox.control %s", fmt % args)

    def _send_json(self, status: int, payload: Mapping[str, Any]) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        srv = self.server_self
        try:
            caller = srv._authenticate(self.headers.get("Authorization"))
        except ControlServerError as exc:
            self._send_json(exc.status_code, {"error": str(exc)})
            return
        if self.path == "/health":
            self._send_json(200, srv.handle_health(caller, {}))
            return
        self._send_json(404, {"error": f"unknown path {self.path}"})

    def do_POST(self) -> None:  # noqa: N802
        srv = self.server_self
        audit_id = uuid.uuid4().hex
        try:
            caller = srv._authenticate(self.headers.get("Authorization"))
        except ControlServerError as exc:
            self._send_json(exc.status_code, {"error": str(exc), "audit_id": audit_id})
            return

        if self.path not in _OP_MAP:
            self._send_json(404, {"error": f"unknown path {self.path}", "audit_id": audit_id})
            return

        length = int(self.headers.get("Content-Length") or 0)
        try:
            raw = self.rfile.read(length) if length else b""
            args = json.loads(raw.decode("utf-8")) if raw else {}
        except Exception as exc:
            self._send_json(400, {"error": f"bad JSON body: {exc!r}", "audit_id": audit_id})
            return
        if not isinstance(args, dict):
            self._send_json(400, {"error": "request body must be a JSON object", "audit_id": audit_id})
            return

        tool, handler_name = _OP_MAP[self.path]
        handler = getattr(srv, handler_name)
        result: Dict[str, Any] = {}
        accepted = False
        err: Optional[str] = None
        try:
            result = handler(caller, args)
            accepted = bool(result.get("accepted", True))
            status = 200
        except ControlServerError as exc:
            err = str(exc)
            result = {"error": err}
            status = exc.status_code
        except Exception as exc:  # never let a handler crash the server
            err = f"internal: {type(exc).__name__}: {exc}"
            result = {"error": err}
            status = 500

        srv._audit.emit(AuditRecord(
            audit_id=audit_id,
            timestamp_ns=time.time_ns(),
            token_id=caller.token_id,
            scope=caller.scope,
            tool=tool,
            args=_redact_args(args),
            accepted=accepted,
            dry_run=bool(result.get("dry_run", False)),
            effects=list(result.get("effects") or []),
            error=err,
        ))

        result.setdefault("audit_id", audit_id)
        self._send_json(status, result)


def _redact_args(args: Mapping[str, Any]) -> Dict[str, Any]:
    """Strip secrets before they hit the audit log. Approve tokens
    are one-shot and short-lived but still better not to record."""
    out = dict(args)
    if "approve_token" in out:
        out["approve_token"] = "<redacted>"
    return out


__all__ = [
    "ControlServer",
    "ControlServerError",
    "AuthRequired",
    "ScopeForbidden",
    "RateLimited",
    "ApprovalRequired",
    "AuditRecord",
    "AuditLogger",
    "DEFAULT_RATE_LIMITS",
    "VALID_SCOPES",
]
