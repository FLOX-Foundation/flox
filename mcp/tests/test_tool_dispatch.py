"""The 38 tool schemas and the dispatch chain must agree.

No test imported `flox_mcp.server` at all: CI ran `pytest mcp/tests/` without
installing the `mcp` package, so `server.py` was unimportable and both halves
of the tool surface went unchecked -- the 38 `Tool(...)` schemas returned by
`list_tools`, and the ~220-line if/elif chain in `_call_tool` that has to
match them name for name.

That is the same shape as the defects the binding audit found: helpers tested,
wiring untested. A tool advertised in `list_tools` but missing from the chain
answers every call with "unknown tool: <name>" -- an agent sees the tool, calls
it, and gets a string that reads like a bug in its own prompt.

These tests walk the real registered handlers rather than calling the module
functions directly, because the wiring is the thing under test.
"""

from __future__ import annotations

import asyncio
from typing import Any

import pytest

mcp_types = pytest.importorskip(
    "mcp.types", reason="needs the `mcp` package (declared in mcp/pyproject.toml)"
)

from flox_mcp.server import build_server  # noqa: E402


def _handlers() -> tuple[Any, Any]:
    # mcp 2.0 stores handlers by method string in HandlerEntry records, keyed
    # off the wire method rather than the request class.
    server = build_server()
    return (server.get_request_handler("tools/list").handler,
            server.get_request_handler("tools/call").handler)


def _tool_names() -> list[str]:
    list_tools, _ = _handlers()

    async def go() -> list[str]:
        res = await list_tools(None, None)
        return [t.name for t in res.tools]

    return asyncio.run(go())


def _call(name: str, arguments: dict[str, Any] | None = None) -> str:
    _, call_tool = _handlers()

    async def go() -> str:
        params = mcp_types.CallToolRequestParams(name=name, arguments=arguments or {})
        res = await call_tool(None, params)
        parts = res.content
        return "\n".join(getattr(p, "text", "") for p in parts)

    return asyncio.run(go())


def test_tools_are_advertised() -> None:
    names = _tool_names()
    assert len(names) >= 30, f"only {len(names)} tools advertised"
    assert len(names) == len(set(names)), "duplicate tool names in list_tools"


@pytest.mark.parametrize("name", _tool_names())
def test_every_advertised_tool_is_reachable_in_the_dispatch(name: str) -> None:
    """No advertised tool may fall through to the unknown-tool branch.

    Called with no arguments on purpose: a tool needing arguments raises
    inside its branch and `_call_tool` turns that into a "flox-mcp error: ..."
    string, which still proves the branch was reached. Only "unknown tool"
    means the name never made it into the chain.
    """
    text = _call(name)
    assert not text.startswith("unknown tool:"), (
        f"{name} is advertised by list_tools but has no branch in _call_tool")


def test_an_unregistered_name_reports_unknown_tool() -> None:
    # The negative control: if this stopped saying "unknown tool", the check
    # above would pass for every name and prove nothing.
    assert _call("definitely_not_a_flox_tool").startswith("unknown tool:")


def test_dispatch_never_raises_on_missing_arguments() -> None:
    """Every branch must fail as a message, not as an exception.

    An agent calling a tool with the wrong arguments should get text back; a
    raised exception crosses the stdio boundary and kills the session.
    """
    for name in _tool_names():
        text = _call(name)
        assert isinstance(text, str) and text, f"{name} returned no text"
