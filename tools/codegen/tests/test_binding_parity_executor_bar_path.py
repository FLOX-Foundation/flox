"""The executor bar path has to reach all four bindings, not just C++.

`BacktestRunner::runBars` walks a bar open -> low -> high -> close, holds what
the bar callback submits and releases it at the next open. The C ABI carries
none of that: `flox_simulated_executor_on_bar(handle, symbol, close)` is the
whole of the bar path a binding can reach, so a caller driving the executor by
hand gets neither the open nor the intrabar extremes, and a held order has
nowhere to be held.

This file pins the declarations the other three bindings are generated and
checked against:

  * the IDL spec (`include/flox/capi/flox_capi_spec.hpp`) -- the source every
    binding is derived from, plus `close_reason` on `FloxBar`;
  * the Codon golden, which the parity gate accepts as Codon's coverage, and
    the shipped Codon module, which it does not look at;
  * `tools/codegen/binding_parity.yaml`, the manifest
    `scripts/check_binding_parity.py` reads.

On the manifest spelling: today that gate resolves a pybind11 / NAPI
`functions:` entry against top-level functions only (`^def` in the `.pyi`,
`^export function` in the `.d.ts`), so a method on a class is invisible to it
and every entry asserted here reads as missing. Making the gate see methods is
its own task; the entries are written in the form that task will read --
`Class.method`, the name a caller types -- so the manifest does not have to be
rewritten once it lands. Until then `check_binding_parity.py` reports these
four groups as missing, which is the accurate answer: the methods are not
there.
"""
from __future__ import annotations

import re
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[3]
SPEC = REPO / "include" / "flox" / "capi" / "flox_capi_spec.hpp"
MANIFEST = REPO / "tools" / "codegen" / "binding_parity.yaml"
CODON_GOLDEN = REPO / "tools" / "codegen" / "golden" / "flox_capi.codon"
CAPI_HEADER = REPO / "include" / "flox" / "capi" / "flox_capi.h"
CAPI_GOLDEN = REPO / "tools" / "codegen" / "golden" / "flox_capi.h"
CODON_MODULE = REPO / "codon" / "flox" / "backtest.codon"

# The C entry points the bar path needs, on top of the close-only
# `flox_simulated_executor_on_bar` that already exists.
C_FUNCTIONS = [
    "flox_simulated_executor_on_bar_ohlc",
    "flox_simulated_executor_begin_bar_callback_window",
    "flox_simulated_executor_end_bar_callback_window",
    "flox_simulated_executor_reset",
]

EXPECTED_MANIFEST_ENTRIES = {
    # pybind11 wraps the C++ engine directly, so the four C entry points are
    # allowlisted under the binding with the reason every other direct
    # wrapper carries; the gate holds the group required.
    "pybind11": {"allowlisted": C_FUNCTIONS},
    # NAPI calls the C entry points, so they must be reachable from node/src
    # (the gate checks the source) and must not be allowlisted away.
    "napi": {"referenced": C_FUNCTIONS},
    "quickjs": {
        "mapped": {
            "flox_simulated_executor_on_bar_ohlc": "__flox_simulated_executor_on_bar_ohlc",
            "flox_simulated_executor_begin_bar_callback_window":
                "__flox_simulated_executor_begin_bar_callback_window",
            "flox_simulated_executor_end_bar_callback_window":
                "__flox_simulated_executor_end_bar_callback_window",
            "flox_simulated_executor_reset": "__flox_simulated_executor_reset",
        }
    },
}


def _spec_text() -> str:
    return SPEC.read_text(encoding="utf-8")


def _manifest_group(name: str) -> dict:
    groups = yaml.safe_load(MANIFEST.read_text(encoding="utf-8")).get("groups", {})
    assert name in groups, f"{MANIFEST.name} has no `{name}` group"
    return groups[name] or {}


def test_the_idl_declares_the_bar_path() -> None:
    text = _spec_text()
    # Green control: the close-only form is declared, and is all there is.
    assert "flox_simulated_executor_on_bar(" in text

    missing = [fn for fn in C_FUNCTIONS if f"{fn}(" not in text]
    assert missing == [], (
        "the IDL spec declares none of "
        + ", ".join(missing)
        + "; every binding is generated or checked against it, so the bar path "
        "starts there"
    )


def test_on_bar_ohlc_takes_the_open() -> None:
    """The mutation this is shaped against: a four-argument signature that
    drops the open and leaves a held order with nothing to be released at."""
    text = _spec_text()
    marker = "flox_simulated_executor_on_bar_ohlc("
    index = text.find(marker)
    assert index >= 0, "flox_simulated_executor_on_bar_ohlc is not declared"

    declaration = text[index : text.find(";", index)]
    for parameter in ("open_price", "high_price", "low_price", "close_price"):
        assert parameter in declaration, (
            f"{parameter} is missing from {declaration.strip()}; the open is the "
            "first price of the bar and so the first price a held order may trade at"
        )


def test_the_aggregation_struct_carries_a_close_reason() -> None:
    """`FloxBarData` (the live callback) has `close_reason`; `FloxBar` (batch
    aggregation) does not, so why a bar closed is readable on one path only."""
    text = _spec_text()
    start = text.find("typedef struct", text.find("Bar aggregation"))
    end = text.find("} FloxBar;", start)
    assert start >= 0 and end > start, "could not locate the FloxBar declaration"

    assert "close_reason" in text[start:end], (
        "FloxBar has no close_reason; add `uint8_t close_reason;` and carry "
        "flox::Bar::reason through the batch aggregation path"
    )


def test_the_codon_golden_imports_the_bar_path() -> None:
    """The parity gate accepts the golden as Codon's coverage, and the golden
    is generated from the IDL -- so the group lands here or in neither."""
    text = CODON_GOLDEN.read_text(encoding="utf-8")
    missing = [fn for fn in C_FUNCTIONS if fn not in text]
    assert missing == [], (
        "the Codon golden imports none of "
        + ", ".join(missing)
        + "; regenerate it from the spec (tools/codegen/scripts/regenerate.sh)"
    )


def test_the_shipped_codon_module_exposes_the_bar_path() -> None:
    """What the golden cannot say: the gate reads the generated file, not the
    module a Codon strategy actually imports."""
    text = CODON_MODULE.read_text(encoding="utf-8")
    expected = [
        "def on_bar_ohlc(",
        "def begin_bar_callback_window(",
        "def end_bar_callback_window(",
        # Spelled with the receiver so it cannot be satisfied by the
        # `reset_stats` / `reset_rolling` methods other classes in this module
        # already have.
        "def reset(self)",
    ]
    missing = [name for name in expected if name not in text]
    assert missing == [], (
        f"{CODON_MODULE.relative_to(REPO)} declares none of "
        + ", ".join(missing)
        + "; SimulatedExecutor there stops at on_bar(symbol, close_price)"
    )


def test_the_manifest_requires_the_bar_path_from_every_binding() -> None:
    manifest = yaml.safe_load(MANIFEST.read_text())
    group = _manifest_group("simulated_executor")
    allowlists = manifest.get("allowlist_functions") or {}

    problems: list[str] = []
    for binding, expected in EXPECTED_MANIFEST_ENTRIES.items():
        entry = group.get(binding)
        if entry is None:
            problems.append(f"{binding}: no entry at all")
            continue
        if entry.get("status") != "required":
            problems.append(f"{binding}: status is {entry.get('status')!r}, not 'required'")
            continue
        if "allowlisted" in expected:
            listed = allowlists.get(binding) or {}
            for name in expected["allowlisted"]:
                reason = listed.get(name)
                if not isinstance(reason, str) or not reason.strip():
                    problems.append(f"{binding}: {name} has no allowlist entry with a reason")
        if "referenced" in expected:
            listed = allowlists.get(binding) or {}
            for name in expected["referenced"]:
                if name in listed:
                    problems.append(f"{binding}: {name} is allowlisted away instead of wrapped")
            src = "".join(p.read_text() for p in (REPO / "node" / "src").rglob("*.h"))
            for name in expected["referenced"]:
                if name not in src:
                    problems.append(f"{binding}: {name} is not referenced under node/src")
        if "mapped" in expected:
            declared = entry.get("functions") or {}
            for c_name, js_name in expected["mapped"].items():
                if declared.get(c_name) != js_name:
                    problems.append(f"{binding}: {c_name} is not mapped to {js_name}")

    assert problems == [], (
        "tools/codegen/binding_parity.yaml does not require the bar path:\n  "
        + "\n  ".join(problems)
    )


def test_the_manifest_keeps_codon_declared() -> None:
    """Codon carries no function list -- the gate reads the golden for it --
    so the only thing to hold is that the group stays required."""
    group = _manifest_group("simulated_executor")
    codon = group.get("codon") or {}
    assert codon.get("status") == "required", (
        "the simulated_executor group must stay `codon: required`; the gate "
        "resolves it against the generated golden"
    )


def _abi_version(path: Path) -> int:
    """The FLOX_CAPI_ABI_VERSION a header declares."""
    text = path.read_text(encoding="utf-8")
    match = re.search(r"^#define\s+FLOX_CAPI_ABI_VERSION\s+(\d+)\s*$", text, re.MULTILINE)
    assert match, f"{path.relative_to(REPO)} declares no FLOX_CAPI_ABI_VERSION"
    return int(match.group(1))


def test_the_abi_number_agrees_across_the_spec_and_the_headers() -> None:
    """The number a consumer compares before trusting a struct layout.

    It lives in three files that are supposed to move together: the IDL spec,
    the golden the spec generates, and the header shipped to consumers. The
    only test that touched it compared the compiled macro with the function the
    same header compiled, which is true whatever the number says -- so the
    shipped header could be rolled back on its own and nothing said a word.
    """
    spec = _abi_version(SPEC)
    golden = _abi_version(CAPI_GOLDEN)
    live = _abi_version(CAPI_HEADER)

    assert spec == golden == live, (
        "FLOX_CAPI_ABI_VERSION disagrees: "
        f"{SPEC.relative_to(REPO)}={spec}, {CAPI_GOLDEN.relative_to(REPO)}={golden}, "
        f"{CAPI_HEADER.relative_to(REPO)}={live}. The shipped header is what a "
        "consumer compiles against and what flox_capi_abi_version() returns."
    )


def test_the_abi_number_moved_with_the_struct_shape() -> None:
    """`FloxBar` gained `close_reason`, which is a shape change on the
    boundary: a header that has the field and still calls itself 2 is
    describing a struct it does not have."""
    text = CAPI_HEADER.read_text(encoding="utf-8")
    end = text.find("} FloxBar;")
    assert end > 0, "could not locate the FloxBar declaration"
    start = text.rfind("typedef struct", 0, end)
    assert start >= 0, "could not locate the FloxBar declaration"

    if "close_reason" in text[start:end]:
        assert _abi_version(CAPI_HEADER) >= 3, (
            "FloxBar carries close_reason but the header reports ABI "
            f"{_abi_version(CAPI_HEADER)}"
        )
