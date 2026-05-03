"""Tests for the llms.txt-style markdown emitter."""
from __future__ import annotations

from flox_codegen import emit_llms, ir


def test_emit_minimal():
    m = ir.Module(
        handles=[ir.HandleTypedef(name="FloxXHandle")],
        enums=[ir.Enum(name="FloxKind",
                       values=(ir.EnumValue("FLOX_A", 0),
                               ir.EnumValue("FLOX_B", 1)))],
        functions=[
            ir.Function(name="flox_a", return_type="void", params=(),
                        annotations={"group": "g"})
        ],
    )
    text = emit_llms.emit(m)
    assert "# FLOX C API Reference" in text
    assert "`FloxXHandle`" in text
    assert "## Enums" in text
    assert "`FLOX_A` = `0`" in text
    assert "## Functions" in text


def test_handle_alias_marked():
    m = ir.Module(
        handles=[
            ir.HandleTypedef(name="FloxXHandle"),
            ir.HandleTypedef(name="FloxYHandle", alias_of="FloxXHandle"),
        ],
    )
    text = emit_llms.emit(m)
    assert "alias of `FloxXHandle`" in text


def test_struct_fields_table():
    m = ir.Module(
        structs=[
            ir.Struct(
                name="FloxRecord",
                fields=(ir.StructField("x", "int32_t"),
                        ir.StructField("y", "double")),
            )
        ]
    )
    text = emit_llms.emit(m)
    assert "| `x` | `int32_t` |" in text
    assert "| `y` | `double` |" in text


def test_function_signature_format():
    m = ir.Module(
        functions=[
            ir.Function(
                name="flox_x", return_type="int",
                params=(ir.Param("a", "double"), ir.Param("b", "size_t")),
                annotations={"group": "g"},
            )
        ]
    )
    text = emit_llms.emit(m)
    assert "`int flox_x(double a, size_t b)`" in text
