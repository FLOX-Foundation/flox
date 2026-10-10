"""Exchange archive importers — convert public historical tick data
into the lrvx tape format consumed by Engine, BacktestRunner, and
the tape aggregators.

Each exchange ships as its own submodule with a small shared spine:

  * ``aggtrades_to_lrvx`` / ``trades_to_lrvx`` — single-day
    convert.
  * ``range_to_lrvx`` — multi-day with optional HTTP mirror cache.
  * CLI: ``lrvx archive {binance | bybit | okx | bitget | deribit} ...``.

The downloaded zip / csv.gz files share a single on-disk cache rooted
at ``~/.lrvx/archive-cache`` (overridable via ``LRVX_ARCHIVE_CACHE``);
each exchange namespaces its files under
``<cache_root>/<exchange>/<...>/``.

Today ships ``binance`` (aggTrades + book products), ``bybit``,
``okx``, ``bitget``, and ``deribit`` (perp / future / option trades).
"""
from . import _cache, archive_reader, binance, bitget, bybit, deribit, okx

cache_root = _cache.cache_root
ArchiveReader = archive_reader.ArchiveReader
TradeEvent = archive_reader.TradeEvent

__all__ = [
    "ArchiveReader",
    "TradeEvent",
    "cache_root",
    "binance",
    "bitget",
    "bybit",
    "deribit",
    "okx",
]
