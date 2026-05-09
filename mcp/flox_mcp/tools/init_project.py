"""init_project — thin MCP wrapper around the canonical `flox new` CLI.

This is not a parallel scaffolder. It shells out to `flox new`
(installed by `flox-py`) and surfaces the exact CLI output. The
purpose is discoverability: an agent that wants to create a new
project from MCP shouldn't have to read `flox-new.md` and replicate
the layout by hand. The CLI stays the source of truth.
"""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path
from typing import Optional


SUPPORTED_TEMPLATES = ("research", "live", "indicator-library")

_NEXT_STEPS = (
    ('record tape', 'capture market data into a `.floxlog` for the project'),
    ('project layout', 'where strategy / data / backtest live in the new project'),
    ('backtest', 'drive a strategy off the recorded data'),
)


def init_project(
    project_name: str,
    template: str,
    target_dir: str = ".",
) -> str:
    if not isinstance(project_name, str) or not project_name.strip():
        return "init_project: `project_name` is required and must be a non-empty string."
    if template not in SUPPORTED_TEMPLATES:
        return (
            f"init_project: unsupported template {template!r}. "
            f"Supported: {', '.join(SUPPORTED_TEMPLATES)}."
        )
    target = Path(target_dir).expanduser()
    if not target.is_dir():
        return (
            f"init_project: `target_dir` does not exist: {target}. "
            f"Create the directory first or point at an existing one."
        )

    flox_cli = shutil.which("flox")
    if flox_cli is None:
        return (
            "init_project: `flox` CLI is not on PATH. Install with "
            "`pip install flox-py` (or `pip install \"flox-mcp[flox]\"`)."
        )

    # `flox new <name> --template=<t>` creates `<target>/<name>/` and
    # populates it from the bundled template tree. `--here` is the
    # in-place flavour but we always use the named-dir form here so
    # the agent can explicitly control where output lands.
    cmd = [flox_cli, "new", project_name, f"--template={template}"]
    try:
        proc = subprocess.run(
            cmd, cwd=str(target), capture_output=True, text=True, timeout=30,
        )
    except subprocess.TimeoutExpired:
        return (
            "init_project: `flox new` exceeded a 30s timeout. "
            "This is unexpected for a scaffolder; check whether the "
            "`flox` CLI is mis-installed."
        )
    except Exception as exc:
        return f"init_project: failed to invoke `flox new`: {type(exc).__name__}: {exc}"

    project_path = target / project_name
    body_lines = [
        f"# init_project: {template} / {project_name}",
        "",
    ]
    if proc.returncode != 0:
        body_lines += [
            f"`flox new` exited {proc.returncode}.",
            "",
            "## stderr",
            "```",
            (proc.stderr or "(empty)").strip(),
            "```",
        ]
        if proc.stdout.strip():
            body_lines += ["", "## stdout", "```", proc.stdout.strip(), "```"]
        return "\n".join(body_lines)

    if proc.stdout.strip():
        body_lines += ["## CLI output", "```", proc.stdout.strip(), "```", ""]
    body_lines += [
        f"Project created at: `{project_path}`.",
        "",
        "## Next steps",
        "",
    ]
    for query, desc in _NEXT_STEPS:
        body_lines.append(f"- `docs_search(\"{query}\")` — {desc}")
    return "\n".join(body_lines)
