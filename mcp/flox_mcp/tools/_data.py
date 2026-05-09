"""Bundled-data loaders shared by the polyglot MCP tools.

Each helper returns parsed data straight from the wheel's
``flox_mcp/data/`` directory (or the repo-root sources when running
from a checkout). Loaders are cached so tool calls are cheap; cache
keys include the file's mtime so an in-place sync during development
is picked up without restarting the server.
"""
from __future__ import annotations

import functools
import json
import sqlite3
from pathlib import Path
from typing import Any, Optional


_PKG_ROOT = Path(__file__).resolve().parent.parent
_REPO_ROOT_GUESS = Path(__file__).resolve().parents[3]


def _data_path(name: str) -> Optional[Path]:
    """Resolve a bundled artifact path. Try the wheel-bundled location
    first, then the in-repo fallback (so MCP works in a dev checkout)."""
    bundled = _PKG_ROOT / "data" / name
    if bundled.exists():
        return bundled
    fallback = _REPO_ROOT_GUESS / "mcp" / "flox_mcp" / "data" / name
    if fallback.exists():
        return fallback
    return None


def _load_json(name: str) -> Optional[dict]:
    p = _data_path(name)
    if p is None:
        return None
    return json.loads(p.read_text())


@functools.lru_cache(maxsize=1)
def _ir_snapshot_cached(_mtime: float) -> Optional[dict]:
    return _load_json("ir.snapshot.json")


@functools.lru_cache(maxsize=1)
def _binding_manifest_cached(_mtime: float) -> Optional[dict]:
    return _load_json("binding_manifest.json")


@functools.lru_cache(maxsize=1)
def _examples_index_cached(_mtime: float) -> Optional[dict]:
    return _load_json("examples_index.json")


@functools.lru_cache(maxsize=1)
def _gotchas_cached(_mtime: float) -> Optional[dict]:
    return _load_json("gotchas.json")


def _mtime_of(name: str) -> float:
    p = _data_path(name)
    return p.stat().st_mtime if p is not None else 0.0


def load_ir_snapshot() -> Optional[dict]:
    return _ir_snapshot_cached(_mtime_of("ir.snapshot.json"))


def load_binding_manifest() -> Optional[dict]:
    return _binding_manifest_cached(_mtime_of("binding_manifest.json"))


def load_examples_index() -> Optional[dict]:
    return _examples_index_cached(_mtime_of("examples_index.json"))


def load_gotchas() -> Optional[dict]:
    return _gotchas_cached(_mtime_of("gotchas.json"))


def open_docs_db() -> Optional[sqlite3.Connection]:
    """Open the FTS5 docs index read-only. Returns None when the index
    is missing (an installer skipped the data extras)."""
    p = _data_path("docs.fts.sqlite")
    if p is None:
        return None
    uri = f"file:{p}?mode=ro&immutable=1"
    return sqlite3.connect(uri, uri=True)


def template_path(language: str, kind: str) -> Optional[Path]:
    """Path to a strategy scaffold template, or None if absent."""
    p = _data_path(f"templates/strategy/{language}/{kind}.tmpl")
    return p
