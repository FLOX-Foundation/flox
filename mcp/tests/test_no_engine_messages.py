"""Tests for the no-engine fallback message in runtime / control tools.

A fresh sandbox without an engine returned a config-error message
('FLOX_CONTROL_TOKEN is not set'), which made the agent waste turns
trying to set the env var. The actual situation is "no engine to
talk to". The new message names the standalone alternatives the
agent can use.
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "mcp"))

from flox_mcp.tools import analytics, control


def test_analytics_no_engine_message(monkeypatch):
    monkeypatch.delenv("FLOX_CONTROL_TOKEN", raising=False)
    out = analytics.list_strategies()
    assert "No flox engine detected" in out
    assert "docs_search" in out
    assert "scaffold_strategy" in out


def test_control_no_engine_message(monkeypatch):
    monkeypatch.delenv("FLOX_CONTROL_TOKEN", raising=False)
    out = control.place_order(
        account="any", symbol=1, side="buy", qty=0.1, dry_run=True,
    )
    assert "No flox engine detected" in out
    # Mutating-tool variant lists the mutating tools, not the read-only ones.
    assert "place_order" in out
