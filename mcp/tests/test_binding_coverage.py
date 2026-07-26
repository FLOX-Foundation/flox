"""Every FLOX surface must be inventoried, including C++ and QuickJS.

The manifest used to cover four surfaces of six, and two of those at the wrong
granularity:

    cpp        absent entirely — `list_bindings('cpp')` was "unknown language",
               and `lookup_symbol('BacktestConfig')` reported "no match" for a
               struct that is C++-only. `SimulatedExecutor` resolved to its
               Python and Node *wrappers* while the class they wrap was
               invisible.
    quickjs    zero symbols — the tool answered "No QuickJS exports recorded
               yet" for a surface with ~500 registered globals.
    codon      inventoried from the generated FFI golden, so it listed `flox_*`
               C names; a Codon user writing `BacktestRunner` found nothing.
    python     classes and module functions only, no methods, so `run_csv`
    node       resolved nowhere in any binding.

C++ matters most of the two additions: it is the engine, every other surface
wraps it, and it is what the only two people who ever did substantial work in
a fork actually used.
"""

from __future__ import annotations

import pytest

from flox_mcp.tools import lookup

ALL_SURFACES = ("cpp", "capi", "python", "node", "codon", "quickjs")


@pytest.mark.parametrize("language", ALL_SURFACES)
def test_every_surface_enumerates(language: str) -> None:
    out = lookup.list_bindings(language, limit=1)
    assert "unknown language" not in out, out
    assert "symbol(s)" in out, out
    assert "| `" in out, f"{language} returned a header but no rows:\n{out}"


@pytest.mark.parametrize("language,minimum", [
    ("cpp", 400),        # 250 public headers
    ("capi", 700),       # 729 IDL functions
    ("python", 500),
    ("node", 500),
    ("codon", 500),
    ("quickjs", 300),    # ~500 registered globals
])
def test_surface_is_not_a_stub(language: str, minimum: int) -> None:
    """Guards against a surface silently regressing to a handful of entries.

    quickjs shipped at exactly zero for long enough that the tool grew a
    hand-written apology for it.
    """
    out = lookup.list_bindings(language, limit=1)
    count = int(out.split("(")[1].split(" ")[0])
    assert count >= minimum, f"{language} inventories only {count} symbols"


def test_cpp_is_a_valid_language() -> None:
    assert "cpp" in lookup.VALID_LANGUAGES


def test_cpp_only_type_resolves() -> None:
    """BacktestConfig exists in no binding — only in C++."""
    out = lookup.lookup_symbol("BacktestConfig")
    assert "no match" not in out, out
    assert "cpp" in out
    assert "backtest_config.h" in out, "the header is what makes it actionable"


def test_wrapped_class_resolves_to_the_definition_first() -> None:
    """SimulatedExecutor is a C++ class; the others wrap it. C++ leads."""
    out = lookup.lookup_symbol("SimulatedExecutor")
    body = [l for l in out.splitlines() if l.startswith("| ") and "Binding" not in l
            and "---" not in l]
    assert body, out
    assert body[0].split("|")[1].strip() == "cpp", f"first row was {body[0]}"
    for surface in ("python", "node", "codon"):
        assert surface in out, f"{surface} missing from {out}"


def test_codon_resolves_its_own_api_not_c_names() -> None:
    out = lookup.list_bindings("codon", filter="BacktestRunner", limit=5)
    assert "BacktestRunner" in out, "Codon's own class must be findable"
    assert "No symbols match" not in out


def test_methods_resolve_with_their_owner() -> None:
    out = lookup.lookup_symbol("run_csv")
    assert "no match" not in out, out
    assert "method" in out
    assert "BacktestRunner.run_csv" in out, "a bare method name is not actionable"


def test_quickjs_globals_are_inventoried() -> None:
    out = lookup.list_bindings("quickjs", filter="set_queue_fifo_top_n", limit=5)
    assert "No symbols match" not in out, (
        "the global whose missing registration shipped as a ReferenceError "
        "should be visible in the inventory")


def test_unknown_language_still_rejected() -> None:
    out = lookup.list_bindings("fortran", limit=1)
    assert "unknown language" in out
    assert "cpp" in out, "the error should list cpp among the valid surfaces"
