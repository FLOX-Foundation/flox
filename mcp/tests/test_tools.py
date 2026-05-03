"""Unit tests for the framework-agnostic tool functions."""
from __future__ import annotations

import sys
from pathlib import Path

# Make `flox_mcp` importable when running pytest from the repo root.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from flox_mcp.tools import capi, errors, events, indicators, strategy


# ── lookup_error_code ─────────────────────────────────────────────────


def test_lookup_error_code_known():
    out = errors.lookup_error_code("E_SYM_001")
    assert "E_SYM_001" in out
    assert "Symbol" in out
    assert "is not registered" in out


def test_lookup_error_code_unknown():
    out = errors.lookup_error_code("E_DOES_NOT_EXIST_999")
    assert "No page" in out or "Unknown" in out or "Invalid" in out


def test_lookup_error_code_invalid_format():
    out = errors.lookup_error_code("not-a-code")
    assert "Invalid" in out


# ── list_capi_functions ───────────────────────────────────────────────


def test_list_capi_functions_default():
    out = capi.list_capi_functions(limit=5)
    assert "C-API functions" in out
    # First 5 listed entries should appear as table rows.
    rows = [l for l in out.splitlines() if l.startswith("| `flox_")]
    assert 1 <= len(rows) <= 5


def test_list_capi_functions_with_filter():
    out = capi.list_capi_functions(filter="indicator_ema")
    assert "flox_indicator_ema" in out


def test_list_capi_functions_no_match():
    out = capi.list_capi_functions(filter="absolutely_nothing_matches_this")
    # Header still present; no rows.
    rows = [l for l in out.splitlines() if l.startswith("| `flox_")]
    assert rows == []


# ── validate_strategy ─────────────────────────────────────────────────


def test_validate_strategy_ok():
    code = (
        "import flox_py\n"
        "class S:\n"
        "    def on_trade(self, ctx, trade):\n"
        "        pass\n"
    )
    out = strategy.validate_strategy(code)
    assert "OK" in out
    assert "on_trade" in out


def test_validate_strategy_missing_hook():
    code = "import flox_py\nx = 1\n"
    out = strategy.validate_strategy(code)
    assert "REVIEW" in out or "WARN" in out


def test_validate_strategy_forbidden_eval():
    code = "import flox_py\ndef on_trade(ctx, t):\n    eval('1+1')\n"
    out = strategy.validate_strategy(code)
    assert "REJECT" in out
    assert "eval" in out


def test_validate_strategy_syntax_error():
    out = strategy.validate_strategy("def on_trade(\n")
    assert "SyntaxError" in out


# ── explain_event ─────────────────────────────────────────────────────


def test_explain_event_by_type_name():
    out = events.explain_event(type_name="FloxTradeData")
    assert "FloxTradeData" in out
    assert "price_raw" in out
    assert "is_buy" in out


def test_explain_event_unknown_type():
    out = events.explain_event(type_name="NotAStruct")
    assert "Unknown" in out


def test_explain_event_dict_shape_match():
    # Keys uniquely identify FloxBarData
    e = {"open_raw": 0, "high_raw": 0, "low_raw": 0, "close_raw": 0,
         "bar_type": 0, "bar_type_param": 60_000_000_000}
    out = events.explain_event(event=e)
    assert "FloxBarData" in out


def test_explain_event_no_args():
    out = events.explain_event()
    assert "Pass either" in out


# ── list_indicators ───────────────────────────────────────────────────


def test_list_indicators():
    out = indicators.list_indicators()
    # When the .pyi is present in the repo we should at least see one
    # known indicator like SMA or EMA.
    assert "indicators" in out.lower()
    if "Could not locate" not in out:
        assert "SMA" in out or "EMA" in out


def test_list_indicators_filter():
    out = indicators.list_indicators(filter="ema")
    if "Could not locate" not in out:
        # filter='ema' should keep EMA, drop SMA
        assert "EMA" in out
