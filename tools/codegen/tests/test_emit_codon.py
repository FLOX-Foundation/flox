"""Tests for the Codon FFI emitter."""
from __future__ import annotations

from flox_codegen import emit_codon, ir


def _module(*funcs: ir.Function, **kw) -> ir.Module:
    return ir.Module(functions=list(funcs), **kw)


def test_void_return_no_arrow():
    m = _module(
        ir.Function(name="flox_x", return_type="void", params=(),
                    annotations={"group": "g"})
    )
    text = emit_codon.emit(m)
    assert "from C import flox_x()" in text
    assert "-> " not in text  # void return → no arrow


def test_int_return_arrow():
    m = _module(
        ir.Function(name="flox_x", return_type="int32_t", params=(),
                    annotations={"group": "g"})
    )
    text = emit_codon.emit(m)
    assert "from C import flox_x() -> i32" in text


def test_pointer_to_scalar():
    m = _module(
        ir.Function(
            name="flox_x", return_type="void",
            params=(
                ir.Param("input", "const double *"),
                ir.Param("len", "size_t"),
                ir.Param("output", "double *"),
            ),
            annotations={"group": "g"},
        )
    )
    text = emit_codon.emit(m)
    assert "from C import flox_x(Ptr[float], int, Ptr[float])" in text


def test_handle_and_string_collapse_to_cobj():
    m = _module(
        ir.Function(
            name="flox_x", return_type="void",
            params=(
                ir.Param("h", "FloxStrategyHandle"),
                ir.Param("name", "const char *"),
            ),
            annotations={"group": "g"},
        )
    )
    text = emit_codon.emit(m)
    assert "from C import flox_x(cobj, cobj)" in text


def test_pointer_to_pointer():
    """const char* const* maps to Ptr[cobj]."""
    m = _module(
        ir.Function(
            name="flox_x", return_type="void",
            params=(ir.Param("deps", "const char *const *"),),
            annotations={"group": "g"},
        )
    )
    text = emit_codon.emit(m)
    assert "Ptr[cobj]" in text


def test_grouping_alphabetical():
    m = _module(
        ir.Function(name="flox_b", return_type="void", params=(),
                    annotations={"group": "zebra"}),
        ir.Function(name="flox_a", return_type="void", params=(),
                    annotations={"group": "alpha"}),
    )
    text = emit_codon.emit(m)
    alpha_pos = text.find("alpha")
    zebra_pos = text.find("zebra")
    assert 0 < alpha_pos < zebra_pos


def test_unsigned_widths():
    m = _module(
        ir.Function(
            name="flox_x", return_type="uint64_t",
            params=(
                ir.Param("a", "uint8_t"),
                ir.Param("b", "uint16_t"),
                ir.Param("c", "uint32_t"),
                ir.Param("d", "uint64_t"),
            ),
            annotations={"group": "g"},
        )
    )
    text = emit_codon.emit(m)
    assert "from C import flox_x(u8, u16, u32, u64) -> u64" in text


def test_double_returns_codon_float():
    m = _module(
        ir.Function(name="flox_x", return_type="double", params=(),
                    annotations={"group": "g"})
    )
    text = emit_codon.emit(m)
    assert "-> float" in text


def test_map_type_void_pointer():
    assert emit_codon.map_type("void *") == "cobj"


def test_map_type_const_pointer():
    assert emit_codon.map_type("const double *") == "Ptr[float]"


def test_map_type_unknown_passthrough():
    # Unknown type keeps the literal — useful for noticing gaps.
    assert emit_codon.map_type("__weird_type") == "__weird_type"
