"""Exchange archive importers — convert public historical tick data
into the floxlog tape format consumed by Engine, BacktestRunner, and
the tape aggregators.

Each exchange ships as its own submodule with a small shared spine:

  * ``aggtrades_to_floxlog`` / ``trades_to_floxlog`` — single-day
    convert.
  * ``range_to_floxlog`` — multi-day with optional HTTP mirror cache.
  * CLI: ``flox archive {binance | bybit | ...} ...``.

The downloaded zip / csv.gz files share a single on-disk cache rooted
at ``~/.flox/archive-cache`` (overridable via ``FLOX_ARCHIVE_CACHE``);
each exchange namespaces its files under
``<cache_root>/<exchange>/<...>/``.

Phase 1 ships ``binance`` (aggTrades + book products) and ``bybit``
(trades). OKX / Bitget / Deribit follow under the same conventions
when there is a research use case for them.
"""
from . import _cache, archive_reader, binance, bybit

cache_root = _cache.cache_root
ArchiveReader = archive_reader.ArchiveReader
TradeEvent = archive_reader.TradeEvent

__all__ = [
    "ArchiveReader",
    "TradeEvent",
    "cache_root",
    "binance",
    "bybit",
]
