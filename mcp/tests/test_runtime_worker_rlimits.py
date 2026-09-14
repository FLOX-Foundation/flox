"""_apply_rlimits must not swallow a total memory-rlimit failure in
silence.

On macOS, `resource.setrlimit(RLIMIT_AS, ...)` and
`resource.setrlimit(RLIMIT_DATA, ...)` both raise `ValueError` for any
`rss_bytes` below the kernel's hard ceiling -- there is no lower memory
rlimit obtainable on that platform at all. `_apply_rlimits` used to catch
both and move on with `except: continue`, so `run_backtest` reported
success on a completely memory-unbounded worker with no trace anywhere
that the requested limit was never applied.

These tests monkeypatch `resource.setrlimit` directly so the behaviour is
deterministic on every CI platform, rather than depending on the actual
host OS's rlimit semantics.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "mcp"))

resource = pytest.importorskip("resource", reason="POSIX-only")

from flox_mcp.tools._runtime_worker import _apply_rlimits  # noqa: E402


def _deny(*names: str):
    """A fake setrlimit that raises ValueError for the given limit names
    (matching macOS's real error for RLIMIT_AS / RLIMIT_DATA) and
    delegates everything else to a no-op."""
    denied = {getattr(resource, n) for n in names if hasattr(resource, n)}

    def fake_setrlimit(which, limits):
        if which in denied:
            raise ValueError("current limit exceeds maximum limit")
        return None

    return fake_setrlimit


def test_memory_rlimit_total_failure_is_reported(monkeypatch, capsys) -> None:
    monkeypatch.setattr(resource, "setrlimit", _deny("RLIMIT_AS", "RLIMIT_DATA"))

    _apply_rlimits(cpu_seconds=60, rss_bytes=256 * 1024 * 1024, fsize_bytes=1024)

    err = capsys.readouterr().err
    assert "memory" in err.lower()
    assert "unbounded" in err.lower()


def test_memory_rlimit_success_is_silent(monkeypatch, capsys) -> None:
    calls: list[int] = []

    def fake_setrlimit(which, limits):
        calls.append(which)
        return None

    monkeypatch.setattr(resource, "setrlimit", fake_setrlimit)

    _apply_rlimits(cpu_seconds=60, rss_bytes=256 * 1024 * 1024, fsize_bytes=1024)

    err = capsys.readouterr().err
    assert err == ""
    assert resource.RLIMIT_AS in calls


def test_memory_rlimit_falls_back_to_rlimit_data_without_a_warning(
    monkeypatch, capsys
) -> None:
    monkeypatch.setattr(resource, "setrlimit", _deny("RLIMIT_AS"))

    _apply_rlimits(cpu_seconds=60, rss_bytes=256 * 1024 * 1024, fsize_bytes=1024)

    err = capsys.readouterr().err
    assert err == ""
