# Language bindings

All bindings expose the same core model: strategy callbacks, order emission, position queries. The API shape is the same across languages.

| Language | Guide | Reference |
|---|---|---|
| Python | [Python](python.md) | [API reference](../reference/python/index.md) |
| Node.js | [Node.js](node.md) | [API reference](../reference/node/index.md) |
| Codon | [Codon](codon.md) | [API reference](../reference/codon/index.md) |
| JavaScript (embedded) | [JavaScript](javascript.md) | [API reference](../reference/quickjs/index.md) |
| C API | [C API](capi.md) | [reference](../reference/api/capi/flox_capi.md) |

## Which one to use

Python is the easiest starting point for backtesting — `BacktestRunner` for event-driven strategies, `Engine` for pre-built signal lists. Node.js makes more sense if your infrastructure is already JS.

Codon has nearly identical syntax to Python but compiles to native code. Start with Python, switch to Codon if you need it.

The embedded JS binding runs QuickJS inside the C++ process — no separate Node.js runtime. Useful for scripted rules and backtesting where spinning up an external runtime isn't practical.

For other languages, use the C API.

## AI-agent companion

[`flox-mcp`](https://pypi.org/project/flox-mcp/) is a Model Context
Protocol server. AI coding agents (Cursor, Claude Code, Cline) spawn
it locally and ask it about FLOX before generating code. It exposes
tools that resolve a symbol across surfaces (`lookup_symbol`),
enumerate one surface's exports (`list_bindings`), return a starter
strategy class that parses + validates (`scaffold_strategy`), pull
example code from the docs corpus (`get_example`), and search the
docs (`docs_search`). The package version is bumped in lockstep with
flox-py and the npm package, so the agent's view of the surface
matches what you installed. Setup is in
[the package README](https://github.com/FLOX-Foundation/flox/tree/main/mcp).

`lookup_symbol` and `list_bindings` cover **six** surfaces — `cpp`, `capi`,
`python`, `node`, `codon`, `quickjs` — down to individual methods, reported as
`Owner.method`. C++ is listed first in a resolution, because it is the engine
and every other surface wraps it; the row carries the header to include. So
`SimulatedExecutor` resolves in all of them, `BacktestConfig` resolves in C++
alone, and `run_csv` resolves as `BacktestRunner.run_csv`.

Note that `capi` is not C++: it is the flat `extern "C"` ABI built for FFI
consumers, so `BacktestRunner` appears there as the opaque handle
`FloxBacktestRunnerHandle`. Ask for `cpp` when you mean the engine's own
classes.
