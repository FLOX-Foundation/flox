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


def _write_fake_flox_cli(bin_dir: Path) -> None:
    """A stand-in for `flox new <name> --template=<t>` that mirrors just
    the one behaviour this file's comment makes a claim about: the real
    CLI resolves `<name>` against its cwd (`Path.cwd() / project_name` in
    `python/flox_py/cli.py::cmd_new`), so a `project_name` containing
    `..` walks back out of wherever it was invoked. No real `flox`
    install is required to pin this down -- only that one join.
    """
    script = bin_dir / "flox"
    script.write_text(
        "#!/usr/bin/env python3\n"
        "import sys\n"
        "from pathlib import Path\n"
        "name = sys.argv[2]\n"
        "dest = Path.cwd() / name\n"
        "dest.mkdir(parents=True, exist_ok=True)\n"
        "(dest / 'flox.toml').write_text('# fake project\\n')\n"
        "print(f'Created {dest}')\n"
    )
    script.chmod(0o755)


def test_init_project_name_is_not_confined_to_target_dir(monkeypatch, tmp_path):
    """Pins the behaviour the comment above `cmd` in init_project.py
    describes: `project_name` is not restricted to a single path
    component, so `target_dir` is not a containment boundary -- it is
    where the agent points a normal, non-escaping name, same as running
    `flox new` from a shell in that directory. This used to be described
    (misleadingly) as the tool giving the agent "explicit control over
    where output lands", which reads as containment; escaping target_dir
    with `..` here is the same size of surprise as `cd some/dir && flox
    new ../elsewhere` would be from a shell -- expected, not a boundary
    break.
    """
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    _write_fake_flox_cli(bin_dir)
    monkeypatch.setenv("PATH", f"{bin_dir}:{os.environ.get('PATH', '')}")

    target = tmp_path / "target"
    target.mkdir()
    escape_root = tmp_path / "escaped"
    # `target/../escaped/out_here` == `escaped/out_here`, entirely
    # outside `target`.
    out = init_project_tool.init_project(
        "../escaped/out_here", template="research", target_dir=str(target),
    )
    assert "exited" not in out.lower(), out
    assert (escape_root / "out_here").is_dir(), (
        "project_name with `..` did not escape target_dir the way the "
        "underlying `flox new <name>` join does from a shell"
    )
    assert not (target / "escaped").exists(), (
        "the escaped project landed inside target_dir instead of outside it "
        "-- the fake CLI or the path arithmetic in this test is wrong"
    )
    # And a normal, non-escaping name still lands inside target_dir.
    out2 = init_project_tool.init_project(
        "plain_proj", template="research", target_dir=str(target),
    )
    assert (target / "plain_proj").is_dir(), out2
