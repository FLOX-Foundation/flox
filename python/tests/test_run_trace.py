"""Round-trip and metadata tests for the .floxrun TraceRecorder/Reader bindings."""

from __future__ import annotations

import shutil
import tempfile
from pathlib import Path

import pytest

import flox_py
from flox_py._flox_py import (
    FillLiquidity,
    OrderEventKind,
    TapeRef,
    TraceReader,
    TraceRecorder,
)


def _scratch(tag: str) -> Path:
    p = Path(tempfile.gettempdir()) / f"flox_run_pytest_{tag}"
    if p.exists():
        shutil.rmtree(p)
    return p


def test_round_trip_signal_order_fill() -> None:
    out = _scratch("roundtrip")
    refs = [
        TapeRef(
            "BTCUSDT.floxlog",
            "sha256:abc",
            1_700_000_000_000_000_000,
            1_700_000_000_500_000_000,
        )
    ]
    rec = TraceRecorder(
        str(out),
        strategy_id="ratio-cross",
        strategy_hash="sha256:test",
        run_started_ns=1_700_000_000_000_000_000,
        tape_refs=refs,
    )
    rec.write_signal(
        run_ts_ns=1_700_000_000_100_000_000,
        feed_ts_ns=1_700_000_000_099_000_000,
        signal_id=42,
        flags=flox_py._flox_py.SIGNAL_FLAG_ENTER,
        strength_raw=75_000_000,
        name="ratio-cross",
        symbol_ids=[1, 2],
        payload=b'{"src":"ETH","dst":"BTC"}',
    )
    rec.write_order_event(
        run_ts_ns=1_700_000_000_200_000_000,
        feed_ts_ns=1_700_000_000_099_000_000,
        order_id=7,
        parent_signal_id=42,
        price_raw=50_000_00000000,
        qty_raw=100_000_000,
        symbol_id=1,
        event_kind=OrderEventKind.SUBMIT,
        side=0,
        order_type=1,
        flags=flox_py._flox_py.ORDER_FLAG_POST_ONLY,
        reason="",
    )
    rec.write_fill(
        run_ts_ns=1_700_000_000_300_000_000,
        feed_ts_ns=1_700_000_000_250_000_000,
        order_id=7,
        fill_id=12345,
        price_raw=50_000_00000000,
        qty_raw=100_000_000,
        fee_raw=50_000,
        symbol_id=1,
        side=0,
        liquidity=FillLiquidity.MAKER,
    )
    rec.set_run_ended_ns(1_700_000_000_400_000_000)
    rec.close()

    reader = TraceReader(str(out))
    assert reader.strategy_id == "ratio-cross"
    assert reader.run_ended_ns == 1_700_000_000_400_000_000
    assert reader.tape_refs[0]["path"] == "BTCUSDT.floxlog"

    sigs = reader.read_all_signals()
    assert len(sigs) == 1
    assert sigs[0]["signal_id"] == 42
    assert sigs[0]["name"] == "ratio-cross"
    assert sigs[0]["symbol_ids"] == [1, 2]
    assert sigs[0]["payload"] == b'{"src":"ETH","dst":"BTC"}'

    orders = reader.read_all_order_events()
    assert len(orders) == 1
    assert orders[0]["order_id"] == 7
    assert orders[0]["parent_signal_id"] == 42
    assert orders[0]["event_kind"] == int(OrderEventKind.SUBMIT)

    fills = reader.read_all_fills()
    assert len(fills) == 1
    assert fills[0]["fill_id"] == 12345
    assert fills[0]["liquidity"] == int(FillLiquidity.MAKER)

    shutil.rmtree(out)


def test_empty_recorder_writes_manifest_only() -> None:
    out = _scratch("empty")
    rec = TraceRecorder(str(out), strategy_id="empty")
    rec.close()
    reader = TraceReader(str(out))
    assert reader.read_all_signals() == []
    assert reader.read_all_order_events() == []
    assert reader.read_all_fills() == []
    shutil.rmtree(out)


def test_multi_symbol_signal_preserves_all_ids() -> None:
    out = _scratch("multisym")
    rec = TraceRecorder(str(out))
    rec.write_signal(
        run_ts_ns=1_000_000_000,
        name="pair-trade",
        symbol_ids=[10, 20, 30, 40],
        payload=b"",
    )
    rec.close()
    reader = TraceReader(str(out))
    sigs = reader.read_all_signals()
    assert len(sigs) == 1
    assert sigs[0]["symbol_ids"] == [10, 20, 30, 40]
    shutil.rmtree(out)


def test_order_count_in_chronological_order() -> None:
    out = _scratch("seq")
    rec = TraceRecorder(str(out))
    for i in range(50):
        rec.write_order_event(
            run_ts_ns=1_000_000_000 + i * 1_000,
            order_id=i,
            event_kind=OrderEventKind.SUBMIT,
            side=i % 2,
            order_type=1,
            symbol_id=i % 4,
        )
    rec.close()
    reader = TraceReader(str(out))
    orders = reader.read_all_order_events()
    assert len(orders) == 50
    for i, o in enumerate(orders):
        assert o["order_id"] == i
        assert o["event_kind"] == int(OrderEventKind.SUBMIT)


def test_unknown_path_raises() -> None:
    with pytest.raises(Exception):
        TraceReader("/tmp/does-not-exist-floxrun-aoeuhtns")
