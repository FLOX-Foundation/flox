"""The Codon binding reads C event structs by byte offset. Pin the offsets.

codon/flox/dispatch.codon reaches into FloxTradeData, FloxBarData,
FloxOrderEventData and FloxSymbolContext with integer literals written by
hand, and codon/flox/strategy.codon does the same for FloxBar. Nothing
connects those literals to the header: adding a field, reordering two, or
changing one's width moves every offset after it, and the only thing that
would notice is a smoke example that has to be run with a built Codon
toolchain. `reject_reason` has no constant at all, so a Codon strategy
cannot read why an order was rejected.

The fix these tests describe: generate the layout from the C ABI into
`include/flox/capi/flox_capi_layout.h` (a table a C++ test compares against
`offsetof` -- see tests/test_capi_event_layout.cpp) and into
`codon/flox/layout.codon` (the constants the Codon binding imports), so the
two cannot drift apart and neither can drift from the header.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LAYOUT_HEADER = REPO / "include" / "flox" / "capi" / "flox_capi_layout.h"
LAYOUT_CODON = REPO / "codon" / "flox" / "layout.codon"
DISPATCH = REPO / "codon" / "flox" / "dispatch.codon"
STRATEGY = REPO / "codon" / "flox" / "strategy.codon"

# Codon constant -> (C struct, field). The names on the left are the ones
# codon/flox/*.codon already uses, so the generated module has to keep them;
# _EV_REJECT_REASON is the one that was missing.
CONSTANTS: dict[str, tuple[str, str]] = {
    "_CTX_SYMBOL_ID": ("FloxSymbolContext", "symbol_id"),

    "_TRADE_SYMBOL": ("FloxTradeData", "symbol"),
    "_TRADE_PRICE_RAW": ("FloxTradeData", "price_raw"),
    "_TRADE_QTY_RAW": ("FloxTradeData", "quantity_raw"),
    "_TRADE_IS_BUY": ("FloxTradeData", "is_buy"),
    "_TRADE_TS_NS": ("FloxTradeData", "exchange_ts_ns"),

    "_BAR_SYMBOL": ("FloxBarData", "symbol"),
    "_BAR_TYPE": ("FloxBarData", "bar_type"),
    "_BAR_CLOSE_REASON": ("FloxBarData", "close_reason"),
    "_BAR_TYPE_PARAM": ("FloxBarData", "bar_type_param"),
    "_BAR_OPEN_RAW": ("FloxBarData", "open_raw"),
    "_BAR_HIGH_RAW": ("FloxBarData", "high_raw"),
    "_BAR_LOW_RAW": ("FloxBarData", "low_raw"),
    "_BAR_CLOSE_RAW": ("FloxBarData", "close_raw"),
    "_BAR_VOLUME_RAW": ("FloxBarData", "volume_raw"),
    "_BAR_BUY_VOLUME_RAW": ("FloxBarData", "buy_volume_raw"),
    "_BAR_START_NS": ("FloxBarData", "start_time_ns"),
    "_BAR_END_NS": ("FloxBarData", "end_time_ns"),

    "_EV_ORDER_ID": ("FloxOrderEventData", "order_id"),
    "_EV_SYMBOL_ID": ("FloxOrderEventData", "symbol_id"),
    "_EV_SIDE": ("FloxOrderEventData", "side"),
    "_EV_ORDER_TYPE": ("FloxOrderEventData", "order_type"),
    "_EV_STATUS": ("FloxOrderEventData", "status"),
    "_EV_FILL_QTY_RAW": ("FloxOrderEventData", "fill_qty_raw"),
    "_EV_FILL_PRICE_RAW": ("FloxOrderEventData", "fill_price_raw"),
    "_EV_TS_NS": ("FloxOrderEventData", "exchange_ts_ns"),
    "_EV_REJECT_REASON": ("FloxOrderEventData", "reject_reason"),
    "_EV_QUEUE_AHEAD_RAW": ("FloxOrderEventData", "queue_ahead_raw"),
    "_EV_QUEUE_TOTAL_RAW": ("FloxOrderEventData", "queue_total_raw"),
    "_EV_IS_MAKER": ("FloxOrderEventData", "is_maker"),
    "_EV_MARKET_POSITION": ("FloxOrderEventData", "market_position"),
    "_EV_DISTANCE_TICKS": ("FloxOrderEventData", "distance_to_best_ticks"),

    "_FLOXBAR_START_NS": ("FloxBar", "start_time_ns"),
    "_FLOXBAR_END_NS": ("FloxBar", "end_time_ns"),
    "_FLOXBAR_OPEN_RAW": ("FloxBar", "open_raw"),
    "_FLOXBAR_HIGH_RAW": ("FloxBar", "high_raw"),
    "_FLOXBAR_LOW_RAW": ("FloxBar", "low_raw"),
    "_FLOXBAR_CLOSE_RAW": ("FloxBar", "close_raw"),
    "_FLOXBAR_VOLUME_RAW": ("FloxBar", "volume_raw"),
    "_FLOXBAR_BUY_VOLUME_RAW": ("FloxBar", "buy_volume_raw"),
    "_FLOXBAR_TRADE_COUNT": ("FloxBar", "trade_count"),
}

# Struct size constants the Codon binding allocates buffers from.
SIZE_CONSTANTS: dict[str, str] = {
    "_FLOXBAR_SIZE": "FloxBar",
}

_FIELD_ENTRY = re.compile(
    r'\{\s*"([A-Za-z_0-9]+)"\s*,\s*"([A-Za-z_0-9]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}')
_STRUCT_ENTRY = re.compile(
    r'\{\s*"([A-Za-z_0-9]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}')
_CODON_CONST = re.compile(r"^([_A-Z][_A-Z0-9]*)\s*=\s*(\d+)\s*$", re.MULTILINE)


def _section(text: str, array: str) -> str:
    start = text.index(array)
    return text[start:text.index("};", start)]


def field_offsets() -> dict[tuple[str, str], int]:
    body = _section(LAYOUT_HEADER.read_text(), "kFloxEventLayout")
    return {(s, f): int(off) for s, f, off, _size in _FIELD_ENTRY.findall(body)}


def struct_sizes() -> dict[str, int]:
    body = _section(LAYOUT_HEADER.read_text(), "kFloxEventStructLayout")
    return {s: int(size) for s, size, _align in _STRUCT_ENTRY.findall(body)}


def codon_constants() -> dict[str, int]:
    return {name: int(value)
            for name, value in _CODON_CONST.findall(LAYOUT_CODON.read_text())}


def test_the_layout_header_covers_every_struct_codon_reads():
    assert LAYOUT_HEADER.exists(), f"{LAYOUT_HEADER} is missing"
    offsets = field_offsets()
    missing = [key for key in CONSTANTS.values() if key not in offsets]
    assert not missing, f"no layout entry for: {missing}"
    sizes = struct_sizes()
    for struct in SIZE_CONSTANTS.values():
        assert struct in sizes, f"no struct-size entry for {struct}"


def test_the_codon_layout_module_matches_the_generated_header():
    assert LAYOUT_CODON.exists(), f"{LAYOUT_CODON} is missing"
    offsets = field_offsets()
    sizes = struct_sizes()
    consts = codon_constants()

    for name, key in CONSTANTS.items():
        assert name in consts, f"{name} is not defined in {LAYOUT_CODON.name}"
        assert consts[name] == offsets[key], (
            f"{name} is {consts[name]}, header says {key[0]}.{key[1]} is at "
            f"{offsets[key]}")
    for name, struct in SIZE_CONSTANTS.items():
        assert name in consts, f"{name} is not defined in {LAYOUT_CODON.name}"
        assert consts[name] == sizes[struct]


def test_codon_can_read_the_rejection_reason():
    """FloxOrderEventData.reject_reason had no constant, so a Codon strategy
    had no way to reach the field the C ABI fills in for it."""
    assert LAYOUT_CODON.exists(), f"{LAYOUT_CODON} is missing"
    consts = codon_constants()
    assert "_EV_REJECT_REASON" in consts
    assert consts["_EV_REJECT_REASON"] == \
        field_offsets()[("FloxOrderEventData", "reject_reason")]
    assert "_EV_REJECT_REASON" in DISPATCH.read_text(), \
        "dispatch.codon does not use the constant it was given"


def test_no_binding_file_hardcodes_a_struct_offset():
    """The generated module is only worth having if the hand-written files
    stop carrying their own copies."""
    dispatch = DISPATCH.read_text()
    for name in CONSTANTS:
        assert not re.search(rf"^{name}\s*=\s*\d+", dispatch, re.MULTILINE), \
            f"{name} is still defined by hand in dispatch.codon"
    assert "from flox.layout import" in dispatch

    strategy = STRATEGY.read_text()
    assert not re.search(r"buf \+ \d", strategy), \
        "strategy.codon still reads FloxBar at literal offsets"
    assert not re.search(r"^_FLOXBAR_SIZE\s*=\s*\d+", strategy, re.MULTILINE), \
        "_FLOXBAR_SIZE is still defined by hand in strategy.codon"


# ── Second pass: the constant a call site actually uses ───────────────
#
# The tests above prove the constants exist and carry the right offsets, and
# that no file defines its own. They say nothing about which constant a given
# read passes: swapping _EV_REJECT_REASON for _EV_TS_NS at one call site
# leaves every generated artifact correct and every assertion above green,
# and hands the strategy eight bytes of timestamp as a string pointer.
#
# There is no Codon toolchain in this environment, so this is checked at the
# text level: each read is matched to the C type of the field its constant
# names, which is what an accessor is choosing between.

CODON_READERS = ("dispatch.codon", "strategy.codon", "tools.codon")

# Codon accessor -> the C field types it may be pointed at.
ACCESSOR_TYPES: dict[str, tuple[str, ...]] = {
    "_cstr_at": ("const char*",),
    "_u8_at": ("uint8_t",),
    "_u32_at": ("uint32_t",),
    "_i32_at": ("int32_t",),
    "_i64_at": ("int64_t",),
}

# Raw Ptr[...] read -> the same.
PTR_TYPES: dict[str, tuple[str, ...]] = {
    "u8": ("uint8_t",),
    "u32": ("uint32_t",),
    "u64": ("uint64_t",),
    "i32": ("int32_t",),
    "i64": ("int64_t",),
}

_ACCESSOR_CALL = re.compile(r"(_[a-z0-9]+_at)\(\s*\w+\s*,\s*(_[A-Z][A-Z_0-9]*)\s*\)")
_PTR_READ = re.compile(r"Ptr\[([a-z0-9]+)\]\(\s*\w+\s*\+\s*(_[A-Z][A-Z_0-9]*)\s*\)")

_STRUCT_BLOCK = re.compile(r"typedef struct\s*\{(.*?)\}\s*(Flox\w+);", re.DOTALL)
_FIELD_DECL = re.compile(
    r"^(?P<type>[A-Za-z_][\w ]*?[\w*])\s+(?P<field>[A-Za-z_]\w*)(?:\[\d+\])?$")


def _normalize_type(spelling: str) -> str:
    return " ".join(spelling.replace("*", " *").split()).replace(" *", "*")


def c_field_types() -> dict[tuple[str, str], str]:
    """(struct, field) -> C type spelling, read from the shipped header."""
    text = (REPO / "include" / "flox" / "capi" / "flox_capi.h").read_text()
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", " ", text)
    types: dict[tuple[str, str], str] = {}
    for body, struct in _STRUCT_BLOCK.findall(text):
        for decl in body.split(";"):
            m = _FIELD_DECL.match(" ".join(decl.split()))
            if m:
                types[(struct, m.group("field"))] = m.group("type")
    return types


def test_every_event_read_uses_a_constant_of_the_right_type():
    types = c_field_types()
    checked = 0
    for name in CODON_READERS:
        text = (REPO / "codon" / "flox" / name).read_text()
        for accessor, const in _ACCESSOR_CALL.findall(text):
            if const not in CONSTANTS:
                continue
            assert accessor in ACCESSOR_TYPES, f"unknown accessor {accessor} in {name}"
            struct, field = CONSTANTS[const]
            actual = _normalize_type(types[(struct, field)])
            assert actual in ACCESSOR_TYPES[accessor], (
                f"{name}: {accessor}(..., {const}) reads {struct}.{field}, "
                f"which is `{actual}`, not "
                f"{' or '.join(ACCESSOR_TYPES[accessor])}")
            checked += 1
        for width, const in _PTR_READ.findall(text):
            if const not in CONSTANTS:
                continue
            assert width in PTR_TYPES, f"unknown Ptr width {width} in {name}"
            struct, field = CONSTANTS[const]
            actual = _normalize_type(types[(struct, field)])
            assert actual in PTR_TYPES[width], (
                f"{name}: Ptr[{width}](... + {const}) reads {struct}.{field}, "
                f"which is `{actual}`")
            checked += 1
    assert checked >= 40, f"only {checked} reads matched; the scan drifted"


def test_the_order_event_builder_reads_the_rejection_reason_by_its_own_constant():
    """_EV_REJECT_REASON is the only constant that may reach the string read;
    every other one points at a number and would be handed to the strategy as
    a pointer."""
    text = (REPO / "codon" / "flox" / "dispatch.codon").read_text()
    body = text[text.index("def _build_order_event"):]
    body = body[:body.index("\ndef ", 1)]
    reads = dict((const, accessor) for accessor, const in _ACCESSOR_CALL.findall(body))
    assert reads.get("_EV_REJECT_REASON") == "_cstr_at", \
        "_build_order_event does not read reject_reason through _EV_REJECT_REASON"
    assert "_cstr_at" not in {a for c, a in reads.items() if c != "_EV_REJECT_REASON"}, \
        "a numeric field is being read as a string"
