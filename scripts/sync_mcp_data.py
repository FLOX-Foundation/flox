#!/usr/bin/env python3
"""scripts/sync_mcp_data.py

Copy canonical artifacts into the flox-mcp package data directory so the
installable package works without a repo checkout.

Sources (canonical):
- `.api/c-api.snapshot`              → mcp/flox_mcp/data/c-api.snapshot
- `docs/errors/E_*.md`               → mcp/flox_mcp/data/errors/

CI gate: run with `--check`; if any committed copy under
`mcp/flox_mcp/data/` differs from the canonical source, exit 1.

Usage:
    python3 scripts/sync_mcp_data.py            # write
    python3 scripts/sync_mcp_data.py --check    # exit 1 if stale
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_SNAPSHOT = REPO_ROOT / ".api" / "c-api.snapshot"
SRC_ERRORS_DIR = REPO_ROOT / "docs" / "errors"
DST_DATA = REPO_ROOT / "mcp" / "flox_mcp" / "data"


def _copy_if_different(src: Path, dst: Path, *, check_only: bool) -> bool:
    """Return True if the file IS in sync, False if it would change.

    In check_only mode, never writes.
    """
    if not src.exists():
        return True  # nothing to sync; not a drift
    src_bytes = src.read_bytes()
    if dst.exists() and dst.read_bytes() == src_bytes:
        return True
    if check_only:
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(src_bytes)
    return True  # synced


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--check", action="store_true",
                   help="don't write; exit 1 on drift")
    args = p.parse_args(argv)

    drifts: list[str] = []

    # 1. ABI snapshot
    dst = DST_DATA / "c-api.snapshot"
    if not _copy_if_different(SRC_SNAPSHOT, dst, check_only=args.check):
        drifts.append(str(dst.relative_to(REPO_ROOT)))

    # 2. Error catalog. We sync exactly the set of E_*.md files; anything
    # extra under mcp/flox_mcp/data/errors/ is reported as stale.
    dst_errors_dir = DST_DATA / "errors"
    src_pages = sorted(SRC_ERRORS_DIR.glob("E_*.md"))

    src_names = {p.name for p in src_pages}
    if dst_errors_dir.is_dir():
        for existing in dst_errors_dir.glob("*.md"):
            if existing.name not in src_names:
                if args.check:
                    drifts.append(str(existing.relative_to(REPO_ROOT)) + " (stale)")
                else:
                    existing.unlink()

    for src in src_pages:
        dst = dst_errors_dir / src.name
        if not _copy_if_different(src, dst, check_only=args.check):
            drifts.append(str(dst.relative_to(REPO_ROOT)))

    if drifts:
        print("::error::flox-mcp bundled data is out of sync:", file=sys.stderr)
        for d in drifts:
            print(f"  {d}", file=sys.stderr)
        print("Run: python3 scripts/sync_mcp_data.py", file=sys.stderr)
        return 1

    if args.check:
        print(f"OK: flox-mcp bundled data in sync.")
    else:
        n_pages = len(src_pages)
        print(f"synced: c-api.snapshot + {n_pages} error page(s) → "
              f"{DST_DATA.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
