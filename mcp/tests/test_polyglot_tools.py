"""Unit tests for the polyglot MCP tools introduced in W2-T014.

Each test calls the framework-agnostic tool function directly (no MCP
client) so failure surfaces are tight. The bundled data must already
be in sync (run ``python3 scripts/sync_mcp_data.py`` once before
running these tests in a fresh checkout).
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "mcp"))

from flox_mcp.tools import (
    docs_search as docs_search_tool,
    examples,
    lookup,
    scaffold,
)


# ── lookup_symbol ─────────────────────────────────────────────────────


def test_lookup_symbol_known_struct():
    out = lookup.lookup_symbol("FloxBarData")
    assert "FloxBarData" in out
    assert "capi" in out
    # Cross-language hit: every binding should contribute *something*
    # for a core event struct unless a binding is in allowlist mode.
    assert "struct" in out


def test_lookup_symbol_indicator_short_name():
    """`ema` → finds flox_indicator_ema in C, plus exposed forms across
    bindings (the function `ema` is exported by both pybind11 and NAPI;
    `EMA` exists as a class — that's a separate lookup)."""
    out = lookup.lookup_symbol("ema")
    assert "flox_indicator_ema" in out
    # Python + Node both export at least one form.
    assert "python" in out
    assert "node" in out


def test_lookup_symbol_indicator_class_name():
    """`EMA` → finds the class form across bindings."""
    out = lookup.lookup_symbol("EMA")
    assert "EMA" in out
    assert "python" in out


def test_lookup_symbol_unknown():
    out = lookup.lookup_symbol("ThisSymbolDoesNotExist_xyz")
    assert "no match" in out.lower()


def test_lookup_symbol_language_filter():
    full = lookup.lookup_symbol("FloxBarData")
    capi_only = lookup.lookup_symbol("FloxBarData", language="capi")
    assert "capi" in capi_only
    # Filter restricts the table; full output mentions other bindings,
    # filtered output should not.
    assert capi_only.count("|") <= full.count("|")


def test_lookup_symbol_invalid_language():
    out = lookup.lookup_symbol("FloxBarData", language="rust")
    assert "unknown language" in out


def test_lookup_symbol_requires_string():
    out = lookup.lookup_symbol("")
    assert "required" in out.lower() or "must be" in out.lower()


# ── list_bindings ─────────────────────────────────────────────────────


@pytest.mark.parametrize("language",
                         ["capi", "python", "node", "codon"])
def test_list_bindings_non_empty(language):
    out = lookup.list_bindings(language=language)
    assert "binding" in out
    # Each surface should have at least a few symbols listed.
    rows = [l for l in out.splitlines() if l.startswith("| `")]
    assert len(rows) > 5, f"{language} surface looks empty: {out}"


def test_list_bindings_quickjs_is_documented_empty():
    """QuickJS contributes nothing today — the tool should explain that
    rather than silently returning empty."""
    out = lookup.list_bindings(language="quickjs")
    assert "QuickJS" in out or "quickjs" in out.lower()


def test_list_bindings_filter_reduces():
    full = lookup.list_bindings(language="capi", limit=200)
    filtered = lookup.list_bindings(language="capi",
                                     filter="indicator", limit=200)
    assert full.count("\n| `") > filtered.count("\n| `")
    assert "indicator" in filtered.lower()


def test_list_bindings_limit_truncates():
    full = lookup.list_bindings(language="capi", limit=10)
    rows = [l for l in full.splitlines() if l.startswith("| `flox_")]
    assert len(rows) <= 10


def test_list_bindings_invalid_language():
    out = lookup.list_bindings(language="ruby")
    assert "unknown language" in out


# ── get_example ───────────────────────────────────────────────────────


def test_get_example_known_topic():
    out = examples.get_example("backtest")
    assert "backtest" in out.lower()
    assert "```" in out, "expected fenced code block in get_example output"


def test_get_example_language_filter():
    out = examples.get_example("backtest", language="python")
    assert "python" in out.lower()
    # Should not include codon-only examples.
    assert "codon_" not in out or "python" in out.split("```")[0].lower()


def test_get_example_unknown_topic():
    out = examples.get_example("does-not-exist")
    assert "unknown topic" in out


def test_get_example_invalid_language():
    out = examples.get_example("backtest", language="rust")
    assert "unknown language" in out


def test_get_example_topic_with_no_match_is_friendly():
    out = examples.get_example("risk")  # we don't bundle risk examples yet
    assert "no examples" in out.lower() or "```" in out


# ── scaffold_strategy ─────────────────────────────────────────────────


@pytest.mark.parametrize("kind", scaffold.SUPPORTED_KINDS)
def test_scaffold_python_parses_and_validates(kind):
    """Template-rot gate for Python — every (kind) renders code that
    (a) compiles in Python, (b) passes the existing validate_strategy."""
    import ast

    from flox_mcp.tools import strategy

    rendered = scaffold.scaffold_strategy(language="python", kind=kind,
                                          name="MyStrat")
    # Strip the markdown wrapper to recover raw source.
    code = _extract_code_block(rendered, "python")
    assert "MyStrat" in code
    ast.parse(code)
    out = strategy.validate_strategy(code)
    # validate_strategy returns Markdown; OK status is the success signal.
    assert "OK" in out
    assert "FAIL" not in out and "missing" not in out.lower()


@pytest.mark.parametrize("kind", scaffold.SUPPORTED_KINDS)
def test_scaffold_node_parses(kind):
    """Template-rot gate for Node — ensure the rendered JS is at least
    syntactically valid by checking it via Node's `--check` if Node is
    available. Skip cleanly when Node is not installed."""
    import shutil
    import subprocess
    import tempfile

    rendered = scaffold.scaffold_strategy(language="node", kind=kind,
                                          name="MyStrat")
    code = _extract_code_block(rendered, "javascript")
    assert "MyStrat" in code

    node = shutil.which("node")
    if node is None:
        pytest.skip("node not available — JS template parse check skipped")
    with tempfile.NamedTemporaryFile(suffix=".mjs", delete=False) as f:
        f.write(code.encode("utf-8"))
        tmp = f.name
    try:
        # `node --check <file>` parses without executing.
        result = subprocess.run([node, "--check", tmp],
                                capture_output=True, text=True)
        assert result.returncode == 0, (
            f"node --check failed for {kind!r}: {result.stderr}"
        )
    finally:
        Path(tmp).unlink(missing_ok=True)


def test_scaffold_invalid_language():
    out = scaffold.scaffold_strategy(language="rust")
    assert "unsupported language" in out


def test_scaffold_missing_language_is_required():
    """language is required — picking the binding for the user is wrong."""
    out = scaffold.scaffold_strategy()
    assert "`language` is required" in out
    assert "polyglot" in out


def test_scaffold_invalid_kind():
    out = scaffold.scaffold_strategy(language="python", kind="quantum-driven")
    assert "unsupported kind" in out


def test_scaffold_invalid_name():
    out = scaffold.scaffold_strategy(language="python", name="123 not an ident")
    assert "valid identifier" in out


def test_scaffold_includes_next_steps():
    """Tool result includes a Next steps section with docs_search queries
    so the agent has explicit pointers at recording / backtest / layout."""
    rendered = scaffold.scaffold_strategy(language="python", kind="bar-driven",
                                           name="MyStrat")
    assert "## Next steps" in rendered
    assert 'docs_search("project layout")' in rendered
    assert 'docs_search("record tape")' in rendered
    assert 'docs_search("backtest")' in rendered


@pytest.mark.parametrize("language", ["codon", "quickjs"])
@pytest.mark.parametrize("kind", scaffold.SUPPORTED_KINDS)
def test_scaffold_codon_quickjs_renders(language, kind):
    """Template-rot gate for the new languages — every (language, kind)
    combination renders with the strategy class name substituted."""
    rendered = scaffold.scaffold_strategy(language=language, kind=kind,
                                           name="MyStrat")
    assert "MyStrat" in rendered
    assert "scaffold_strategy:" in rendered  # markdown header
    assert "## Next steps" in rendered


def _extract_code_block(rendered: str, lang: str) -> str:
    """Pull the body of the first ```<lang> ... ``` fence."""
    in_block = False
    out: list[str] = []
    for line in rendered.splitlines():
        if not in_block and line.strip() == f"```{lang}":
            in_block = True
            continue
        if in_block and line.strip() == "```":
            break
        if in_block:
            out.append(line)
    return "\n".join(out)


# ── docs_search ───────────────────────────────────────────────────────


def test_docs_search_known_query():
    out = docs_search_tool.docs_search("walk forward", k=3)
    assert "walk forward" in out.lower() or "walk-forward" in out.lower()
    assert "docs/" in out


def test_docs_search_phrase_with_punctuation():
    """Inputs with dashes / punctuation must not blow up the FTS5 parser."""
    out = docs_search_tool.docs_search("walk-forward", k=3)
    assert "no matches" not in out.lower()


def test_docs_search_multi_word_and_matches():
    """Plain multi-word queries AND-match every token rather than
    requiring an exact phrase. A query like `ccxt fetch_ohlcv historical`
    has to find docs where every term appears, but they need not be
    adjacent. The previous default phrase-quoted, returning zero hits
    on natural agent queries.
    """
    out = docs_search_tool.docs_search("ccxt fetch_ohlcv historical", k=5)
    assert "no matches" not in out.lower()
    assert "docs/" in out


def test_docs_search_explicit_phrase_still_works():
    """Wrapping in double quotes preserves exact-phrase ranking."""
    out = docs_search_tool.docs_search('"walk forward"', k=3)
    assert "walk forward" in out.lower() or "walk-forward" in out.lower()
    assert "docs/" in out


def test_docs_search_unknown_query_is_friendly():
    out = docs_search_tool.docs_search("xyzpdq nonexistentterm zzzqqq")
    assert "no matches" in out.lower()


def test_docs_search_excludes_internal_paths():
    """Sanity — even if a term appears in private docs, it must not leak."""
    out = docs_search_tool.docs_search("roadmap", k=10)
    # The .notes/roadmap.md is gitignored AND filtered by the
    # allowlist when building the FTS index. Query may legitimately hit
    # docs that mention 'roadmap'; what we check is no internal path.
    assert ".notes/" not in out
    assert "/CLAUDE.md" not in out
    assert "\nCLAUDE.md" not in out


def test_docs_search_invalid_input():
    assert "non-empty" in docs_search_tool.docs_search("")
    assert "positive" in docs_search_tool.docs_search("x", k=0)


def test_docs_search_clamps_k():
    """A huge k is silently clamped (no error)."""
    out = docs_search_tool.docs_search("walk forward", k=10_000)
    # Result must still be Markdown; specific row count depends on
    # corpus, just verify we got *something* and not an error.
    assert "docs_search" in out or "## " in out
