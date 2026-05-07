"""Thread-safe in-memory event log for live engine introspection.

The log is a fixed-capacity ring buffer of structured records. The
user's flox app emits to it from inside hooks (signal emitted, order
placed, fill received, risk check passed/failed); the MCP analytics
tools read from it through the control plane.

The buffer keeps the most recent ``capacity`` records; older ones
fall off the back. ``causal_parent_id`` lets analytics walk a
decision back to root cause without touching the engine again.
"""
from __future__ import annotations

import threading
import time
import uuid
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Deque, Dict, List, Mapping, Optional


@dataclass
class EventRecord:
    """One structured event."""

    event_id: str
    timestamp_ns: int
    type: str
    strategy: Optional[str] = None
    causal_parent_id: Optional[str] = None
    payload: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "event_id": self.event_id,
            "timestamp_ns": self.timestamp_ns,
            "type": self.type,
            "strategy": self.strategy,
            "causal_parent_id": self.causal_parent_id,
            "payload": dict(self.payload),
        }


class EventLog:
    """Thread-safe ring buffer of ``EventRecord``. Emit from any hook
    thread; query from the analytics handler on the HTTP server
    thread. ``capacity`` defaults to 10k; tune to match the live
    decision rate so the agent has a useful debugging window."""

    def __init__(self, capacity: int = 10_000) -> None:
        if capacity <= 0:
            raise ValueError("capacity must be > 0")
        self._capacity = int(capacity)
        self._buf: Deque[EventRecord] = deque(maxlen=self._capacity)
        self._lock = threading.Lock()

    @property
    def capacity(self) -> int:
        return self._capacity

    def __len__(self) -> int:
        with self._lock:
            return len(self._buf)

    def emit(
        self,
        type: str,  # noqa: A002 — matches the field name on the record
        *,
        strategy: Optional[str] = None,
        causal_parent_id: Optional[str] = None,
        payload: Optional[Mapping[str, Any]] = None,
        event_id: Optional[str] = None,
        timestamp_ns: Optional[int] = None,
    ) -> EventRecord:
        rec = EventRecord(
            event_id=event_id or uuid.uuid4().hex,
            timestamp_ns=int(timestamp_ns) if timestamp_ns is not None else time.time_ns(),
            type=str(type),
            strategy=strategy,
            causal_parent_id=causal_parent_id,
            payload=dict(payload or {}),
        )
        with self._lock:
            self._buf.append(rec)
        return rec

    def query(
        self,
        *,
        strategy: Optional[str] = None,
        type: Optional[str] = None,  # noqa: A002
        from_ts_ns: Optional[int] = None,
        to_ts_ns: Optional[int] = None,
        limit: Optional[int] = None,
    ) -> List[EventRecord]:
        """Return matching records in arrival order (oldest first).
        Filters AND-compose; empty filters return everything within
        ``limit``."""
        with self._lock:
            snapshot = list(self._buf)
        out: List[EventRecord] = []
        for r in snapshot:
            if strategy is not None and r.strategy != strategy:
                continue
            if type is not None and r.type != type:
                continue
            if from_ts_ns is not None and r.timestamp_ns < int(from_ts_ns):
                continue
            if to_ts_ns is not None and r.timestamp_ns > int(to_ts_ns):
                continue
            out.append(r)
            if limit is not None and len(out) >= int(limit):
                break
        return out

    def find(self, event_id: str) -> Optional[EventRecord]:
        """Lookup by exact event_id. O(n) but n is bounded by
        capacity; the buffer is rarely more than 10k."""
        with self._lock:
            snapshot = list(self._buf)
        for r in snapshot:
            if r.event_id == event_id:
                return r
        return None

    def trace(self, event_id: str, *, max_depth: int = 32) -> List[EventRecord]:
        """Walk the causal-parent chain from ``event_id`` toward root.
        Returns the chain in order (from the requested event back to
        the root). Stops at ``max_depth`` to bound runaway chains."""
        chain: List[EventRecord] = []
        seen: set[str] = set()
        cur: Optional[str] = event_id
        depth = 0
        while cur and depth < max_depth and cur not in seen:
            seen.add(cur)
            rec = self.find(cur)
            if rec is None:
                break
            chain.append(rec)
            cur = rec.causal_parent_id
            depth += 1
        return chain

    def clear(self) -> None:
        with self._lock:
            self._buf.clear()


__all__ = [
    "EventLog",
    "EventRecord",
]
