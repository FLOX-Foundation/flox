"""The Codon context reads the optional quote accessors.

There is no Codon toolchain here to run the binding (CI compiles and runs
the examples), so this reads the modules: the context must take its quotes
from the ``_opt`` accessors, which return the presence flag and write the
price through a pointer, and must type the three accessors and the spread as
``Optional[float]``. The raw accessors answer 0 for an empty side, the number
a bid at exactly 0.0 also answers, so a context reading them cannot tell the
two apart.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CONTEXT = REPO_ROOT / "codon" / "flox" / "context.codon"
STRATEGY = REPO_ROOT / "codon" / "flox" / "strategy.codon"
PAIRS = REPO_ROOT / "codon" / "examples" / "pairs_strategy.codon"


def test_the_context_imports_the_optional_accessors_and_not_the_raw_ones() -> None:
    source = CONTEXT.read_text()
    for name in ("flox_best_bid_raw_opt", "flox_best_ask_raw_opt", "flox_mid_price_raw_opt"):
        assert re.search(rf"^from C import {name}\(cobj, u32, Ptr\[i64\]\) -> u8$", source, re.M), (
            f"context.codon does not declare {name} with the (handle, symbol, price_out) -> flag shape"
        )
    for name in ("flox_best_bid_raw", "flox_best_ask_raw", "flox_mid_price_raw"):
        assert not re.search(rf"\b{name}\(", source), (
            f"context.codon still calls {name}, which folds an empty side into a price of 0"
        )


def test_the_quote_accessors_are_optional_and_answer_none_on_a_clear_flag() -> None:
    source = CONTEXT.read_text()
    for method, c_name in (
        ("best_bid", "flox_best_bid_raw_opt"),
        ("best_ask", "flox_best_ask_raw_opt"),
        ("mid_price", "flox_mid_price_raw_opt"),
    ):
        body = re.search(
            rf"    def {method}\(self\) -> Optional\[float\]:\n(.*?)(?=\n    def )", source, re.S
        )
        assert body, f"{method} is not typed Optional[float]"
        text = body.group(1)
        assert f"{c_name}(self._handle, self.symbol_id, __ptr__(raw)) == u8(0)" in text, (
            f"{method} does not branch on the presence flag {c_name} returns"
        )
        assert "return None" in text, f"{method} never answers None"
        assert "float(raw) / float(SCALE)" in text, f"{method} does not scale the price it was handed"


def test_the_spread_is_none_unless_both_sides_quote() -> None:
    source = CONTEXT.read_text()
    body = re.search(r"    def book_spread\(self\) -> Optional\[float\]:\n(.*?)(?=\n    def )", source, re.S)
    assert body, "book_spread is not typed Optional[float]"
    assert "if bid is None or ask is None:" in body.group(1)
    assert "> 0.0" not in body.group(1), "the spread still guards on a positive price, which drops a quote at or below zero"


def test_the_strategy_level_accessors_carry_the_same_type() -> None:
    source = STRATEGY.read_text()
    for method in ("best_bid", "best_ask", "mid_price"):
        assert re.search(
            rf"    def {method}\(self, symbol: Optional\[str\] = None\) -> Optional\[float\]:", source
        ), f"Strategy.{method} is not typed Optional[float]"


def test_the_pairs_example_guards_on_none_not_on_zero() -> None:
    source = PAIRS.read_text()
    assert "if mid1 is None or mid2 is None:" in source
    assert "mid1 == 0.0" not in source, "the example still treats a mid of exactly 0.0 as no quote"
