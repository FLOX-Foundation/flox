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


# check() used to index function
# declarations only -- parsed without PARSE_DETAILED_PROCESSING_RECORD, so
# macros never entered libclang's AST, and struct fields were never
# visited at all. Deleting every FLOX_SIGNAL_TYPE_* constant, or slipping
# an extra field into FloxSignal, passed --require-full-coverage clean.
# These tests pin macro and struct-field comparison down directly.


def test_macro_value_mismatch_is_caught(tmp_path):
    a = _write_header(tmp_path / "a.h", "#define FLOX_SIGNAL_TYPE_LIMIT 1\n")
    b = _write_header(tmp_path / "b.h", "#define FLOX_SIGNAL_TYPE_LIMIT 2\n")
    mismatches, missing, extra = check_signatures.check(
        expected_header=a, actual_header=b
    )
    assert len(mismatches) == 1
    assert mismatches[0].reason == "macro-value"
    assert mismatches[0].name == "FLOX_SIGNAL_TYPE_LIMIT"


def test_macro_missing_from_actual_is_informational(tmp_path):
    a = _write_header(
        tmp_path / "a.h",
        "#define FLOX_SOME_CONST 0\n#define FLOX_OTHER_CONST 1\n",
    )
    b = _write_header(tmp_path / "b.h", "#define FLOX_SOME_CONST 0\n")
    mismatches, missing, extra = check_signatures.check(
        expected_header=a, actual_header=b
    )
    assert mismatches == []
    assert missing == ["FLOX_OTHER_CONST"]


def test_macros_pulled_in_via_include_are_not_compared(tmp_path):
    # stdint.h / stddef.h macros must not leak into the diff just because
    # PARSE_DETAILED_PROCESSING_RECORD makes libclang see them too.
    a = _write_header(tmp_path / "a.h", "int32_t flox_x(int32_t y);\n")
    b = _write_header(tmp_path / "b.h", "int32_t flox_x(int32_t y);\n")
    mismatches, missing, extra = check_signatures.check(
        expected_header=a, actual_header=b
    )
    assert mismatches == []
    assert missing == []
    assert extra == []


def test_struct_field_count_mismatch_is_caught(tmp_path):
    a = _write_header(
        tmp_path / "a.h",
        "typedef struct { uint64_t order_id; uint8_t side; } FloxSignal;\n",
    )
    b = _write_header(
        tmp_path / "b.h",
        "typedef struct { uint64_t order_id; uint8_t side; double extra; } "
        "FloxSignal;\n",
    )
    mismatches, missing, extra = check_signatures.check(
        expected_header=a, actual_header=b
    )
    assert len(mismatches) == 1
    assert mismatches[0].reason == "struct-field-count"
    assert mismatches[0].name == "FloxSignal"


def test_struct_field_type_mismatch_is_caught(tmp_path):
    a = _write_header(
        tmp_path / "a.h",
        "typedef struct { uint64_t order_id; uint8_t side; } FloxSignal;\n",
    )
    b = _write_header(
        tmp_path / "b.h",
        "typedef struct { uint64_t order_id; uint32_t side; } FloxSignal;\n",
    )
    mismatches, missing, extra = check_signatures.check(
        expected_header=a, actual_header=b
    )
    assert len(mismatches) == 1
    assert mismatches[0].reason == "struct-field"
    assert mismatches[0].name == "FloxSignal"


def test_matching_struct_and_macro_is_clean(tmp_path):
    body = (
        "#define FLOX_SIGNAL_TYPE_MARKET 0\n"
        "typedef struct { uint64_t order_id; uint8_t side; } FloxSignal;\n"
    )
    a = _write_header(tmp_path / "a.h", body)
    b = _write_header(tmp_path / "b.h", body)
    mismatches, missing, extra = check_signatures.check(
        expected_header=a, actual_header=b
    )
    assert mismatches == []
    assert missing == []
    assert extra == []


def test_known_missing_macros_exemption_does_not_hide_a_value_mismatch(tmp_path):
    # The exemption must only suppress "absent from actual"; a macro that
    # IS present in actual, with the wrong value, is a real regression and
    # must still fail even if its name is in KNOWN_MISSING_MACROS.
    name = next(iter(check_signatures.KNOWN_MISSING_MACROS))
    a = _write_header(tmp_path / "a.h", f"#define {name} 0\n")
    b = _write_header(tmp_path / "b.h", f"#define {name} 99\n")
    mismatches, missing, extra = check_signatures.check(
        expected_header=a, actual_header=b
    )
    assert len(mismatches) == 1
    assert mismatches[0].reason == "macro-value"
    assert mismatches[0].name == name
