"""Tests for the record_data MCP tool — wrapper around the canonical
recording paths (`flox tape record` / `scripts/backfill_to_tape.py`)."""
from __future__ import annotations

import shutil
import sys
from pathlib import Path
from unittest.mock import patch

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "mcp"))

from flox_mcp.tools import record_data as record_data_tool


def test_record_data_rejects_unknown_mode():
    out = record_data_tool.record_data(
        mode="quantum", exchange="bitget", symbol="BTC/USDT",
        out_path="/tmp/x",
    )
    assert "unsupported mode" in out


def test_record_data_rejects_unknown_data_type():
    out = record_data_tool.record_data(
        mode="historical", exchange="bitget", symbol="BTC/USDT",
        out_path="/tmp/x", data_type="ticks",
        from_dt="2026-04-01", to_dt="2026-04-02",
    )
    assert "unsupported data_type" in out


def test_record_data_requires_dates_for_historical():
    out = record_data_tool.record_data(
        mode="historical", exchange="bitget", symbol="BTC/USDT",
        out_path="/tmp/x",
    )
    assert "from_dt" in out
    assert "to_dt" in out


def test_record_data_requires_core_args():
    out = record_data_tool.record_data(
        mode="historical", exchange="", symbol="BTC/USDT",
        out_path="/tmp/x",
    )
    assert "required" in out


def test_record_data_live_reports_missing_cli(monkeypatch):
    """When `flox` CLI is not on PATH, the live path returns an
    install hint instead of failing cryptically."""
    monkeypatch.setattr(shutil, "which", lambda name: None)
    out = record_data_tool.record_data(
        mode="live", exchange="bitget", symbol="BTC/USDT",
        out_path="/tmp/x", duration="10s",
    )
    assert "not on PATH" in out
    assert "pip install flox-py" in out


def test_record_data_historical_dispatches_to_backfill_script():
    """Historical mode shells out to scripts/backfill_to_tape.py.
    Mock subprocess.run to capture the command without actually
    running ccxt."""
    import subprocess
    captured = {}

    class FakeProc:
        returncode = 3  # cap exceeded
        stdout = '{"error": "estimated 50000000 records exceeds cap"}'
        stderr = ""

    def fake_run(cmd, **kwargs):
        captured["cmd"] = cmd
        return FakeProc()

    with patch.object(subprocess, "run", fake_run):
        out = record_data_tool.record_data(
            mode="historical", exchange="bitget", symbol="BTC/USDT",
            out_path="/tmp/x", data_type="klines",
            from_dt="2026-04-01", to_dt="2026-12-01",
            max_records=1_000,
        )
    assert "cmd" in captured
    cmd = captured["cmd"]
    assert "scripts/backfill_to_tape.py" in " ".join(cmd)
    assert "--exchange" in cmd
    assert "bitget" in cmd
    # Cap-exceeded surfaced from backfill script.
    assert "exceeds cap" in out or "exited 3" in out
