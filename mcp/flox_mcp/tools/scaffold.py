"""scaffold_strategy — return a starter strategy that compiles + validates.

The output is the rendered template content (not a file path). Templates
live in ``mcp/flox_mcp/data/templates/strategy/<lang>/<kind>.tmpl`` and
are rendered with ``string.Template``. Only the strategy class name is
substituted (``${name}``) — anything more is policy that should live in
a project scaffolder, not in an MCP tool.

A CI test (``mcp/tests/test_scaffold_strategy.py``) renders every
``(language, kind)`` combination and asserts the result parses + (for
Python) passes ``validate_strategy``. That's the template-rot gate
that keeps the tool honest as the surrounding APIs evolve.
"""
from __future__ import annotations

import re
import string
from typing import Optional

from . import _data


SUPPORTED_LANGUAGES = ("python", "node")
SUPPORTED_KINDS = ("bar-driven", "trade-driven", "hybrid")
_VALID_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def scaffold_strategy(language: str = "python", kind: str = "bar-driven",
                      name: str = "MyStrategy") -> str:
    if language not in SUPPORTED_LANGUAGES:
        return (
            f"scaffold_strategy: unsupported language {language!r}. "
            f"Supported: {', '.join(SUPPORTED_LANGUAGES)}. "
            f"(Codon / QuickJS templates are tracked as a follow-up.)"
        )
    if kind not in SUPPORTED_KINDS:
        return (
            f"scaffold_strategy: unsupported kind {kind!r}. "
            f"Supported: {', '.join(SUPPORTED_KINDS)}."
        )
    if not _VALID_NAME.match(name or ""):
        return (
            f"scaffold_strategy: name {name!r} is not a valid identifier. "
            f"Use a class-name-shaped string, e.g. 'EmaCrossStrategy'."
        )

    tmpl_path = _data.template_path(language, kind)
    if tmpl_path is None:
        return (
            f"scaffold_strategy: template "
            f"`templates/strategy/{language}/{kind}.tmpl` is not bundled. "
            f"This is a packaging bug — reinstall flox-mcp or report it."
        )

    raw = tmpl_path.read_text()
    rendered = string.Template(raw).safe_substitute(name=name)

    fence_lang = "javascript" if language == "node" else language
    return (
        f"# scaffold_strategy: {language} / {kind} / `{name}`\n\n"
        f"```{fence_lang}\n{rendered.rstrip()}\n```\n"
    )
