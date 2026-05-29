"""
python/tests/test_option_chain.py — Deribit option chain loading and querying.

Builds a synthetic mirror of a few option instruments (no network), loads them
into a chain, and exercises parse / query / as-of / roll.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_option_chain.py
or against an installed module:
    python3 python/tests/test_option_chain.py
"""

import sys
import os
import gzip
import tempfile
import shutil
import unittest
from datetime import date
from pathlib import Path

build_dir = os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'python')
if os.path.isdir(build_dir):
    sys.path.insert(0, os.path.abspath(build_dir))

from flox_py.archives import deribit

_CSV_HEADER = ("trade_id,timestamp_ms,instrument,side,price,amount,"
               "mark_price,iv,index_price\n")

# (instrument, [rows])
_INSTRUMENTS = {
    "BTC-29MAR24-50000-C": [
        (200, 1_700_000_000_000, "BTC-29MAR24-50000-C", "buy", 0.05, 10.0, 0.0498, 0.55, 42_000.0),
        (201, 1_700_000_001_000, "BTC-29MAR24-50000-C", "sell", 0.051, 5.0, 0.0511, 0.56, 42_010.0),
    ],
    "BTC-29MAR24-50000-P": [
        (300, 1_700_000_000_500, "BTC-29MAR24-50000-P", "buy", 0.04, 8.0, 0.0399, 0.60, 42_005.0),
    ],
    "BTC-26APR24-55000-C": [
        (400, 1_700_000_002_000, "BTC-26APR24-55000-C", "buy", 0.03, 3.0, 0.0301, 0.62, 42_020.0),
    ],
}
_DAY = "2024-01-15"


def _build_mirror(root: Path) -> None:
    for inst, rows in _INSTRUMENTS.items():
        d = root / "option" / inst
        d.mkdir(parents=True, exist_ok=True)
        with gzip.open(d / f"{inst}-{_DAY}.csv.gz", "wt") as f:
            f.write(_CSV_HEADER)
            for r in rows:
                f.write(",".join(str(x) for x in r) + "\n")


class OptionChainTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="opt-chain-"))
        self.mirror = self.tmp / "mirror"
        self.out = self.tmp / "tapes"
        _build_mirror(self.mirror)

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_parse_instrument(self):
        u, e, k, o = deribit.parse_option_instrument("BTC-29MAR24-50000-C")
        self.assertEqual(u, "BTC")
        self.assertEqual(e, date(2024, 3, 29))
        self.assertEqual(k, 50000.0)
        self.assertEqual(o, "C")
        with self.assertRaises(ValueError):
            deribit.parse_option_instrument("BTC-PERPETUAL")

    def test_load_and_query(self):
        chain = deribit.load_option_chain(
            list(_INSTRUMENTS), _DAY, self.out, mirror=self.mirror,
        )
        self.assertEqual(len(chain), 3)
        self.assertEqual(chain.expiries(), [date(2024, 3, 29), date(2024, 4, 26)])
        self.assertEqual(chain.strikes(date(2024, 3, 29)), [50000.0])

        # Query by expiry/type.
        mar_calls = chain.query(expiry=date(2024, 3, 29), option_type="C")
        self.assertEqual(len(mar_calls), 1)
        self.assertEqual(mar_calls[0].instrument, "BTC-29MAR24-50000-C")

        # get() resolves a single contract and its tape is readable.
        import flox_py
        c = chain.get(date(2024, 3, 29), 50000.0, "P")
        self.assertIsNotNone(c)
        quotes = flox_py.DataReader(str(c.tape)).read_option_quotes_from(0)
        self.assertEqual(quotes.size, 1)
        self.assertAlmostEqual(quotes["iv_raw"][0] / 1e8, 0.60, places=6)

    def test_as_of_and_roll(self):
        chain = deribit.load_option_chain(
            list(_INSTRUMENTS), _DAY, self.out, mirror=self.mirror,
        )
        # Both expiries live before March.
        self.assertEqual(len(chain.as_of("2024-01-15")), 3)
        # After the March expiry only the April contract remains live.
        live_after_mar = chain.as_of("2024-03-30")
        self.assertEqual(len(live_after_mar), 1)
        self.assertEqual(live_after_mar.expiries(), [date(2024, 4, 26)])
        # Roll: the expiry a March position rolls into is April.
        self.assertEqual(chain.next_expiry_after("2024-03-29"), date(2024, 4, 26))
        self.assertIsNone(chain.next_expiry_after("2024-04-26"))


if __name__ == "__main__":
    unittest.main()
