"""Unit tests for the struct-layout emitter.

The layout pass has one job the rest of the codegen does not: it must agree
with what the C compiler does to a struct. tests/test_capi_event_layout.cpp
is the end of that check -- it compares the emitted table against offsetof on
a real build. These tests cover the pass itself: padding, nesting, arrays,
and the shape of the two artifacts.
"""
from __future__ import annotations

import pytest

from flox_codegen import emit_layout, ir


_NAMES = ("FloxTradeData", "FloxBookSnapshot", "FloxSymbolContext")


def _module() -> ir.Module:
    return ir.Module(structs=[
        ir.Struct("FloxTradeData", (
            ir.StructField("symbol", "uint32_t"),
            ir.StructField("price_raw", "int64_t"),
            ir.StructField("is_buy", "uint8_t"),
        )),
        ir.Struct("FloxBookSnapshot", (
            ir.StructField("bid_price_raw", "int64_t"),
        )),
        ir.Struct("FloxSymbolContext", (
            ir.StructField("symbol_id", "uint32_t"),
            ir.StructField("_pad", "uint8_t[2]"),
            ir.StructField("book", "FloxBookSnapshot"),
        )),
    ])


def _layouts(names=_NAMES):
    return {s.name: s for s in emit_layout.compute(_module(), names)}


def test_a_field_starts_at_its_own_alignment():
    trade = _layouts()["FloxTradeData"]
    offsets = {f.name: f.offset for f in trade.fields}
    assert offsets == {"symbol": 0, "price_raw": 8, "is_buy": 16}


def test_a_struct_is_padded_out_to_its_widest_member():
    trade = _layouts()["FloxTradeData"]
    assert (trade.size, trade.alignment) == (24, 8)


def test_a_nested_struct_carries_its_own_alignment():
    ctx = _layouts()["FloxSymbolContext"]
    offsets = {f.name: f.offset for f in ctx.fields}
    # symbol_id 0..4, _pad 4..6, then the nested struct at the next multiple
    # of 8. Padding is laid out but not exported: nothing may read it.
    assert offsets == {"symbol_id": 0, "book": 8}
    assert ctx.size == 16


def test_an_unknown_field_type_is_an_error_not_a_guess():
    module = ir.Module(structs=[
        ir.Struct("FloxOdd", (ir.StructField("x", "long double"),))])
    with pytest.raises(emit_layout.LayoutError):
        emit_layout.compute(module, ("FloxOdd",))


def test_the_header_carries_both_tables():
    text = emit_layout.emit_header(_module(), format=False, names=_NAMES)
    assert "kFloxEventLayout" in text
    assert "kFloxEventStructLayout" in text
    assert '{"FloxTradeData", "price_raw", 8, 8},' in text
    assert '{"FloxTradeData", 24, 8},' in text
    assert "_pad" not in text


def test_the_codon_module_uses_the_names_the_binding_imports():
    text = emit_layout.emit_codon(_module(), _NAMES)
    assert "_TRADE_PRICE_RAW = 8" in text
    assert "_CTX_SYMBOL_ID = 0" in text
    assert "_TRADE_SIZE = 24" in text


def test_every_layout_struct_has_a_codon_prefix():
    missing = [n for n in emit_layout.LAYOUT_STRUCTS
               if n not in emit_layout._CODON_PREFIX]
    assert not missing, f"no Codon constant prefix for {missing}"


def test_an_array_field_occupies_every_one_of_its_elements():
    """Nothing on the real event boundary has a non-padding array field: the
    only arrays are `_pad` members, and mis-sizing one is absorbed by the
    alignment of the field behind it, so the shipped tables cannot show this
    rule holding. A synthetic struct can -- `tag` has to push `tail` six
    bytes, not one."""
    module = ir.Module(structs=[
        ir.Struct("FloxArrayed", (
            ir.StructField("head", "uint32_t"),
            ir.StructField("tag", "uint8_t[6]"),
            ir.StructField("tail", "uint16_t"),
        ))])
    layout = emit_layout.compute(module, ("FloxArrayed",))[0]

    assert {f.name: f.offset for f in layout.fields} == {
        "head": 0, "tag": 4, "tail": 10}
    assert {f.name: f.size for f in layout.fields}["tag"] == 6
    assert (layout.size, layout.alignment) == (12, 4)


def test_an_array_of_structs_is_sized_by_element_too():
    module = ir.Module(structs=[
        ir.Struct("FloxPair", (
            ir.StructField("a", "int32_t"),
            ir.StructField("b", "int32_t"),
        )),
        ir.Struct("FloxHolder", (
            ir.StructField("pairs", "FloxPair[3]"),
            ir.StructField("count", "uint32_t"),
        ))])
    layout = {s.name: s for s in emit_layout.compute(
        module, ("FloxPair", "FloxHolder"))}["FloxHolder"]

    assert {f.name: f.offset for f in layout.fields} == {"pairs": 0, "count": 24}
    assert layout.size == 28
