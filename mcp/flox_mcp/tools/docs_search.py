"""docs_search — top-k snippets from the bundled FTS5 docs index.

The index is built by ``scripts/sync_mcp_data.py`` over a strict
allowlist of doc roots (``docs/bindings``, ``docs/how-to``,
``docs/tutorials``, ``docs/reference``, ``docs/explanation``,
``docs/errors``). Internal trackers (``.notes/``) and author-only
files (``CLAUDE.md``) are NEVER indexed — verified by a CI test.

Plain word lists are AND-matched (every token must appear in a doc
to count). To force exact-phrase ranking, wrap the phrase in double
quotes: ``"walk forward"``. The previous default was to phrase-quote
plain queries, but FTS5 phrase match is so strict that natural agent
queries (``"ccxt fetch_ohlcv historical"``) returned zero hits even
when every keyword appeared in a single doc.
"""
from __future__ import annotations

import re
from typing import Optional

from . import _data


# FTS5 reserved chars / boolean-operator syntax. If the user query
# contains any of these, we treat it as an explicit FTS5 expression
# and pass through unchanged.
_FTS_OPERATOR_HINTS = ('"', " OR ", " AND ", " NEAR(", " NOT ", "(", "*", ":")
_TOKEN_RE = re.compile(r"[A-Za-z0-9_]+")


def _normalize_query(q: str) -> str:
    """Make a user query safe + reasonably useful for FTS5.

    * If the query already uses FTS5 operators (``"phrase"``, ``OR``,
      ``AND``, ``NEAR``, ``NOT``, ``*``, parens, column-prefix), pass
      through unchanged.
    * Otherwise tokenize and AND the terms. ``ccxt fetch_ohlcv
      historical`` becomes ``ccxt AND fetch_ohlcv AND historical`` —
      every term must appear, but they can be in any order or
      distance apart, which matches what an agent typing a multi-word
      query actually wants.
    """
    q = q.strip()
    if not q:
        return q
    if any(op in q for op in _FTS_OPERATOR_HINTS):
        return q
    tokens = _TOKEN_RE.findall(q)
    if not tokens:
        return ""
    if len(tokens) == 1:
        return tokens[0]
    return " AND ".join(tokens)


def _excerpt(body: str, query: str, *, span: int = 240) -> str:
    """Return a short window around the first query hit."""
    text = body.replace("\r\n", "\n").replace("\r", "\n")
    needle = query.strip().strip('"').lower()
    if not needle:
        return text[:span].replace("\n", " ").strip()
    idx = text.lower().find(needle.split()[0]) if needle else -1
    if idx < 0:
        return text[:span].replace("\n", " ").strip()
    start = max(0, idx - span // 2)
    end = min(len(text), start + span)
    return text[start:end].replace("\n", " ").strip()


def docs_search(query: str, k: int = 5) -> str:
    if not isinstance(query, str) or not query.strip():
        return "docs_search: `query` is required and must be a non-empty string."
    if not isinstance(k, int) or k <= 0:
        return "docs_search: `k` must be a positive integer."
    if k > 25:
        # Keep responses bounded — agents that need more should narrow.
        k = 25

    db = _data.open_docs_db()
    if db is None:
        return ("docs_search: bundled docs.fts.sqlite missing. "
                "Reinstall flox-mcp (it ships in the wheel).")

    try:
        fts_query = _normalize_query(query)
        if not fts_query:
            return f"docs_search: query {query!r} normalised to empty."
        try:
            rows = db.execute(
                "SELECT path, title, body, "
                "rank "
                "FROM docs WHERE docs MATCH ? "
                "ORDER BY rank LIMIT ?",
                (fts_query, k),
            ).fetchall()
        except Exception as exc:
            return (
                f"docs_search: FTS5 rejected the query `{fts_query}`: "
                f"{type(exc).__name__}: {exc}"
            )
    finally:
        db.close()

    if not rows:
        return (
            f"# docs_search: no matches for `{query}`\n\n"
            f"Tried FTS expression `{fts_query}`. Try a single keyword, "
            f"or a quoted phrase like `\"walk forward\"`."
        )

    lines = [f"# docs_search: top {len(rows)} match(es) for `{query}`", ""]
    for path, title, body, rank in rows:
        lines.append(f"## `{path}` — {title}")
        lines.append("")
        lines.append(_excerpt(body, query))
        lines.append("")
        lines.append(f"_score: {rank:.3f}_")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"
