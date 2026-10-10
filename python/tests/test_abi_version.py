"""The Python binding must check the C ABI version at import.

``include/lrvx/capi/lrvx_capi.h`` says to "compare LRVX_CAPI_ABI_VERSION
against lrvx_capi_abi_version() once at startup and refuse the mismatch":
the structs on that boundary carry no reserved tail, so a header/library
skew shows up as wrong numbers rather than a failed load. No binding did
the comparison.

The refusal itself is pinned in C++ (tests/test_capi_abi_check.cpp), which
can force a wrong number. What Python can observe is that the check has
something to compare: the version the extension was compiled against and
the version the library reports, both exposed, and equal.
"""
from __future__ import annotations

import re
from pathlib import Path

import lrvx


def test_the_compiled_against_abi_version_is_exposed() -> None:
    assert isinstance(lrvx.CAPI_ABI_VERSION, int)
    assert lrvx.CAPI_ABI_VERSION > 0


def test_the_runtime_abi_version_is_exposed() -> None:
    assert isinstance(lrvx.capi_abi_version(), int)
    assert lrvx.capi_abi_version() > 0


def test_the_two_abi_versions_agree() -> None:
    assert lrvx.capi_abi_version() == lrvx.CAPI_ABI_VERSION, (
        "the extension was built against C ABI version "
        f"{lrvx.CAPI_ABI_VERSION} but links a library reporting "
        f"{lrvx.capi_abi_version()}"
    )


# ── The check has to run, not merely be available ─────────────────────

# Both numbers above are read from the same macro and the same C function
# whether or not the module init ever compared them, so their equality
# cannot tell a binding that checks from one that does not. The comparison
# happens once, in PYBIND11_MODULE, before anything else is bound -- and
# an import that finds a skew has to fail, not warn. Reading the init
# source is the only place that distinction is visible from here: a
# mismatch cannot be constructed at runtime, because the extension carries
# its own copy of the C API and there is no second library for it to
# disagree with.

REPO_ROOT = Path(__file__).resolve().parents[2]


def _module_init_source() -> str:
    return (REPO_ROOT / "python" / "lrvx.cpp").read_text()


def test_the_module_init_compares_the_two_versions() -> None:
    source = _module_init_source()
    assert "checkAbiVersion(LRVX_CAPI_ABI_VERSION" in source, (
        "python/lrvx.cpp no longer runs the shared ABI check at import; "
        "CAPI_ABI_VERSION and capi_abi_version() would still agree"
    )


def test_a_mismatch_fails_the_import() -> None:
    source = _module_init_source()
    at = source.index("checkAbiVersion(LRVX_CAPI_ABI_VERSION")
    following = source[at:at + 400]
    assert "throw" in following, (
        "the ABI check runs but nothing refuses the import:\n" + following
    )
    assert "import_error" in following, (
        "the refusal is not an ImportError, so `import lrvx` succeeds anyway:\n"
        + following
    )


# ── Codon ─────────────────────────────────────────────────────────────

# Codon links a prebuilt liblrvx against declarations generated from a
# possibly different header -- the case lrvx_capi.h warns "produces wrong
# numbers rather than a failed load", and the one binding where nothing
# else would notice. Each module is imported on its own (importing
# lrvx.backtest does not execute lrvx/__init__.codon), so the check has to
# sit in every module that declares C functions, not in one place. There
# is no Codon toolchain here to run them, so this reads the modules.


def test_every_codon_module_with_c_declarations_runs_the_abi_check() -> None:
    codon_dir = REPO_ROOT / "codon" / "lrvx"
    modules = sorted(codon_dir.glob("*.codon"))
    assert modules, f"no Codon modules under {codon_dir}"

    missing = []
    checked = []
    for module in modules:
        source = module.read_text()
        if "from C import" not in source:
            continue
        if module.name == "abi.codon":
            continue
        checked.append(module.name)
        if "from lrvx.abi import check_abi_version" not in source:
            missing.append(module.name)

    assert checked, "no Codon module declares C functions any more"
    assert not missing, (
        "Codon modules that declare C functions without importing the ABI "
        f"check: {', '.join(missing)}"
    )


def test_the_codon_binding_mirrors_the_header_abi_number() -> None:
    """abi.codon carries the number by hand, so a bump in the header that
    forgets it makes every Codon example exit at startup against the
    library built from that same header."""
    header = (REPO_ROOT / "include" / "lrvx" / "capi" / "lrvx_capi.h").read_text()
    codon = (REPO_ROOT / "codon" / "lrvx" / "abi.codon").read_text()
    in_header = re.search(r"^#define LRVX_CAPI_ABI_VERSION (\d+)$", header, re.M)
    in_codon = re.search(r"^LRVX_CAPI_ABI_VERSION: int = (\d+)$", codon, re.M)
    assert in_header and in_codon
    assert int(in_codon.group(1)) == int(in_header.group(1)), (
        f"codon/lrvx/abi.codon says {in_codon.group(1)} but lrvx_capi.h says "
        f"{in_header.group(1)}"
    )
