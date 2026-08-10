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

CI builds the `OFF` configuration on every run and checks that the full test
suite still passes with no venue code compiled.

## Platform support

Linux and macOS. The module is skipped on MSVC and clang-cl: the ledger keeps
money in `__int128`, and those compilers have no 128-bit integer type. A
Windows build configures with the module off and prints a status message.

## Dependencies

The venue links `${FLOX}` core and `simdjson` (pulled via FetchContent at
configure time, same pin as `connectors/`; the REST perimeter codec parses
with it). OpenSSL is needed by the TLS gateway alone; it is detected inside
`venue/CMakeLists.txt`, and without it the module still builds, skipping only
the TLS gateway test.

## Consuming it

```cmake
target_link_libraries(my_target PRIVATE flox::venue)
```

That brings the include prefix with it:

```cpp
#include "flox-venue/matching_engine.h"   // module
#include "flox-venue/matching_book.h"      // module (reference oracle book)
```

## Layout

```
venue/
  CMakeLists.txt          target flox::venue, gated by FLOX_BUILD_VENUE
  include/flox-venue/     public headers, namespace flox::venue
  src/                    the few non-header translation units
  tests/                  the verification corpus (registered into FLOX ctest)
  scripts/                sanitizer gate
  benchmarks/             book microbenchmark
```

Order-level book primitives (`flox/book/{resting_order,ladder_book}.h`) and
the shared utilities the venue needed (`flox/util/{crypto,wire,transport,
websocket,system_clock}.h`) live in core, because they are useful on their own
and carry no venue-specific policy. The map-based `MatchingBook` lives in the
module (`flox-venue/matching_book.h`): it is the correctness oracle the venue
matcher is fuzz-verified against, not a general-purpose book.
