"""Order-journey tracer surface smoke tests.

End-to-end emission semantics (queue position, timestamps, maker/taker)
are covered by the C++ unit tests in tests/test_order_journey_tracer.cpp.
These tests verify the Python class exists, can be constructed, and
returns the expected analytics shape.
"""
from __future__ import annotations

import flox_py as flox


def test_tracer_constructs_with_defaults() -> None:
    t = flox.OrderJourneyTracer()
    assert t.order_count() == 0
    assert t.record_count() == 0


def test_tracer_constructs_with_overrides() -> None:
    t = flox.OrderJourneyTracer(max_orders=1024, max_records_per_order=8,
                                sample_rate=0.5)
    assert t.order_count() == 0


def test_tracer_analytics_methods_exist() -> None:
    t = flox.OrderJourneyTracer()
    # All four analytics methods should be callable; on an empty trace
    # they return NaN-like values without raising.
    for name in (
        "median_ack_latency_ns",
        "median_time_to_first_fill_ns",
        "maker_fill_ratio",
        "cancel_race_loss_rate",
    ):
        getattr(t, name)()


def test_tracer_result_is_structured_numpy() -> None:
    t = flox.OrderJourneyTracer()
    arr = t.result()
    assert arr.dtype.names is not None
    assert "order_id" in arr.dtype.names
    assert "status" in arr.dtype.names
    assert "queue_ahead" in arr.dtype.names
    assert "submitted_at_ns" in arr.dtype.names
    assert "is_maker" in arr.dtype.names


def test_tracer_journey_returns_empty_for_unknown_order() -> None:
    t = flox.OrderJourneyTracer()
    arr = t.journey(99999)
    assert len(arr) == 0
