"""The context quotes say "no quote" as None, and a price of 0.0 as 0.0.

``SymbolContext.best_bid`` / ``best_ask`` / ``mid_price`` and the strategy's
``best_bid(symbol)`` / ``best_ask(symbol)`` / ``mid_price(symbol)`` used to
write 0.0 for an empty side, the number a bid at exactly 0.0 also writes,
so a strategy could not tell the two apart and ``book_spread()`` guarded on
``> 0``, which threw away a real quote at or below zero. The C API's
snapshot now carries presence flags and the raw accessors have their
``_opt`` variants; this is the Python surface reading them.
"""

from __future__ import annotations

import unittest

import flox_py as flox


class Recorder(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.seen: list[dict] = []

    def on_book_update(self, ctx):
        self.seen.append(
            {
                "bid": ctx.best_bid,
                "ask": ctx.best_ask,
                "mid": ctx.mid_price,
                "spread": ctx.book_spread(),
                # The strategy-level accessors read the live book, not the
                # snapshot handed to the callback; the two must agree.
                "s_bid": self.best_bid(),
                "s_ask": self.best_ask(),
                "s_mid": self.mid_price(),
            }
        )


class SymbolContextQuotes(unittest.TestCase):
    def setUp(self):
        self.registry = flox.SymbolRegistry()
        self.sym = self.registry.add_symbol("test", "ZERO", tick_size=0.01)
        self.strategy = Recorder([self.sym])
        self.runner = flox.Runner(self.registry, lambda sig: None)
        self.runner.add_strategy(self.strategy)
        self.runner.start()

    def tearDown(self):
        self.runner.stop()

    def push(self, bids, asks, ts=1):
        self.runner.on_book_snapshot(
            self.sym,
            [p for p, _ in bids],
            [q for _, q in bids],
            [p for p, _ in asks],
            [q for _, q in asks],
            ts,
        )

    def last(self):
        self.assertTrue(self.strategy.seen, "on_book_update never fired")
        return self.strategy.seen[-1]

    def test_an_empty_book_reads_as_none_on_every_field(self):
        self.push([], [])
        seen = self.last()
        for key in ("bid", "ask", "mid", "spread", "s_bid", "s_ask", "s_mid"):
            self.assertIsNone(seen[key], f"{key} on an empty book should be None")

    def test_a_bid_at_exactly_zero_is_a_price_and_the_missing_ask_is_none(self):
        self.push([(0.0, 1.0)], [])
        seen = self.last()
        self.assertEqual(seen["bid"], 0.0)
        self.assertIsInstance(seen["bid"], float)
        self.assertEqual(seen["s_bid"], 0.0)
        self.assertIsNone(seen["ask"])
        self.assertIsNone(seen["s_ask"])
        self.assertIsNone(seen["mid"], "no mid without both sides")
        self.assertIsNone(seen["s_mid"])
        self.assertIsNone(seen["spread"], "no spread without both sides")

    def test_an_ask_at_exactly_zero_is_a_price_and_the_missing_bid_is_none(self):
        self.push([], [(0.0, 1.0)])
        seen = self.last()
        self.assertEqual(seen["ask"], 0.0)
        self.assertEqual(seen["s_ask"], 0.0)
        self.assertIsNone(seen["bid"])
        self.assertIsNone(seen["s_bid"])
        self.assertIsNone(seen["mid"])

    def test_a_book_straddling_zero_quotes_every_field(self):
        self.push([(-0.01, 1.0)], [(0.01, 1.0)])
        seen = self.last()
        self.assertAlmostEqual(seen["bid"], -0.01)
        self.assertAlmostEqual(seen["ask"], 0.01)
        self.assertEqual(seen["mid"], 0.0, "a mid of exactly 0.0 is a price")
        self.assertIsInstance(seen["mid"], float)
        self.assertAlmostEqual(seen["spread"], 0.02, msg="a spread across zero is a spread")
        self.assertAlmostEqual(seen["s_bid"], -0.01)
        self.assertAlmostEqual(seen["s_ask"], 0.01)
        self.assertEqual(seen["s_mid"], 0.0)

    def test_an_ordinary_book_quotes_floats(self):
        self.push([(100.0, 2.0)], [(100.05, 1.0)])
        seen = self.last()
        self.assertAlmostEqual(seen["bid"], 100.0)
        self.assertAlmostEqual(seen["ask"], 100.05)
        self.assertAlmostEqual(seen["mid"], 100.025)
        self.assertAlmostEqual(seen["spread"], 0.05)

    def test_a_side_that_goes_away_reads_as_none_again(self):
        self.push([(100.0, 2.0)], [(100.05, 1.0)], ts=1)
        self.push([(100.0, 2.0)], [], ts=2)
        seen = self.last()
        self.assertAlmostEqual(seen["bid"], 100.0)
        self.assertIsNone(seen["ask"], "the ask side emptied; a stale price must not survive")
        self.assertIsNone(seen["mid"])

    def test_the_accessors_answer_none_before_any_book(self):
        strategy = Recorder([self.sym])
        self.assertIsNone(strategy.best_bid())
        self.assertIsNone(strategy.best_ask())
        self.assertIsNone(strategy.mid_price())
        ctx = strategy.ctx()
        self.assertIsNone(ctx.best_bid)
        self.assertIsNone(ctx.book_spread())


if __name__ == "__main__":
    unittest.main()
