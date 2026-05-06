"""get_example — return canonical example code matching a topic + language.

Source: ``mcp/flox_mcp/data/examples_index.json`` (built by
``scripts/sync_mcp_data.py``). Source files come along in the wheel
under ``mcp/flox_mcp/data/examples/`` when bundled — but we read
straight from the repo root when MCP runs in a checkout, so a
just-edited example surfaces immediately during development.
"""
from __future__ import annotations

from pathlib import Path
from typing import List, Optional

from . import _data


VALID_TOPICS = ("strategy", "connector", "indicator",
                "event-handler", "risk", "backtest")
VALID_LANGUAGES = ("python", "node", "codon", "cpp")


_REPO_ROOT_GUESS = Path(__file__).resolve().parents[3]


def _read_example(rel_path: str) -> Optional[str]:
    """Read example source. Prefer in-repo location (so dev edits
    show up), fall back to wheel-bundled copy."""
    p = _REPO_ROOT_GUESS / rel_path
    if p.exists():
        return p.read_text()
    bundled = _data._data_path(rel_path)  # noqa: SLF001
    if bundled is not None and bundled.exists():
        return bundled.read_text()
    return None


def get_example(topic: str, language: Optional[str] = None) -> str:
    """Return Markdown listing examples matching the topic.

    Each entry includes path, language, a one-line description (the
    first non-empty comment in the source), and the full code block.
    """
    if topic not in VALID_TOPICS:
        return (
            f"get_example: unknown topic {topic!r}. "
            f"Valid: {', '.join(VALID_TOPICS)}."
        )
    if language is not None and language not in VALID_LANGUAGES:
        return (
            f"get_example: unknown language {language!r}. "
            f"Valid: {', '.join(VALID_LANGUAGES)}."
        )

    index = _data.load_examples_index()
    if index is None:
        return (
            "get_example: bundled examples_index.json missing. "
            "Reinstall flox-mcp."
        )

    matches: List[dict] = [
        e for e in index.get("examples", [])
        if e.get("topic") == topic
        and (language is None or e.get("language") == language)
    ]
    if not matches:
        suffix = f" (language={language!r})" if language else ""
        return (
            f"# get_example: no examples found for topic `{topic}`{suffix}\n\n"
            f"Available topics: {', '.join(VALID_TOPICS)}."
        )

    lines = [f"# {len(matches)} example(s) for topic `{topic}`"
             + (f" / language `{language}`" if language else ""), ""]
    for ex in matches:
        path = ex["path"]
        code = _read_example(path)
        first_comment = _first_comment(code, ex["language"]) if code else ""
        description = first_comment or "(no description)"
        lines.append(f"## `{path}` — {ex['language']}")
        lines.append("")
        lines.append(description)
        lines.append("")
        if code is None:
            lines.append("_(source file unavailable — index points at it but the "
                         "wheel did not bundle it)_")
        else:
            fence_lang = {"node": "javascript",
                          "cpp": "cpp"}.get(ex["language"], ex["language"])
            lines.append(f"```{fence_lang}")
            lines.append(code.rstrip())
            lines.append("```")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def _first_comment(code: Optional[str], language: str) -> str:
    """Pull the first non-empty comment line out of the source."""
    if not code:
        return ""
    for raw in code.splitlines():
        line = raw.strip()
        if not line:
            continue
        if language == "python":
            if line.startswith('"""') or line.startswith("'''"):
                # First docstring line.
                return line.strip("'\" ")
            if line.startswith("#"):
                return line.lstrip("# ").strip()
        elif language in ("node", "cpp"):
            if line.startswith("//"):
                return line.lstrip("/ ").strip()
            if line.startswith("/*"):
                return line.strip("/* ").strip()
        elif language == "codon":
            if line.startswith("#"):
                return line.lstrip("# ").strip()
        # Reading actual code → stop.
        if not line.startswith(("#", "//", "/*", '"""', "'''")):
            return ""
    return ""
