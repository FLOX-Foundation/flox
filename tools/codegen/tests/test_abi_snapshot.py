"""Tests for the ABI snapshot serializer + diff classifier."""
from __future__ import annotations

from pathlib import Path

from lrvx_codegen import abi_snapshot, check_signatures


def _sig(name, ret, *params):
    return check_signatures.FuncSig(
        name=name, return_type=ret, param_types=tuple(params)
    )


def test_render_parse_round_trip():
    sigs = {
        "lrvx_a": _sig("lrvx_a", "void", "int"),
        "lrvx_b": _sig("lrvx_b", "double", "const double *", "size_t"),
    }
    text = abi_snapshot.render(sigs)
    parsed = abi_snapshot.parse(text)
    assert parsed == sigs


def test_render_is_sorted():
    sigs = {
        "lrvx_z": _sig("lrvx_z", "void"),
        "lrvx_a": _sig("lrvx_a", "void"),
        "lrvx_m": _sig("lrvx_m", "void"),
    }
    text = abi_snapshot.render(sigs)
    body = [l for l in text.splitlines() if l and not l.startswith("#")]
    assert body == ["lrvx_a|void", "lrvx_m|void", "lrvx_z|void"]


def test_render_is_deterministic():
    sigs = {
        "lrvx_b": _sig("lrvx_b", "void"),
        "lrvx_a": _sig("lrvx_a", "void"),
    }
    a = abi_snapshot.render(sigs)
    b = abi_snapshot.render(sigs)
    assert a == b


def test_diff_added_only_not_breaking():
    old = {"a": _sig("a", "void")}
    new = {"a": _sig("a", "void"), "b": _sig("b", "void")}
    d = abi_snapshot.diff(old, new)
    assert d.added == ["b"]
    assert d.removed == []
    assert d.changed == []
    assert not abi_snapshot.is_breaking(d)


def test_diff_removed_is_breaking():
    old = {"a": _sig("a", "void"), "b": _sig("b", "void")}
    new = {"a": _sig("a", "void")}
    d = abi_snapshot.diff(old, new)
    assert d.removed == ["b"]
    assert abi_snapshot.is_breaking(d)


def test_diff_signature_changed_is_breaking():
    old = {"a": _sig("a", "void", "int")}
    new = {"a": _sig("a", "void", "long")}
    d = abi_snapshot.diff(old, new)
    assert d.changed == ["a"]
    assert abi_snapshot.is_breaking(d)


def test_diff_return_type_changed_is_breaking():
    old = {"a": _sig("a", "void")}
    new = {"a": _sig("a", "int")}
    d = abi_snapshot.diff(old, new)
    assert d.changed == ["a"]
    assert abi_snapshot.is_breaking(d)


def test_diff_unchanged_is_clean():
    sigs = {"a": _sig("a", "void"), "b": _sig("b", "int", "double")}
    d = abi_snapshot.diff(sigs, sigs)
    assert d.added == []
    assert d.removed == []
    assert d.changed == []
    assert not abi_snapshot.is_breaking(d)


def test_parse_skips_blank_and_comment_lines():
    text = """\
# header comment
# another

lrvx_a|void

# trailing
lrvx_b|int|double
"""
    parsed = abi_snapshot.parse(text)
    assert set(parsed) == {"lrvx_a", "lrvx_b"}
    assert parsed["lrvx_b"].param_types == ("double",)


def test_parse_rejects_malformed():
    import pytest
    with pytest.raises(ValueError):
        abi_snapshot.parse("only_one_field\n")


def test_from_header_returns_funcsigs(tmp_path):
    h = tmp_path / "sample.h"
    h.write_text(
        "#include <stdint.h>\nint32_t lrvx_x(int32_t y);\nvoid lrvx_y(double *out);\n"
    )
    sigs = abi_snapshot.from_header(h)
    assert set(sigs) == {"lrvx_x", "lrvx_y"}
    # We preserve typedef names (so `int32_t` stays `int32_t`) instead of
    # canonicalizing to platform-specific underlying types.
    assert sigs["lrvx_x"].return_type == "int32_t"
