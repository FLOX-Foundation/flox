"""Tests for the init_project MCP tool — thin wrapper around `flox new`."""
from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "mcp"))

from flox_mcp.tools import init_project as init_project_tool


def test_init_project_rejects_unsupported_template():
    out = init_project_tool.init_project("my_proj", template="quantum",
                                          target_dir="/tmp")
    assert "unsupported template" in out


def test_init_project_rejects_empty_name():
    out = init_project_tool.init_project("", template="research",
                                          target_dir="/tmp")
    assert "required" in out


def test_init_project_rejects_missing_target_dir(tmp_path):
    nope = tmp_path / "does_not_exist"
    out = init_project_tool.init_project("my_proj", template="research",
                                          target_dir=str(nope))
    assert "does not exist" in out


def test_init_project_reports_missing_cli(monkeypatch, tmp_path):
    """When `flox` CLI is not on PATH, the tool returns a helpful
    install hint instead of trying to shell out and failing
    cryptically."""
    monkeypatch.setattr(shutil, "which", lambda name: None)
    out = init_project_tool.init_project("my_proj", template="research",
                                          target_dir=str(tmp_path))
    assert "not on PATH" in out
    assert "pip install flox-py" in out


@pytest.mark.skipif(
    shutil.which("flox") is None,
    reason="needs the `flox` CLI on PATH (install flox-py)",
)
def test_init_project_happy_path(tmp_path):
    out = init_project_tool.init_project("smoke_proj", template="research",
                                          target_dir=str(tmp_path))
    assert "init_project: research / smoke_proj" in out
    assert "Next steps" in out
    assert 'docs_search("record tape")' in out
    # Project directory was actually created.
    assert (tmp_path / "smoke_proj").is_dir()
