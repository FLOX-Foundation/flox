"""Tests for the llms.txt-style markdown emitter."""
from __future__ import annotations

from lrvx_codegen import emit_llms, ir


def test_emit_minimal():
    m = ir.Module(
        handles=[ir.HandleTypedef(name="LrvxXHandle")],
        enums=[ir.Enum(name="LrvxKind",
                       values=(ir.EnumValue("LRVX_A", 0),
                               ir.EnumValue("LRVX_B", 1)))],
        functions=[
            ir.Function(name="lrvx_a", return_type="void", params=(),
                        annotations={"group": "g"})
        ],
    )
    text = emit_llms.emit(m)
    assert "# lrvx C API Reference" in text
    assert "`LrvxXHandle`" in text
    assert "## Enums" in text
    assert "`LRVX_A` = `0`" in text
    assert "## Functions" in text


def test_handle_alias_marked():
    m = ir.Module(
        handles=[
            ir.HandleTypedef(name="LrvxXHandle"),
            ir.HandleTypedef(name="LrvxYHandle", alias_of="LrvxXHandle"),
        ],
    )
    text = emit_llms.emit(m)
    assert "alias of `LrvxXHandle`" in text


def test_struct_fields_table():
    m = ir.Module(
        structs=[
            ir.Struct(
                name="LrvxRecord",
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
                name="lrvx_x", return_type="int",
                params=(ir.Param("a", "double"), ir.Param("b", "size_t")),
                annotations={"group": "g"},
            )
        ]
    )
    text = emit_llms.emit(m)
    assert "`int lrvx_x(double a, size_t b)`" in text


def test_macro_constants_listed_by_group():
    m = ir.Module(
        macros=[
            ir.MacroConstant(name="LRVX_A_ONE", value="0", group="alpha"),
            ir.MacroConstant(name="LRVX_A_TWO", value="1", group="alpha"),
        ]
    )
    text = emit_llms.emit(m)
    assert "## Macro constants" in text
    assert "### alpha" in text
    assert "`LRVX_A_ONE` = `0`" in text
    assert "`LRVX_A_TWO` = `1`" in text


def test_no_macros_omits_macro_section():
    m = ir.Module(
        functions=[
            ir.Function(name="lrvx_a", return_type="void", params=(),
                        annotations={"group": "g"})
        ],
    )
    text = emit_llms.emit(m)
    assert "Macro constants" not in text
