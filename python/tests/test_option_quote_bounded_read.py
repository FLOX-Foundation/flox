"""DataReader.read_option_quotes_from must honor the reader's to_ns:
collect only in-range quotes and abort the walk past the bound, so a
bounded window costs O(prefix) — not O(tape) — in time and memory.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_option_quote_bounded_read.py
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import numpy as np  # noqa: E402

import flox_py  # noqa: E402

_BASE = 1_700_000_000_000_000_000
_HOUR = 3_600_000_000_000


def _write_quote_tape(tape: Path, n_hours: int) -> None:
    w = flox_py.DataWriter(str(tape), max_segment_mb=64, exchange_id=0,
                           compression="none")
    ts = np.array([_BASE + i * _HOUR for i in range(n_hours)], dtype=np.int64)
    n = len(ts)
    w.write_option_quotes(
        exchange_ts_ns=ts, recv_ts_ns=ts,
        mark_prices=np.full(n, 1.0), index_prices=np.full(n, 1.0),
        ivs=np.full(n, 0.5), open_interest=np.full(n, 1.0),
        symbol_ids=np.full(n, 7, dtype=np.uint32))
    w.close()


class BoundedOptionReadTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="opt-bounded-"))
        self.tape = self.tmp / "tape"
        self.tape.mkdir()
        _write_quote_tape(self.tape, 48)

    def _hours(self, arr) -> list:
        return sorted(int(x) for x in
                      (arr["exchange_ts_ns"].astype(np.int64) - _BASE) // _HOUR)

    def test_bounded_window(self) -> None:
        r = flox_py.DataReader(str(self.tape),
                               from_ns=int(_BASE + 10 * _HOUR),
                               to_ns=int(_BASE + 20 * _HOUR))
        out = r.read_option_quotes_from(int(_BASE + 10 * _HOUR))
        self.assertEqual(self._hours(out), list(range(10, 20)))

    def test_unbounded_unchanged(self) -> None:
        out = flox_py.DataReader(str(self.tape)).read_option_quotes_from(0)
        self.assertEqual(len(out), 48)
        self.assertEqual(self._hours(out), list(range(48)))

    def test_to_only(self) -> None:
        r = flox_py.DataReader(str(self.tape), to_ns=int(_BASE + 5 * _HOUR))
        out = r.read_option_quotes_from(0)
        self.assertEqual(self._hours(out), list(range(5)))

    def test_empty_window(self) -> None:
        r = flox_py.DataReader(str(self.tape),
                               from_ns=int(_BASE + 100 * _HOUR),
                               to_ns=int(_BASE + 101 * _HOUR))
        out = r.read_option_quotes_from(int(_BASE + 100 * _HOUR))
        self.assertEqual(len(out), 0)


if __name__ == "__main__":
    unittest.main()
