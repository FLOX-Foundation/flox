"""The Python binding must check the C ABI version at import.

``include/flox/capi/flox_capi.h`` says to "compare FLOX_CAPI_ABI_VERSION
against flox_capi_abi_version() once at startup and refuse the mismatch":
the structs on that boundary carry no reserved tail, so a header/library
skew shows up as wrong numbers rather than a failed load. No binding did
the comparison.

The refusal itself is pinned in C++ (tests/test_capi_abi_check.cpp), which
can force a wrong number. What Python can observe is that the check has
something to compare: the version the extension was compiled against and
the version the library reports, both exposed, and equal.
"""
from __future__ import annotations

import flox_py as flox


def test_the_compiled_against_abi_version_is_exposed() -> None:
    assert isinstance(flox.CAPI_ABI_VERSION, int)
    assert flox.CAPI_ABI_VERSION > 0


def test_the_runtime_abi_version_is_exposed() -> None:
    assert isinstance(flox.capi_abi_version(), int)
    assert flox.capi_abi_version() > 0


def test_the_two_abi_versions_agree() -> None:
    assert flox.capi_abi_version() == flox.CAPI_ABI_VERSION, (
        "the extension was built against C ABI version "
        f"{flox.CAPI_ABI_VERSION} but links a library reporting "
        f"{flox.capi_abi_version()}"
    )
