"""
python/tests/test_exchange_capabilities.py — ExchangeCapabilities mirrors
every C++ capability flag, so a Python executor can advertise what its
venue supports. Pins the binding for batch_submit: a field missing here
is exactly how a Python executor ends up unable to raise the flag.
"""

import os
import sys

build_dir = os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'python')
sys.path.insert(0, os.path.abspath(build_dir))

import lrvx


def test_batch_submit_defaults_false_and_roundtrips():
    caps = lrvx.ExchangeCapabilities()
    assert caps.batch_submit is False
    caps.batch_submit = True
    assert caps.batch_submit is True


def test_every_cpp_flag_has_a_python_mirror():
    expected = {
        "stop_market", "stop_limit", "take_profit_market", "take_profit_limit",
        "trailing_stop", "iceberg", "oco", "gtc", "ioc", "fok", "gtd",
        "post_only", "reduce_only", "close_position", "batch_submit",
    }
    have = {a for a in dir(lrvx.ExchangeCapabilities()) if not a.startswith("_")}
    missing = expected - have
    assert not missing, f"ExchangeCapabilities lost mirrors: {sorted(missing)}"
