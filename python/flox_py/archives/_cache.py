"""Shared on-disk cache for downloaded exchange archives.

`~/.flox/archive-cache/` is the default mirror root for the
exchange-specific importers (`binance`, `bybit`, ...). Each module
namespaces its files under `<cache_root>/<exchange>/<...>/` so
re-running the same import is a no-op after the first download.

The cache has no auto-eviction today; it grows monotonically. Callers
that want to wipe it can `rm -rf` the directory at any time — the
download path is idempotent.
"""
from __future__ import annotations

import os
from pathlib import Path
from typing import Optional


_DEFAULT_CACHE_ENV = "FLOX_ARCHIVE_CACHE"


def cache_root(override: Optional[Path] = None) -> Path:
    """Resolve the shared archive cache root.

    Precedence: explicit ``override`` > ``FLOX_ARCHIVE_CACHE`` env var
    > ``~/.flox/archive-cache``. The directory is created on demand."""
    if override is not None:
        root = Path(override).expanduser()
    else:
        env = os.environ.get(_DEFAULT_CACHE_ENV)
        if env:
            root = Path(env).expanduser()
        else:
            root = Path.home() / ".flox" / "archive-cache"
    root.mkdir(parents=True, exist_ok=True)
    return root


__all__ = ["cache_root"]
