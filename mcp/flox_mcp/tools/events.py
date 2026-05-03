"""explain_event — describe FLOX event struct fields.

Static schema map keyed by struct name. Updated when new event types are
added to the C-API spec (`include/flox/capi/flox_capi_spec.hpp`); a
lighter cousin of the auto-generated llms.md, focused on AI-grounding
for "what's in this event" questions during code authoring.

The dict-shape matcher is heuristic — given an event dict, picks the
known struct whose fields best match.
"""
from __future__ import annotations

from typing import Optional


# Field shape: (name, type, units, description)
_SCHEMAS: dict[str, list[tuple[str, str, str, str]]] = {
    "FloxTradeData": [
        ("symbol", "uint32_t", "id", "Symbol ID assigned by the registry."),
        ("price_raw", "int64_t", "price * 1e8", "Trade price as fixed-point int64."),
        ("quantity_raw", "int64_t", "qty * 1e8", "Trade quantity as fixed-point int64."),
        ("is_buy", "uint8_t", "0=sell, 1=buy", "Aggressor side."),
        ("exchange_ts_ns", "int64_t", "ns since epoch", "Exchange-reported timestamp."),
    ],
    "FloxBookData": [
        ("symbol", "uint32_t", "id", "Symbol ID."),
        ("exchange_ts_ns", "int64_t", "ns since epoch", "Snapshot timestamp."),
        ("snapshot", "FloxBookSnapshot", "struct", "BBO snapshot — see FloxBookSnapshot."),
    ],
    "FloxBookSnapshot": [
        ("bid_price_raw", "int64_t", "price * 1e8", "Best bid; 0 if no bids."),
        ("bid_qty_raw", "int64_t", "qty * 1e8", "Best-bid size."),
        ("ask_price_raw", "int64_t", "price * 1e8", "Best ask; 0 if no asks."),
        ("ask_qty_raw", "int64_t", "qty * 1e8", "Best-ask size."),
        ("mid_raw", "int64_t", "price * 1e8", "(bid+ask)/2; 0 if either side empty."),
        ("spread_raw", "int64_t", "price * 1e8", "ask - bid; 0 if either side empty."),
    ],
    "FloxBarData": [
        ("symbol", "uint32_t", "id", "Symbol ID."),
        ("bar_type", "uint8_t", "enum", "0=Time, 1=Tick, 2=Volume, 3=Renko, 4=Range, 5=HeikinAshi."),
        ("close_reason", "uint8_t", "enum", "0=Threshold, 1=Gap, 2=Forced, 3=Warmup."),
        ("bar_type_param", "uint64_t", "—", "Interval ns / tick count / volume threshold per type."),
        ("open_raw", "int64_t", "price * 1e8", "Open price."),
        ("high_raw", "int64_t", "price * 1e8", "High."),
        ("low_raw", "int64_t", "price * 1e8", "Low."),
        ("close_raw", "int64_t", "price * 1e8", "Close."),
        ("volume_raw", "int64_t", "vol * 1e8", "Total volume in the bar."),
        ("buy_volume_raw", "int64_t", "vol * 1e8", "Buy-aggressor volume."),
        ("trade_count_raw", "int64_t", "count * 1e8", "Trade count, scaled."),
        ("start_time_ns", "int64_t", "ns since epoch", "Bar open time."),
        ("end_time_ns", "int64_t", "ns since epoch", "Bar close time."),
    ],
    "FloxSymbolContext": [
        ("symbol_id", "uint32_t", "id", "Symbol ID."),
        ("position_raw", "int64_t", "qty * 1e8", "Net position; signed."),
        ("avg_entry_price_raw", "int64_t", "price * 1e8", "VWAP of current position; 0 if flat."),
        ("last_trade_price_raw", "int64_t", "price * 1e8", "Last trade price seen."),
        ("last_update_ns", "int64_t", "ns since epoch", "Last context update."),
        ("book", "FloxBookSnapshot", "struct", "BBO snapshot at last update."),
    ],
    "FloxSignal": [
        ("order_id", "uint64_t", "id", "Strategy-emitted order ID."),
        ("symbol", "uint32_t", "id", "Symbol ID."),
        ("side", "uint8_t", "0=buy, 1=sell", "Order side."),
        ("order_type", "uint8_t", "enum", "0=market, 1=limit, 2=stop_market, 3=stop_limit, 4=tp_market, 5=tp_limit, 6=trailing_stop, 7=cancel, 8=cancel_all, 9=modify."),
        ("price", "double", "price", "Limit price; 0 for market."),
        ("quantity", "double", "qty", "Order size."),
        ("trigger_price", "double", "price", "Stop / TP trigger; 0 for unconditional."),
        ("trailing_offset", "double", "price", "Trailing-stop absolute offset."),
        ("trailing_bps", "int32_t", "bps", "Trailing-stop callback rate in basis points."),
        ("new_price", "double", "price", "Modify: updated price."),
        ("new_quantity", "double", "qty", "Modify: updated quantity."),
    ],
}


def _match_event_shape(event: dict) -> Optional[str]:
    """Pick the schema whose required fields are the best superset of `event`."""
    keys = set(event.keys())
    best: tuple[int, str] | None = None
    for name, fields in _SCHEMAS.items():
        field_names = {f[0] for f in fields}
        # Score: events with more matching fields and fewer extra keys win.
        matches = len(keys & field_names)
        if matches == 0:
            continue
        if best is None or matches > best[0]:
            best = (matches, name)
    return best[1] if best else None


def explain_event(type_name: Optional[str] = None,
                  event: Optional[dict] = None) -> str:
    if type_name is None and event is None:
        return ("Pass either `type_name` (e.g. 'FloxTradeData') or an "
                "`event` dict to introspect.")

    if type_name is None:
        type_name = _match_event_shape(event or {})
        if type_name is None:
            keys = sorted((event or {}).keys())
            return ("Could not match event shape against any known struct. "
                    f"Got keys: {keys}. Known structs: "
                    + ", ".join(sorted(_SCHEMAS.keys())))

    fields = _SCHEMAS.get(type_name)
    if fields is None:
        return (f"Unknown struct {type_name!r}. Known: "
                + ", ".join(sorted(_SCHEMAS.keys())))

    lines = [
        f"# {type_name}",
        "",
        "| Field | Type | Units | Description |",
        "|---|---|---|---|",
    ]
    for name, ty, units, desc in fields:
        lines.append(f"| `{name}` | `{ty}` | {units} | {desc} |")

    return "\n".join(lines)
