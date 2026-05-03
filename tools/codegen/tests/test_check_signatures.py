"""Signature-diff tests."""
from __future__ import annotations

from pathlib import Path

from flox_codegen import check_signatures


def _write_header(p: Path, body: str) -> Path:
    p.write_text(
        "#pragma once\n#include <stdint.h>\n#include <stddef.h>\n" + body
    )
    return p


def test_signature_match(tmp_path):
    a = _write_header(
        tmp_path / "a.h",
        "int32_t flox_x(int32_t y);\nvoid flox_y(double *out);\n",
    )
    b = _write_header(
        tmp_path / "b.h",
        # Same shapes, different param names — equivalence is by type only.
        "int32_t flox_x(int32_t z);\nvoid flox_y(double *result);\n",
    )
    mismatches, missing, extra = check_signatures.check(
        expected_header=a, actual_header=b
    )
    assert mismatches == []
    assert missing == []
    assert extra == []


def test_arity_mismatch(tmp_path):
    a = _write_header(tmp_path / "a.h", "int32_t flox_x(int32_t y);\n")
    b = _write_header(tmp_path / "b.h", "int32_t flox_x(int32_t y, int32_t z);\n")
    mismatches, missing, extra = check_signatures.check(
        expected_header=a, actual_header=b
    )
    assert len(mismatches) == 1
    assert mismatches[0].reason == "arity"


def test_return_type_mismatch(tmp_path):
    a = _write_header(tmp_path / "a.h", "int32_t flox_x(void);\n")
    b = _write_header(tmp_path / "b.h", "int64_t flox_x(void);\n")
    mismatches, missing, extra = check_signatures.check(
        expected_header=a, actual_header=b
    )
    assert len(mismatches) == 1
    assert mismatches[0].reason == "return-type"


def test_param_type_mismatch(tmp_path):
    a = _write_header(tmp_path / "a.h", "void flox_x(const double *p);\n")
    b = _write_header(tmp_path / "b.h", "void flox_x(double *p);\n")  # missing const
    mismatches, missing, extra = check_signatures.check(
        expected_header=a, actual_header=b
    )
    assert len(mismatches) == 1
    assert mismatches[0].reason == "param-type"


def test_missing_function_is_informational(tmp_path):
    a = _write_header(tmp_path / "a.h", "void flox_a(void);\nvoid flox_b(void);\n")
    b = _write_header(tmp_path / "b.h", "void flox_a(void);\n")
    mismatches, missing, extra = check_signatures.check(
        expected_header=a, actual_header=b
    )
    assert mismatches == []
    assert missing == ["flox_b"]
    assert extra == []


def test_extra_function_is_informational(tmp_path):
    a = _write_header(tmp_path / "a.h", "void flox_a(void);\n")
    b = _write_header(tmp_path / "b.h", "void flox_a(void);\nvoid flox_b(void);\n")
    mismatches, missing, extra = check_signatures.check(
        expected_header=a, actual_header=b
    )
    assert mismatches == []
    assert missing == []
    assert extra == ["flox_b"]
