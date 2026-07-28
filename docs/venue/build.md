# Building with and without the venue

The venue is an optional module. If you only write strategies, backtests, or
connectors, you never pay for it.

## The flag

```bash
# with the venue (default)
cmake -S . -B build
cmake --build build

# without it — nothing venue-related is configured, compiled, or tested
cmake -S . -B build -DFLOX_BUILD_VENUE=OFF
cmake --build build
```

`FLOX_BUILD_VENUE` follows the repository's `FLOX_BUILD_*` convention (the flags
that gate optional build outputs; `FLOX_ENABLE_*` gates capabilities of the core
library). Turning it off removes:

- the `flox-venue` library target and its headers from the build,
- every venue test from `ctest`,
- `multi_agent_venue_demo` from the demo targets.

A `-DFLOX_BUILD_VENUE=OFF` build configures and passes its full test suite with
no venue code compiled at all — verify with `ctest` after building.

## What it costs when ON

Nothing beyond the module itself. The venue links only `${FLOX}` core: no new
third-party dependency is forced on you. The one heavy dependency in the module
— OpenSSL, needed by the TLS gateway — is detected inside `venue/CMakeLists.txt`
and is **optional**: without it the module still builds and only the TLS gateway
test is skipped, exactly the posture `connectors/` uses.

## Consuming it

```cmake
target_link_libraries(my_target PRIVATE flox::venue)
```

That brings the include prefix with it:

```cpp
#include "flox-venue/matching_engine.h"   // module
#include "flox/book/matching_book.h"      // core (order-level books live in core)
```

## Layout

```
venue/
  CMakeLists.txt          target flox::venue, gated by FLOX_BUILD_VENUE
  include/flox-venue/     public headers, namespace flox::venue
  src/                    the few non-header translation units
  tests/                  the verification corpus (registered into flox ctest)
  scripts/                sanitizer gate
  benchmarks/             book microbenchmark
  AUDIT-LOG.md            defects found and fixed while hardening the module
  DESIGN.md               the original build plan
```

Order-level books (`flox/book/{resting_order,matching_book,ladder_book}.h`) and
the shared utilities the venue needed (`flox/util/{crypto,wire,transport,
websocket,system_clock}.h`) live in **core**, not the module: they are useful on
their own and carry no venue-specific policy.
