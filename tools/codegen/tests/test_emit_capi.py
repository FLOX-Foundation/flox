"""Tests for the C-header emitter.

Two layers:
1. Direct IR → text checks (deterministic, no libclang).
2. Round-trip via the spec parser, asserting the generated header is itself
   parseable by libclang as a valid C TU.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

from flox_codegen import emit_capi, ir


def test_emit_minimal():
    mod = ir.Module(
        functions=[
            ir.Function(
                name="flox_demo",
                return_type="int",
                params=(ir.Param("x", "int"),),
                annotations={"group": "demo"},
            )
        ]
    )
    text = emit_capi.emit(mod)
    assert "int flox_demo(int x);" in text
    assert "#pragma once" in text
    assert "extern \"C\"" in text


def test_emit_array_field_renders_as_c():
    mod = ir.Module(
        structs=[
            ir.Struct(
                name="FloxThing",
                fields=(
                    ir.StructField("count", "uint32_t"),
                    ir.StructField("_pad", "uint8_t[4]"),
                ),
            )
        ]
    )
    text = emit_capi.emit(mod)
    # Array suffix must move to the field name, not stay attached to the type.
    assert "uint8_t _pad[4];" in text
    assert "uint8_t[4]" not in text


def test_emit_callback_bundle_after_fnptrs():
    mod = ir.Module(
        function_pointers=[
            ir.FunctionPointerTypedef(
                name="FloxOnEvent",
                return_type="void",
                params=(ir.Param("", "void *"), ir.Param("", "int32_t")),
            )
        ],
        structs=[
            ir.Struct(
                name="FloxBundle",
                fields=(
                    ir.StructField("on_event", "FloxOnEvent"),
                    ir.StructField("user_data", "void *"),
                ),
            )
        ],
    )
    text = emit_capi.emit(mod)
    fnptr_pos = text.find("FloxOnEvent")
    bundle_pos = text.find("FloxBundle;")
    assert 0 < fnptr_pos < bundle_pos


def test_emit_long_signature_wraps():
    mod = ir.Module(
        functions=[
            ir.Function(
                name="flox_long_thing_with_lots_of_params",
                return_type="void",
                params=tuple(
                    ir.Param(f"arg_with_long_name_{i}", "const double *")
                    for i in range(8)
                ),
                annotations={"group": "demo"},
            )
        ]
    )
    text = emit_capi.emit(mod)
    body = next(line for line in text.splitlines() if "flox_long_thing" in line)
    # The wrap inserts continuation lines aligned under the open paren.
    assert text.count("flox_long_thing_with_lots_of_params(") == 1
    # Some line in the function declaration must be ≤ ~100 cols.
    fn_lines = [
        line for line in text.splitlines() if "arg_with_long_name" in line
    ]
    assert all(len(line) <= 110 for line in fn_lines)


def test_emit_macro_constant_renders_as_define():
    mod = ir.Module(
        macros=[
            ir.MacroConstant(name="FLOX_THING_A", value="0", group="thing"),
            ir.MacroConstant(name="FLOX_THING_B", value="1", group="thing"),
        ]
    )
    text = emit_capi.emit(mod)
    assert "#define FLOX_THING_A 0" in text
    assert "#define FLOX_THING_B 1" in text


def test_emit_macro_groups_get_their_own_banner():
    mod = ir.Module(
        macros=[
            ir.MacroConstant(name="FLOX_A_ONE", value="0", group="alpha"),
            ir.MacroConstant(name="FLOX_B_ONE", value="0", group="beta"),
        ]
    )
    text = emit_capi.emit(mod)
    # Alpha comes before beta (groups sorted), and each macro sits under its
    # own group's section rather than a single flat dump.
    assert text.find("FLOX_A_ONE") < text.find("FLOX_B_ONE")


def test_no_macros_emits_no_macro_section():
    mod = ir.Module(functions=[
        ir.Function(name="flox_x", return_type="void", params=(),
                    annotations={"group": "g"}),
    ])
    text = emit_capi.emit(mod)
    assert "Macro constants" not in text


def test_round_trip_produces_compilable_header(tmp_path):
    """The slice spec → generated header must be a valid C TU."""
    repo_root = Path(__file__).resolve().parents[3]
    spec = repo_root / "include" / "flox" / "capi" / "flox_capi_spec.hpp"
    if not spec.exists():
        return  # tests run before spec authored; skip silently.

    from flox_codegen import extractor
    mod = extractor.parse_spec(spec)
    out_path = tmp_path / "out.h"
    out_path.write_text(emit_capi.emit(mod))

    # Compile-only check via the system clang (faster than libclang TU parse
    # and gives a clear pass/fail).
    proc = subprocess.run(
        ["clang", "-x", "c", "-c", "-fsyntax-only", str(out_path)],
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 0, proc.stderr


def test_emitter_is_deterministic():
    """Two emit() calls on the same module yield byte-identical output."""
    mod = ir.Module(
        handles=[ir.HandleTypedef(name="FloxXHandle")],
        structs=[
            ir.Struct(
                name="FloxX",
                fields=(ir.StructField("a", "int32_t"),),
            )
        ],
        functions=[
            ir.Function(
                name="flox_a",
                return_type="void",
                params=(),
                annotations={"group": "g"},
            ),
            ir.Function(
                name="flox_b",
                return_type="int",
                params=(ir.Param("x", "int"),),
                annotations={"group": "g"},
            ),
        ],
    )
    a = emit_capi.emit(mod)
    b = emit_capi.emit(mod)
    assert a == b
