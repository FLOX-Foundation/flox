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

Python is the easiest starting point for backtesting — the `Engine` class runs signal batches in parallel C++ threads with the GIL released. Node.js makes more sense if your infrastructure is already JS.

Codon has nearly identical syntax to Python but compiles to native code. Start with Python, switch to Codon if you need it.

The embedded JS binding runs QuickJS inside the C++ process — no separate Node.js runtime. Useful for scripted rules and backtesting where spinning up an external runtime isn't practical.

For other languages, use the C API.
