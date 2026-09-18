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

The module has two platform requirements, not one, and they are separable.

The **engine** needs a native 128-bit integer -- `venue::Amount`, the type
the ledger keeps money in -- and nothing else from the system. GCC, Clang and
clang-cl all have one. `cl` does not, and Microsoft has announced no plan to
add one, so `FLOX_BUILD_VENUE` turns itself off there and says why.

The **perimeter** needs POSIX sockets. `FLOX_VENUE_PERIMETER` controls it and
defaults off where they are absent:

```bash
# the engine without the network perimeter
cmake -S . -B build -DFLOX_BUILD_VENUE=ON -DFLOX_VENUE_PERIMETER=OFF
```

With the perimeter off the gateways, sessions, distribution and the control
and metrics servers are not built, their tests are excluded, and simdjson is
not fetched. The exclusion is derived from what each test includes rather
than from a list kept by hand -- a list was tried first and was wrong on the
first build.

What that costs is worth stating plainly: eighteen test binaries do not run,
and they cover the session layer, delivery, recovery over the wire and the
parser fuzz. The engine's own suite, including crash recovery, the
differential fuzz and the conservation fuzz, runs in full. CI builds this
configuration on every run and checks both halves of that claim: that the
engine's tests are present and the perimeter's are not.

One durability note. A checkpoint's rename is made durable on POSIX by
syncing the directory afterwards; Windows has no directory handle and orders
the rename itself, so `flox::fileio::syncDirectory` does the work on one and
nothing on the other. See `flox/util/file_io.h`.

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
