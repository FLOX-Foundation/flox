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


def _call_full(name: str, arguments: dict[str, Any] | None = None) -> tuple[str, bool]:
    _, call_tool = _handlers()

    async def go() -> tuple[str, bool]:
        params = mcp_types.CallToolRequestParams(name=name, arguments=arguments or {})
        res = await call_tool(None, params)
        parts = res.content
        text = "\n".join(getattr(p, "text", "") for p in parts)
        return text, bool(res.is_error)

    return asyncio.run(go())


def _call(name: str, arguments: dict[str, Any] | None = None) -> str:
    text, _ = _call_full(name, arguments)
    return text


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


def test_call_tool_offloads_blocking_work_off_the_event_loop(monkeypatch) -> None:
    """A slow tool (record_data shells out; run_backtest and friends do
    real work) must not stall the asyncio event loop the whole server runs
    on -- every other in-flight request would stop making progress for the
    duration, not just the caller's own request.

    `record_data` is monkeypatched to a synchronous 0.3s `time.sleep` here
    (instead of exercising the real subprocess) so the test is fast and
    deterministic; the property under test is generic to any blocking call
    in `_dispatch_tool_sync`, not specific to subprocess plumbing.
    """
    import time

    from flox_mcp import server as server_module

    def blocking_record_data(*args: Any, **kwargs: Any) -> str:
        time.sleep(0.3)
        return "done"

    monkeypatch.setattr(server_module.record_data_tool, "record_data", blocking_record_data)

    _, call_tool = _handlers()

    async def go() -> float | None:
        t0 = time.monotonic()
        first_tick_at: list[float] = []

        async def ticker() -> None:
            for _ in range(3):
                await asyncio.sleep(0.02)
                if not first_tick_at:
                    first_tick_at.append(time.monotonic() - t0)

        params = mcp_types.CallToolRequestParams(
            name="record_data",
            arguments={
                "mode": "historical", "exchange": "x", "symbol": "y",
                "out_path": "z", "from_dt": "2020-01-01", "to_dt": "2020-01-02",
            },
        )
        await asyncio.gather(call_tool(None, params), ticker())
        return first_tick_at[0] if first_tick_at else None

    first_tick_elapsed = asyncio.run(go())
    assert first_tick_elapsed is not None, "the ticker task never ran at all"
    # record_data sleeps 0.3s. If _call_tool ran that inline on the event
    # loop instead of a worker thread, nothing else -- including this
    # ticker's first 20ms sleep -- gets a turn until the 0.3s is over. A
    # first tick landing well under that proves the loop stayed live.
    assert first_tick_elapsed < 0.15, (
        f"first ticker step landed at {first_tick_elapsed:.3f}s into a "
        "0.3s blocking call -- the event loop was stalled by record_data"
    )


def test_unregistered_tool_name_reports_iserror() -> None:
    _, is_error = _call_full("definitely_not_a_flox_tool")
    assert is_error is True


def test_exception_in_a_branch_sets_iserror() -> None:
    # lookup_error_code requires `code`; called bare it raises inside its
    # branch. The text-only checks above already prove this comes back as
    # "flox-mcp error: ...", not a crash -- this proves the *isError* flag
    # (which a client would actually branch on) agrees with that text.
    text, is_error = _call_full("lookup_error_code")
    assert text.startswith("flox-mcp error:")
    assert is_error is True


def test_control_tool_failure_sets_iserror(monkeypatch) -> None:
    """place_order (and the other control-plane mutating tools) used to
    return isError=false on every path, including a control server that
    rejected the order outright -- a client branching on isError alone
    would believe the order went through. `control.place_order` now
    reports (text, is_error) and the dispatcher must propagate it."""
    from flox_mcp import server as server_module

    monkeypatch.setattr(
        server_module.control, "place_order",
        lambda **kwargs: ("control server rejected the request", True),
    )
    text, is_error = _call_full("place_order", {
        "account": "a", "symbol": 1, "side": "buy", "qty": 1.0,
    })
    assert "rejected" in text
    assert is_error is True


def test_control_tool_success_leaves_iserror_false(monkeypatch) -> None:
    from flox_mcp import server as server_module

    monkeypatch.setattr(
        server_module.control, "place_order",
        lambda **kwargs: ('{"accepted": true}', False),
    )
    text, is_error = _call_full("place_order", {
        "account": "a", "symbol": 1, "side": "buy", "qty": 1.0,
    })
    assert "accepted" in text
    assert is_error is False
