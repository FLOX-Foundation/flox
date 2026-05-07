"""Lookahead-bias lint MCP tool. Wraps
``flox_py.lookahead.validate_strategy_no_lookahead``."""
from __future__ import annotations


def validate_strategy_no_lookahead(code: str) -> str:
    """Heuristic AST analysis. Returns JSON with
    ``{"ok": bool, "findings": [...]}``."""
    try:
        import flox_py.lookahead as la
    except Exception as exc:
        import json as _json
        return _json.dumps({
            "error": f"flox_py.lookahead unavailable: "
                     f"{type(exc).__name__}: {exc}",
        }, indent=2)
    return la.validate_strategy_no_lookahead(code)


__all__ = ["validate_strategy_no_lookahead"]
